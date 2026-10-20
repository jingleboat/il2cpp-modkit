/*
 * ModKitTweaks · 易物三件套（手机端，纯 native 实现）
 * ================================================
 * ① 作价恒占优   GameItemPack.GetTotalValue / TradingWithNpcManager.ValueOffset
 *                → 成交两道门槛都过（PC 版行为照搬）
 * ② 显示池子     StartTrading 之后，把 NPC 的「击杀必掉 / 暗取池」内容**复制副本**
 *                塞进 NPC 背包，只给易物面板看；池子原件一个字节都不动。
 * ③ 确认搬空     ConfirmTrading 之后，把交易区残留 + NPC 背包剩余全部搬进队伍背包。
 *
 * 铁律（PC 版验证过，照搬）：
 *   · 搬运一律「新建实例 → 加进目标包 → 删原件」。
 *     RemoveItem 会先改原实例的 Stack，AddItem 又以 Stack>0 为前置，
 *     直接把同一实例加进另一个包会让两边都变空壳。
 *   · 只复制、不写 npc.StealBackPack / DropOnDeadBackPack —— 那两个包同时是
 *     暗取与死亡掉落的数据源。
 *   · 复制出来的副本被易物拿走要记台账，否则同一池子可以无限复制。
 */
#include "common.h"
#include "trade.h"
#include "hook.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#define MAX_COPIES 512
#define MAX_SEEN 512
#define LEDGER_SLOTS 4096

/* ------------------------------------------------------------------ 缓存 */
static void* k_item = nullptr;      /* GameItemInstance */
static void* k_ui = nullptr;        /* TradingWithNpcUI */
static void* k_ptm = nullptr;       /* PlayerTeamManager */

/* ------------------------------------------------------------------ 复制台账 */
struct PoolCopy { void* copy; int id; int drop; int stack0; };
static PoolCopy g_copies[MAX_COPIES];
static int g_ncopies = 0;

static void* g_npc = nullptr;       /* 本次交易的 NPC（拿背包用） */
static void* g_bag = nullptr;       /* 本次交易开始时的 NPC 背包 */

/* ------------------------------------------------------------------ 小工具 */
static inline bool okptr(const void* p) { return (uintptr_t)p > 0x10000 && ((uintptr_t)p & 7) == 0; }

static inline void* list_at(void* list) {
    /* List<T>：+0x10 = T[] , +0x18 = _size */
    return list ? *(void**)((char*)list + 0x10) : nullptr;
}
static inline int list_size(void* list) { return list ? *(int*)((char*)list + 0x18) : 0; }
static inline void* arr_item(void* arr, int i) {
    /* 引用类型数组：+0x18 = length，元素从 +0x20 起 */
    if (!arr) return nullptr;
    int n = *(int*)((char*)arr + 0x18);
    if (i < 0 || i >= n) return nullptr;
    return ((void**)((char*)arr + 0x20))[i];
}
static inline void* list_item(void* list, int i) { return arr_item(list_at(list), i); }

static inline int item_stack(void* it) { return okptr(it) ? *(int*)((char*)it + OFF_ITEM_STACK) : 0; }
static inline int item_tid(void* it) { return okptr(it) ? *(int*)((char*)it + OFF_ITEM_TID) : 0; }

static void* pack_contents(void* pack) {
    if (!okptr(pack)) return nullptr;
    return *(void**)((char*)pack + OFF_PACK_CONTENTS);
}

#define CALL(ret, rva, ...) ((ret (*)(__VA_ARGS__, void*))(g_base + (rva)))
#define CALL0(ret, rva) ((ret (*)(void*))(g_base + (rva)))

static bool pack_add(void* pack, void* item) {
    if (!okptr(pack) || !okptr(item)) return false;
    return CALL(bool, RVA_PACK_ADD_INDEP, void*, void*)(pack, item, nullptr);
}
static bool pack_remove(void* pack, void* item, int n) {
    if (!okptr(pack) || !okptr(item)) return false;
    return CALL(bool, RVA_PACK_REMOVE, void*, void*, int)(pack, item, n, nullptr);
}
static bool item_valid(void* item) {
    if (!okptr(item)) return false;
    return CALL(bool, RVA_ITEM_ISVALID, void*)(item, nullptr);
}
static void* item_new_by_id(int id, int stack) {
    if (!k_item) {
        k_item = find_class("WuLin", "GameItemInstance");
        if (k_item && A_class_init) A_class_init(k_item);
    }
    if (!k_item) return nullptr;
    void* it = A_object_new(k_item);
    if (!it) return nullptr;
    CALL(void, RVA_ITEM_CTOR_ID, void*, int, int, int)(it, id, stack, -1, nullptr);
    return it;
}
static void* item_new_by_key(void* key_str, int stack) {
    if (!k_item) {
        k_item = find_class("WuLin", "GameItemInstance");
        if (k_item && A_class_init) A_class_init(k_item);
    }
    if (!k_item || !okptr(key_str)) return nullptr;
    void* it = A_object_new(k_item);
    if (!it) return nullptr;
    CALL(void, RVA_ITEM_CTOR_KEY, void*, void*, int, int)(it, key_str, stack, -1, nullptr);
    return it;
}
static void* char_backpack(void* npc) { return CALL(void*, RVA_CHAR_BACKPACK, void*)(npc, nullptr); }
static void* char_droppack(void* npc) { return CALL(void*, RVA_CHAR_DROP_PACK, void*)(npc, nullptr); }
static void* char_stealpack(void* npc) { return CALL(void*, RVA_CHAR_STEAL_PACK, void*)(npc, nullptr); }
static void* char_templete(void* npc) { return CALL(void*, RVA_CHAR_TEMPLETE, void*)(npc, nullptr); }
static void* tr_npc(void) { return CALL0(void*, RVA_TR_GET_NPC)(nullptr); }
static void* tr_player_zone(void) { return CALL0(void*, RVA_TR_PLAYER_ZONE)(nullptr); }
static void* tr_npc_zone(void) { return CALL0(void*, RVA_TR_NPC_ZONE)(nullptr); }

static void* team_inventory() {
    if (!k_ptm) k_ptm = find_class("WuLin", "PlayerTeamManager");
    if (!k_ptm) return nullptr;
    void* ptm = find_object_of_type(k_ptm);
    if (!okptr(ptm)) return nullptr;
    void* inv = CALL(void*, RVA_TEAM_INVENTORY, void*)(ptm, nullptr);
    return okptr(inv) ? inv : nullptr;
}

static void refresh_ui() {
    if (!k_ui) k_ui = find_class("WuLin", "TradingWithNpcUI");
    if (!k_ui) return;
    void* ui = find_object_of_type(k_ui);
    if (!okptr(ui)) return;
    void* bags[2];
    bags[0] = *(void**)((char*)ui + OFF_UI_NPC_BAG);
    bags[1] = *(void**)((char*)ui + OFF_UI_NPC_TRADE);
    for (int i = 0; i < 2; ++i) {
        if (!okptr(bags[i])) continue;
        CALL(void, RVA_SHOWER_SETDIRTY, void*)(bags[i], nullptr);
    }
}

/* 数「真货」：Stack > 0 */
static int count_valid(void* pack) {
    void* list = pack_contents(pack);
    int n = list_size(list), c = 0;
    for (int i = 0; i < n; ++i) if (item_stack(list_item(list, i)) > 0) c++;
    return c;
}

/* 清掉 Stack<=0 的空壳 */
static int purge_invalid(void* pack) {
    void* list = pack_contents(pack);
    if (!list) return 0;
    /* 快照，边遍历边删会动 list */
    void* snap[256];
    int n = list_size(list), m = 0;
    for (int i = 0; i < n && m < 256; ++i) {
        void* it = list_item(list, i);
        if (okptr(it)) snap[m++] = it;
    }
    int killed = 0;
    for (int i = 0; i < m; ++i) {
        void* it = snap[i];
        if (item_stack(it) > 0 && item_valid(it)) continue;
        if (pack_remove(pack, it, 1)) killed++;
        else if (pack_remove(pack, it, 0)) killed++;
    }
    return killed;
}

/* src → dst：新实例进 dst，再删 src 原件 */
static int transfer(void* src, void* dst) {
    if (!okptr(src) || !okptr(dst) || src == dst) return 0;
    void* list = pack_contents(src);
    if (!list) return 0;
    void* snap[256];
    int n = list_size(list), m = 0;
    for (int i = 0; i < n && m < 256; ++i) {
        void* it = list_item(list, i);
        if (okptr(it)) snap[m++] = it;
    }
    int moved = 0;
    for (int i = 0; i < m; ++i) {
        void* it = snap[i];
        int stack = item_stack(it);
        int tid = item_tid(it);
        if (stack <= 0 || tid <= 0) continue;
        void* c = item_new_by_id(tid, stack);
        if (!c || item_stack(c) <= 0) continue;
        if (!pack_add(dst, c)) continue;
        if (!pack_remove(src, it, stack)) { pack_remove(dst, c, item_stack(c)); continue; }
        moved++;
    }
    return moved;
}

/* ---------------------------------------------------------------- 台账
 * 语义：**池子里的东西只能白拿一次**。
 *   ① 每次开易物面板，把「这个 NPC 还没被拿走的池子物品」复制一份塞进 NPC 背包；
 *   ② 被玩家拿走（成交/搬空时进了队伍背包）→ 按 uid+iid+类别+物品id 记账；
 *   ③ 下次开易物，台账里有的直接跳过 —— 同一件不会无限生成。
 * key 里带 NPC 实例 id，所以不同 NPC 的同一件物品互不影响。
 * 想重置：删 files/trade-taken.txt。 */
static uint64_t g_ledger[LEDGER_SLOTS];   /* 0 = 空槽；哈希后强制非 0 */
static int g_ledger_loaded = 0;

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x ? x : 1;
}
static uint64_t copy_key(void* npc, int drop, int itemId) {
    uint64_t uid = 0, iid = 0;
    if (okptr(npc)) {
        uid = (uint32_t)CALL(int, RVA_CHAR_TPL_UID, void*)(npc, nullptr);
        iid = (uint32_t)CALL(int, RVA_CHAR_INSTANCE_ID, void*)(npc, nullptr);
    }
    return mix64(uid * 1000003ULL + iid * 31ULL + (drop ? 7 : 3) * 1000000007ULL +
                 (uint32_t)itemId);
}

static int ledger_has(uint64_t k) {
    for (int i = 0; i < LEDGER_SLOTS; ++i) {
        int slot = (int)((k + (uint64_t)i * 0x9E3779B97F4A7C15ULL) % LEDGER_SLOTS);
        if (g_ledger[slot] == 0) return 0;
        if (g_ledger[slot] == k) return 1;
    }
    return 0;
}
static int ledger_add(uint64_t k) {
    for (int i = 0; i < LEDGER_SLOTS; ++i) {
        int slot = (int)((k + (uint64_t)i * 0x9E3779B97F4A7C15ULL) % LEDGER_SLOTS);
        if (g_ledger[slot] == k) return 0;
        if (g_ledger[slot] == 0) { g_ledger[slot] = k; return 1; }
    }
    return 0;
}

static void ledger_load() {
    if (g_ledger_loaded) return;
    g_ledger_loaded = 1;
    int fd = open(LEDGER_PATH, O_RDONLY);
    if (fd < 0) { LOGI("台账: 无历史文件"); return; }
    char buf[4096];
    int n, total = 0;
    char line[128];
    int ln = 0;
    while ((n = (int)read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (ln > 0) {
                    line[ln] = 0;
                    /* 格式 uid:iid:s|d:itemId */
                    int uid = 0, iid = 0, item = 0;
                    char kind = 's';
                    if (sscanf(line, "%d:%d:%c:%d", &uid, &iid, &kind, &item) == 4) {
                        uint64_t k = mix64((uint64_t)(uint32_t)uid * 1000003ULL +
                                           (uint64_t)(uint32_t)iid * 31ULL +
                                           (uint64_t)(kind == 'd' ? 7u : 3u) * 1000000007ULL +
                                           (uint32_t)item);
                        if (ledger_add(k)) total++;
                    }
                    ln = 0;
                }
            } else if (ln < (int)sizeof(line) - 1) {
                line[ln++] = c;
            }
        }
    }
    close(fd);
    LOGI("台账: 载入 %d 条 → %s", total, LEDGER_PATH);
}

static void ledger_mark(void* npc, int drop, int itemId) {
    ledger_load();
    uint64_t k = copy_key(npc, drop, itemId);
    if (!ledger_add(k)) return;
    int fd = open(LEDGER_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    int uid = okptr(npc) ? CALL(int, RVA_CHAR_TPL_UID, void*)(npc, nullptr) : 0;
    int iid = okptr(npc) ? CALL(int, RVA_CHAR_INSTANCE_ID, void*)(npc, nullptr) : 0;
    char line[96];
    int n = snprintf(line, sizeof(line), "%d:%d:%c:%d\n", uid, iid, drop ? 'd' : 's', itemId);
    if (n > 0) write(fd, line, (size_t)n);
    close(fd);
}

/* ------------------------------------------------------- 往背包造「只给易物看」的副本 */
struct Seen { int id[MAX_SEEN]; int n; };
static bool seen_has(Seen* s, int id) {
    for (int i = 0; i < s->n; ++i) if (s->id[i] == id) return true;
    return false;
}
static void seen_add(Seen* s, int id) { if (s->n < MAX_SEEN) s->id[s->n++] = id; }

static void remember_copy(void* copy, int id, int drop, int stack0) {
    if (g_ncopies >= MAX_COPIES) return;
    g_copies[g_ncopies].copy = copy;
    g_copies[g_ncopies].id = id;
    g_copies[g_ncopies].drop = drop;
    g_copies[g_ncopies].stack0 = stack0;
    g_ncopies++;
}

/* 从池子原件里扣掉 count 个 tid（易物拿走了副本 → 原件也要少掉，否则暗取/摸尸还能再拿一份）。
 * 返回实际扣掉的数量。 */
static int pool_deduct(void* npc, int drop, int tid, int count) {
    void* pool = drop ? char_droppack(npc) : char_stealpack(npc);
    if (!okptr(pool) || tid <= 0 || count <= 0) return 0;
    void* list = pack_contents(pool);
    if (!list) return 0;
    int n = list_size(list);
    for (int i = 0; i < n; ++i) {
        void* it = list_item(list, i);
        if (item_tid(it) != tid || item_stack(it) <= 0) continue;
        int have = item_stack(it);
        int take = count < have ? count : have;
        if (!pack_remove(pool, it, take)) return 0;
        return take;
    }
    return 0;
}

static int copy_from_pool(void* src, void* dst, void* npc, int drop, Seen* seen) {
    const char* tag = drop ? "掉落" : "暗取";
    if (!okptr(src) || !okptr(dst) || src == dst) {
        if (drop) LOGI("池子: %s池不可用 src=%p dst=%p", tag, src, dst);
        return 0;
    }
    void* list = pack_contents(src);
    if (!list) return 0;
    void* snap[256];
    int n = list_size(list), m = 0;
    for (int i = 0; i < n && m < 256; ++i) {
        void* it = list_item(list, i);
        if (okptr(it)) snap[m++] = it;
    }
    int made = 0;
    for (int i = 0; i < m; ++i) {
        int tid = item_tid(snap[i]);
        int stack = item_stack(snap[i]);
        if (tid <= 0 || stack <= 0) {
            LOGI("池子: %s#%d tid=%d stack=%d 条目无效", tag, i, tid, stack);
            continue;
        }
        if (seen_has(seen, tid)) {
            LOGI("池子: %s#%d tid=%d 本次已复制过（跳过重复）", tag, i, tid);
            continue;
        }
        if (ledger_has(copy_key(npc, drop, tid))) {
            LOGI("池子: %s#%d tid=%d 已经拿走过（不再生成）", tag, i, tid);
            continue;
        }
        void* c = item_new_by_id(tid, stack);
        if (!c) { LOGI("池子: %s#%d tid=%d 新建实例失败", tag, i, tid); continue; }
        if (item_stack(c) <= 0 || !item_valid(c)) {
            LOGI("池子: %s#%d tid=%d 实例无效 stack=%d", tag, i, tid, item_stack(c));
            continue;
        }
        if (!pack_add(dst, c)) { LOGI("池子: %s#%d tid=%d 加进背包失败", tag, i, tid); continue; }
        LOGI("池子: %s#%d tid=%d x%d → 背包", tag, i, tid, stack);
        seen_add(seen, tid);
        remember_copy(c, tid, drop, stack);
        made++;
    }
    return made;
}

/* 模板里有的条目（池子里还没实例化的）→ 自己按 key/values 造副本 */
static int copy_from_template(void* dst, void* npc, int drop, Seen* seen) {
    const char* tag = drop ? "模板掉" : "模板取";
    void* tpl = char_templete(npc);
    if (!okptr(tpl) || !okptr(dst)) return 0;
    void* arr = *(void**)((char*)tpl + (drop ? OFF_CPL_DROP : OFF_CPL_STEAL));
    if (!arr) return 0;
    int n = *(int*)((char*)arr + 0x18);
    if (n <= 0) return 0;
    int made = 0;
    for (int i = 0; i < n; ++i) {
        void* kwv = ((void**)((char*)arr + 0x20))[i];
        if (!okptr(kwv)) continue;
        void* key = *(void**)((char*)kwv + OFF_KWV_KEY);
        if (!okptr(key)) { LOGI("池子: %s#%d key 为空", tag, i); continue; }
        int cnt = 1;
        void* vals = *(void**)((char*)kwv + OFF_KWV_VALUES);
        if (okptr(vals) && list_size(vals) > 0) {
            void* varr = list_at(vals);
            if (okptr(varr)) {
                int v = *(int*)((char*)varr + 0x20);
                if (v > 0) cnt = v;
            }
        }
        void* gii = item_new_by_key(key, cnt);
        if (!gii || !item_valid(gii) || item_stack(gii) <= 0) {
            char b[96];
            LOGI("池子: %s#%d key=%s cnt=%d 解析失败", tag, i, str_to_buf(key, b, sizeof(b)), cnt);
            continue;
        }
        int tid = item_tid(gii);
        if (tid <= 0) { LOGI("池子: %s#%d tid<=0 跳过", tag, i); continue; }
        if (seen_has(seen, tid)) {
            LOGI("池子: %s#%d tid=%d 本次已复制过（跳过重复）", tag, i, tid);
            continue;
        }
        if (ledger_has(copy_key(npc, drop, tid))) {
            LOGI("池子: %s#%d tid=%d 已经拿走过（不再生成）", tag, i, tid);
            continue;
        }
        if (!pack_add(dst, gii)) { LOGI("池子: %s#%d tid=%d 加进背包失败", tag, i, tid); continue; }
        LOGI("池子: %s#%d tid=%d x%d → 背包", tag, i, tid, cnt);
        seen_add(seen, tid);
        remember_copy(gii, tid, drop, item_stack(gii));
        made++;
    }
    return made;
}

static int tpl_len(void* npc, int drop) {
    void* tpl = char_templete(npc);
    if (!okptr(tpl)) return -1;
    void* arr = *(void**)((char*)tpl + (drop ? OFF_CPL_DROP : OFF_CPL_STEAL));
    return arr ? *(int*)((char*)arr + 0x18) : -1;
}

static void cleanup_copies() {
    if (g_ncopies == 0) { g_npc = nullptr; g_bag = nullptr; return; }
    int removed = 0;
    if (okptr(g_bag)) {
        for (int i = 0; i < g_ncopies; ++i) {
            void* c = g_copies[i].copy;
            int stack = item_stack(c);
            if (stack <= 0) continue;
            if (pack_remove(g_bag, c, stack)) removed++;
        }
    }
    LOGI("易物: 清掉显示副本 %d/%d", removed, g_ncopies);
    g_ncopies = 0;
    g_npc = nullptr;
    g_bag = nullptr;
}

/* ================================================================ hook 主体 */

/* ① 作价恒占优：两道门槛 */
static long long (*orig_total_value)(void*, float) = nullptr;

static long long hook_total_value(void* self, float rate) {
    long long v = orig_total_value ? orig_total_value(self, rate) : 0;
    if (!F_TRADE_WIN) return v;
    return v < 1 ? 1 : v;
}

static long long (*orig_value_offset)() = nullptr;

static long long hook_value_offset() {
    long long v = orig_value_offset ? orig_value_offset() : 0;
    if (!F_TRADE_WIN) return v;
    return v < 0 ? 0 : v;
}

/* ②③ StartTrading / ConfirmTrading / CancelTrading */
static void (*orig_tr_start)(void* npc) = nullptr;
static void (*orig_tr_confirm)() = nullptr;
static void (*orig_tr_cancel)() = nullptr;

/* 上一次异常关闭（没走 Cancel/Confirm）时交易区还留着东西，先退回，再清副本 */
static void trade_before_start() {
    cleanup_copies();

    void* old = tr_npc();
    void* bag = okptr(old) ? char_backpack(old) : nullptr;
    void* nz = tr_npc_zone();
    if (okptr(nz)) {
        int ghost = purge_invalid(nz);
        if (ghost) LOGI("易物: 清掉上次残留空壳 %d（NPC 区）", ghost);
        if (okptr(bag) && count_valid(nz) > 0) {
            int n = transfer(nz, bag);
            if (n) LOGI("易物: 上次残留 NPC 区 → 背包 %d 件", n);
        }
    }
    void* pz = tr_player_zone();
    if (okptr(pz)) {
        int ghost = purge_invalid(pz);
        if (ghost) LOGI("易物: 清掉上次残留空壳 %d（玩家区）", ghost);
        void* inv = team_inventory();
        if (okptr(inv) && count_valid(pz) > 0) {
            int n = transfer(pz, inv);
            if (n) LOGI("易物: 上次残留玩家区 → 队伍背包 %d 件", n);
        }
    }
}

static void trade_after_start(void* npcArg) {
    g_npc = npcArg ? npcArg : tr_npc();
    if (!okptr(g_npc)) { LOGI("易物: StartTrading 后拿不到 NPC"); return; }
    g_bag = char_backpack(g_npc);
    if (!okptr(g_bag)) { LOGI("易物: NPC 背包为空"); return; }

    if (!F_TRADE_POOLS) { LOGI("易物: 池子显示已关闭"); return; }

    int purged = purge_invalid(g_bag);
    if (purged) LOGI("易物: 清掉 NPC 背包空壳 %d", purged);
    int before = count_valid(g_bag);

    void* drop = char_droppack(g_npc);
    void* steal = char_stealpack(g_npc);
    char nb[64];
    str_to_buf(CALL(void*, RVA_CHAR_TPL_NAME, void*)(g_npc, nullptr), nb, sizeof(nb));

    LOGI("易物: NPC=%s 背包=%d 击杀池=%d 暗取池=%d 模板掉=%d 模板取=%d", nb, before,
         count_valid(drop), count_valid(steal), tpl_len(g_npc, 1), tpl_len(g_npc, 0));

    ledger_load();
    Seen seen;
    seen.n = 0;
    int a = copy_from_pool(drop, g_bag, g_npc, 1, &seen);
    int b = copy_from_pool(steal, g_bag, g_npc, 0, &seen);
    int c = copy_from_template(g_bag, g_npc, 1, &seen);
    int d = copy_from_template(g_bag, g_npc, 0, &seen);
    if (a + b + c + d > 0)
        LOGI("易物: 副本 掉落池%d 暗取池%d 模板掉%d 模板取%d（池子原件未动）", a, b, c, d);
    else
        LOGI("易物: 没有可复制的池子内容");

    refresh_ui();
    LOGI("易物: 背包 %d → %d", before, count_valid(g_bag));
}

static void trade_after_confirm() {
    if (!F_TRADE_TAKEALL) { LOGI("易物: 搬空已关闭"); cleanup_copies(); return; }

    void* inv = team_inventory();
    if (!okptr(inv)) { LOGE("易物: 找不到队伍背包，跳过搬空"); cleanup_copies(); return; }
    int before = count_valid(inv);

    void* pz = tr_player_zone();
    void* nz = tr_npc_zone();
    if (okptr(pz)) purge_invalid(pz);
    if (okptr(nz)) purge_invalid(nz);

    int a = okptr(pz) ? transfer(pz, inv) : 0;
    int b = okptr(nz) ? transfer(nz, inv) : 0;
    int c = 0;
    void* bag = okptr(g_npc) ? char_backpack(g_npc) : g_bag;
    if (okptr(bag)) {
        purge_invalid(bag);
        c = transfer(bag, inv);
    }

    /* 副本被拿走 → ① 记台账（以后不再给这个 NPC 造同一件）
     *              ② 从池子原件里扣掉同样数量 —— 否则暗取/摸尸还能再拿一份 */
    int marked = 0, deducted = 0;
    for (int i = 0; i < g_ncopies; ++i) {
        void* cp = g_copies[i].copy;
        if (!okptr(cp)) continue;
        int left = item_stack(cp);
        int took = g_copies[i].stack0 - (left > 0 ? left : 0);
        if (took <= 0) continue;
        if (!okptr(g_npc)) continue;
        int got = pool_deduct(g_npc, g_copies[i].drop, g_copies[i].id, took);
        ledger_mark(g_npc, g_copies[i].drop, g_copies[i].id);
        marked++;
        deducted += got;
        LOGI("池子: %s tid=%d 拿走%d → 台账+1 池子扣%d", g_copies[i].drop ? "掉落" : "暗取",
             g_copies[i].id, took, got);
    }

    int after = count_valid(inv);
    LOGI("易物: 搬空 玩家区%d NPC区%d NPC背包%d 记账%d 扣池%d | 队伍背包 %d → %d%s", a, b, c,
         marked, deducted, before, after, (after > before ? "  OK" : "  <<< 没进来"));
    refresh_ui();
    g_ncopies = 0;
    g_npc = nullptr;
    g_bag = nullptr;
}

static void trade_after_cancel() {
    cleanup_copies();
}

static void hook_tr_start(void* npc) {
    trade_before_start();
    if (orig_tr_start) orig_tr_start(npc);
    trade_after_start(npc);
}

static void hook_tr_confirm() {
    if (orig_tr_confirm) orig_tr_confirm();
    trade_after_confirm();
}

static void hook_tr_cancel() {
    if (orig_tr_cancel) orig_tr_cancel();
    trade_after_cancel();
}

void trade_install() {
    hook_install("易物.ValueOffset", (void*)(g_base + RVA_TR_VALUE_OFFSET),
                 (void*)hook_value_offset, (void**)&orig_value_offset);
    hook_install("易物.GetTotalValue", (void*)(g_base + RVA_PACK_GET_TOTAL),
                 (void*)hook_total_value, (void**)&orig_total_value);
    hook_install("易物.StartTrading", (void*)(g_base + RVA_TR_START),
                 (void*)hook_tr_start, (void**)&orig_tr_start);
    hook_install("易物.ConfirmTrading", (void*)(g_base + RVA_TR_CONFIRM),
                 (void*)hook_tr_confirm, (void**)&orig_tr_confirm);
    hook_install("易物.CancelTrading", (void*)(g_base + RVA_TR_CANCEL),
                 (void*)hook_tr_cancel, (void**)&orig_tr_cancel);
}

void trade_hook_fire() {}
