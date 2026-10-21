/*
 * ModKitTweaks · Android 原生插件（主入口）
 * ==============================================================
 * 通道：il2cpp_init 首指令被改成 B 到插页里的引导桩，桩 dlopen 本库（注入方式自选）。
 * 本文件负责：
 *   1) 等 libil2cpp.so / il2cpp 运行时就绪，解析出全部 RVA 与 il2cpp C API
 *   2) 装 hook：
 *        易物三件套（trade.cpp）
 *        大地图 / 马车地图（IsMapEnable / IsPassIcon / HashSet.Contains / initIcon / cabIcon）
 *        剧情「需要伙伴」条件放宽（两个 CheckAllConnectNode 子类 → 走基类 + 按图名的规则）
 *        水面可走（water.cpp）
 *        游戏内面板（ui.cpp，每帧 tick 挂在 MainThreadSynchronizationContext.Update）
 *   3) 全部功能都由运行时开关控制，面板点一下即时生效
 */
#include "common.h"
#include "hook.h"
#include "trade.h"
#include "ui.h"
#include "probe.h"
#include "water.h"

#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <time.h>

/* 游戏内面板开关：1 = 带面板（默认展开、可折叠），0 = 只装功能不建 UI */
#define WULIN_PANEL 1

/* 排查探针：1 = 往 modkit.log 打水面/阻挡诊断，正式包改回 0 */
#define WULIN_PROBE 0

/* DynamicModifier 运行时懒加载兜底：实测不可用（见 install_tweaks 注释） */
#define WULIN_DM_LAZY 0

/* ------------------------------------------------------------------ 全局 */
uintptr_t g_base = 0;
void* g_i2cpp = nullptr;

volatile int F_TRADE_WIN = 1;
volatile int F_TRADE_POOLS = 1;
volatile int F_TRADE_TAKEALL = 1;
volatile int F_MAP = 1;
volatile int F_CAB = 1;
volatile int F_STORY = 1;   /* v44 起默认开（此前是 0，每次启动要手动点） */
volatile int F_WATER = 1;

fn_i2c_get A_domain_get = nullptr;
fn_i2c_asm A_get_assemblies = nullptr;
fn_i2c_img A_get_image = nullptr;
fn_i2c_cls A_class_from_name = nullptr;
fn_i2c_cls_ty A_class_get_type = nullptr;
fn_i2c_ty_obj A_type_get_object = nullptr;
fn_i2c_new A_object_new = nullptr;
fn_i2c_str A_string_new = nullptr;
fn_i2c_arr A_array_new = nullptr;
fn_i2c_clsinit A_class_init = nullptr;
fn_i2c_objcls A_obj_get_class = nullptr;
fn_i2c_clsname A_class_get_name = nullptr;

/* ------------------------------------------------------------------ 日志 */
void glog(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "modkit", "%s", buf);
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        size_t n = strlen(buf);
        buf[n] = '\n';
        write(fd, buf, n + 1);
        close(fd);
    }
}

/* ============================================================ 反射小工具 */
static void** g_imgs = nullptr;
static size_t g_nimg = 0;

static void cache_images() {
    if (g_imgs || !A_domain_get) return;
    void* dom = A_domain_get();
    if (!dom) return;
    size_t n = 0;
    void** asms = A_get_assemblies(dom, &n);
    if (!asms || !n) return;
    static void* imgs[512];
    if (n > 512) n = 512;
    for (size_t i = 0; i < n; ++i) imgs[i] = A_get_image(asms[i]);
    g_imgs = imgs;
    g_nimg = n;
}

void* find_class(const char* ns, const char* name) {
    cache_images();
    if (!g_imgs) return nullptr;
    for (size_t i = 0; i < g_nimg; ++i) {
        if (!g_imgs[i]) continue;
        void* k = A_class_from_name(g_imgs[i], ns, name);
        if (k) return k;
    }
    return nullptr;
}

void* class_type(void* klass) {
    if (!klass) return nullptr;
    if (A_class_init) A_class_init(klass);
    /* 注意：il2cpp_class_get_type 返回的是 const Il2CppType*（&klass->byval_arg），
     * 不是 System.Type。必须再用 il2cpp_type_get_object 包一层，
     * 否则 AddComponent(Type) / FindObjectOfType(Type) 会在 libil2cpp 里 null 解引用（fault 0x135）。 */
    const void* ty = A_class_get_type(klass);
    if (!ty || !A_type_get_object) return nullptr;
    return A_type_get_object((void*)ty);
}

void* find_object_of_type(void* klass) {
    void* t = class_type(klass);
    if (!t) return nullptr;
    typedef void* (*fn_find)(void*, void*);
    return ((fn_find)(g_base + RVA_FIND_OBJECT_TYPE))(t, nullptr);
}

const char* str_to_buf(void* il2str, char* buf, int cap) {
    if (!buf || cap <= 0) return "";
    buf[0] = 0;
    if (!il2str) return buf;
    int len = *(int*)((char*)il2str + 0x10);
    if (len <= 0) return buf;
    const uint16_t* c = (const uint16_t*)((char*)il2str + 0x14);
    int o = 0;
    for (int i = 0; i < len; ++i) {
        uint32_t u = c[i];
        if (u < 0x80) {
            if (o + 1 >= cap) break;
            buf[o++] = (char)u;
        } else if (u < 0x800) {
            if (o + 2 >= cap) break;
            buf[o++] = (char)(0xC0 | (u >> 6));
            buf[o++] = (char)(0x80 | (u & 0x3F));
        } else if (u >= 0xD800 && u <= 0xDBFF && i + 1 < len &&
                   c[i + 1] >= 0xDC00 && c[i + 1] <= 0xDFFF) {
            uint32_t cp = 0x10000 + ((u - 0xD800) << 10) + (c[++i] - 0xDC00);
            if (o + 4 >= cap) break;
            buf[o++] = (char)(0xF0 | (cp >> 18));
            buf[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            buf[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (o + 3 >= cap) break;
            buf[o++] = (char)(0xE0 | (u >> 12));
            buf[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
            buf[o++] = (char)(0x80 | (u & 0x3F));
        }
    }
    buf[o] = 0;
    return buf;
}

/* ====================================================== 大地图 / 马车地图 */
static int (*orig_contains)(void* set, int id, void* type) = nullptr;
static void* g_uimap = nullptr;
static void* g_data = nullptr;
static void* g_set_enable = nullptr;
static void* g_set_vis = nullptr;
static void* g_set_allow = nullptr;
static void* g_set_deny = nullptr;
static int g_map_log = 0;

static void resolve_save() {
    /* 这个元数据槽里放的是 MonoSingleton<UIWorldMapManager>.get_Instance 的泛型用法槽，
     * 按 MethodInfo 传给共享泛型实现就能拿到实例。v3 就是这么拿到存档数据的，别改。 */
    void** cell = *(void***)(g_base + VA_CLASS_CELL_UIMAP);
    if (!cell || !*cell) return;
    typedef void* (*fn_single)(void*);
    void* sm = ((fn_single)(g_base + RVA_SINGLETON_GET))(*cell);
    if (!sm) return;
    void* save = *(void**)((char*)sm + 0xD8);
    if (!save) return;
    void* data = *(void**)((char*)save + 0x20);
    if (!data) return;
    g_data = data;
    g_set_enable = *(void**)((char*)data + 0x90);
    g_set_vis = *(void**)((char*)data + 0xA0);
    g_set_allow = *(void**)((char*)data + 0xD0);
    g_set_deny = *(void**)((char*)data + 0xD8);
}

/* ---- 存档集合的「尽早解析」 ------------------------------------------------
 * 大地图 / 马车地图的解锁，全靠 hook_contains 认出这四个 HashSet 指针
 * （enable / visible / allow / deny）后强行返回 1/0。
 * 但 resolve_save() 原来只有两个调用点，都在马车地图的路径上
 * （hook_cabIcon ← UICabMapManager.UpdateIconShow、inject_mapdata）。
 *
 * 后果：新开档 / 没打开过马车地图时，四个指针一直是 NULL，
 * hook_contains 认不出任何集合 → 大地图照样锁定；
 * 只有等玩家打开一遍马车地图，resolve_save 跑过之后才「突然生效」。
 * 这就是「开放大地图需要打开一遍马车地图才生效」的成因。
 *
 * 修法：把 resolve_save() 提到所有用到它的入口，谁先来谁解析
 * （大地图打开 → initIcon；IsMapEnable / IsPassIcon；马车 UpdateIconShow）。
 * 解析本身很轻（读一个元数据槽 + 几次解引用），但为了不给热路径添开销，
 * 未就绪时最多每 200ms 试一次；就绪后只做一次指针比较。
 *
 * 注意：**不在 hook_contains 里解析**。那是共享泛型 HashSet<T>.Contains，
 * 存档/资源加载期间可能被后台线程调用，从工作线程碰 il2cpp（MonoSingleton.get_Instance、
 * 解引用 Unity 对象）会直接崩。上面几个入口都是主线程的 UI 方法，安全。 */
static uint64_t rule_now_ms(void);          /* 定义在剧情缓存那一节 */
static void char_mgr_reset(void);           /* 定义在「从未入队」那一节：换档后清角色管理器缓存 */

static int save_sets_ready(void) {
    return ((uintptr_t)g_set_enable > 0x10000) || ((uintptr_t)g_set_vis > 0x10000);
}

static uint64_t g_save_try_t = 0;
static int g_save_busy = 0;
static void* g_last_enable = nullptr;

/* force=1：忽略节流、无条件重新解析（大地图 initIcon / 马车 UpdateIconShow 这种低频入口用；
 *          换档后集合指针会变，靠它们刷新）。
 * force=0：只在未就绪时每 200ms 试一次，就绪后立刻返回（IsMapEnable/IsPassIcon 用）。 */
static int ensure_save(int force) {
    int ready = save_sets_ready();
    if (ready && !force) return 1;
    if (g_save_busy) return ready;

    uint64_t now = rule_now_ms();
    if (!force && g_save_try_t && now - g_save_try_t < 200) return ready;
    g_save_try_t = now;

    g_save_busy = 1;
    resolve_save();
    g_save_busy = 0;

    ready = save_sets_ready();
    if (ready && g_set_enable != g_last_enable) {
        LOGI("地图: 存档集合%s enable=%p vis=%p allow=%p deny=%p",
             g_last_enable ? "换档" : "已就绪", g_set_enable, g_set_vis, g_set_allow, g_set_deny);
        g_last_enable = g_set_enable;
        char_mgr_reset();       /* 换档后 GameCharacterManager 可能换实例，别用旧指针 */
    }
    return ready;
}

static int hook_contains(void* set, int id, void* type) {
    /* 这里只做集合比对，不解析存档（见上面注释：可能是后台线程） */
    if (set && F_MAP) {
        if (set == g_set_enable || set == g_set_vis) {
            if (g_map_log < 6) {
                LOGI("地图: %s-set id=%d -> 1", set == g_set_enable ? "enable" : "visible", id);
                g_map_log++;
            }
            return 1;
        }
    }
    if (set && F_CAB) {
        if (set == g_set_allow) return 1;
        if (set == g_set_deny) return 0;
    }
    return orig_contains ? orig_contains(set, id, type) : 0;
}

/* UIWorldMapManager.IsMapEnable(int) */
static bool (*orig_is_map_enable)(void* self, int id) = nullptr;
static bool hook_is_map_enable(void* self, int id) {
    if (!save_sets_ready()) ensure_save(0);
    bool r = orig_is_map_enable ? orig_is_map_enable(self, id) : false;
    return F_MAP ? true : r;
}

/* UICabMapManager.IsPassIcon(int) */
static bool (*orig_is_pass_icon)(void* self, int id) = nullptr;
static bool hook_is_pass_icon(void* self, int id) {
    if (!save_sets_ready()) ensure_save(0);
    bool r = orig_is_pass_icon ? orig_is_pass_icon(self, id) : false;
    return F_CAB ? true : r;
}

static void (*orig_initIcon)(void* self) = nullptr;
static void hook_initIcon(void* self) {
    if (self && !g_uimap) {
        g_uimap = self;
        LOGI("大地图: uimap=%p listWorldMap=%p", g_uimap, *(void**)((char*)self + 0xE0));
    }
    /* 先解析存档集合，再让游戏建图标 —— 这样第一遍建出来的图标就是解锁态，
     * 不用等玩家去开一次马车地图。 */
    int ready = ensure_save(1);
    if (orig_initIcon) orig_initIcon(self);
    /* 建图标时集合还没解析出来（存档刚载入等情形）：图标已按原版锁定态建好，
     * 而集合刚刚就绪，重建一遍（等价于 inject_mapdata 末尾那次 orig_initIcon）。 */
    if (!ready && orig_initIcon && ensure_save(1)) {
        static int rebuilt = 0;
        if (!rebuilt) {
            rebuilt = 1;
            orig_initIcon(self);
            LOGI("大地图: 集合晚于图标初始化，已重建图标列表");
        }
    }
}

/* 名字 → 数字（马车图标名就是 mapID） */
static int parse_id_from_name(void* str) {
    if (!str) return -1;
    int len = *(int*)((char*)str + 0x10);
    if (len <= 0 || len > 32) return -1;
    const uint16_t* c = (const uint16_t*)((char*)str + 0x14);
    int v = 0;
    for (int i = 0; i < len; ++i) {
        if (c[i] < '0' || c[i] > '9') return -1;
        v = v * 10 + (c[i] - '0');
    }
    return v;
}

static void inject_mapdata(void* self) {
    void* tr = self ? *(void**)((char*)self + 0xB8) : nullptr;
    if (!tr) return;
    if (!g_data) resolve_save();
    if (!g_data) { LOGI("马车: 存档对象未就绪，跳过 MapData 注入"); return; }

    int n = ((int (*)(void*, void*))(g_base + RVA_T_CHILD_COUNT))(tr, nullptr);
    int made = 0, ids[512], nid = 0;
    for (int i = 0; i < n && nid < 512; ++i) {
        void* child = ((void* (*)(void*, int, void*))(g_base + RVA_T_GET_CHILD))(tr, i, nullptr);
        if (!child) continue;
        void* obj = ((void* (*)(void*, void*))(g_base + RVA_GO_CHILD_OBJ))(child, nullptr);
        if (!obj) continue;
        void* nm = ((void* (*)(void*, void*))(g_base + RVA_GET_NAME))(obj, nullptr);
        int id = parse_id_from_name(nm);
        if (id <= 0 || id >= 1000000) continue;
        bool dup = false;
        for (int k = 0; k < nid; ++k) if (ids[k] == id) { dup = true; break; }
        if (dup) continue;
        ids[nid++] = id;
        ((void* (*)(void*, int))(g_base + RVA_GET_OR_CREATE_MAPDATA))(g_data, id);
        made++;
    }
    LOGI("马车: children=%d mapdata+%d", n, made);
    if (made > 0 && g_uimap && orig_initIcon) {
        orig_initIcon(g_uimap);
        LOGI("马车: 世界地图已重建 (+%d)", made);
    }
}

static void (*orig_cabIcon)(void* self) = nullptr;
static void hook_cabIcon(void* self) {
    ensure_save(1);
    if (orig_cabIcon) orig_cabIcon(self);
    if (F_CAB) inject_mapdata(self);
}

/* ============================================================== 剧情条件 */
static bool (*orig_check_joined)(void* self, int io, void* line) = nullptr;
static bool (*orig_check_partner)(void* self, int io, void* line) = nullptr;
typedef bool (*fn_check)(void*, int, void*);

/* 只跳过子类的「队友判断」，其余条件照查 --------------------------------
 *
 * 两个子类的 CheckAllConnectNode 反汇编出来是这么个结构：
 *     if (CheckAllConnectNodeAction == null) return ...;
 *     if (!CheckAllConnectNodeAction(self, io, line)) return false;   ← 队友在队判断
 *     return base.CheckAllConnectNode(io, line);                      ← 尾调用基类
 * 基类 ConditionNode.CheckAllConnectNode 做的事是：把 self 压进 line → CheckAllExceptSelf(io, line) → 弹出 self。
 * 所以「跳过子类、直接跑基类」= 只丢掉 CheckAllConnectNodeAction 这一条，
 * 连线上其它条件节点照常检查 —— 与桌面端 BepInEx 版的 jump-to-base 完全同义。
 *
 * 历史：v5 曾改成「hook 恒 return true」+ 每秒 20 次自旋闸门。恒真会让剧情 Hold 节点
 * 变成"条件随时满足"，StoryManager.ContinueArrange 与 HoldNode.OnActive 互相递归，
 * 切场景栈溢出闪退，那才是闸门存在的理由。走基类不改变 line 的语义、不会恒真，
 * 这个副作用不存在，闸门一并去掉。 */
static fn_check base_check_all = nullptr;

/* 下面两个函数定义在后面，这里先声明 */
static const char* obj_name(void* o, char* buf, int cap);
static int story_blacklisted(const char* g);

/* 主线和师父的故事线：固定按原版判断（不放宽） */
static const char* k_fixed_original[] = {
    "Fame-2",
    "Guide10099-1",                  /* 2026-10-21 追加：与 6/11/13… 同属师父线 */
    "Guide10099-6",  "Guide10099-11", "Guide10099-13",
    "Guide10099-15", "Guide10099-16", "Guide10099-18",
};

/* R10005-11 -> 10005；不是 "R数字-" 形式返回 -1 */
static int parse_r_id(const char* gn) {
    if (!gn || gn[0] != 'R') return -1;
    int v = 0, i = 1;
    while (gn[i] >= '0' && gn[i] <= '9' && i < 12) { v = v * 10 + (gn[i] - '0'); ++i; }
    if (i < 3) return -1;            /* 至少两位数字 */
    if (gn[i] != '-') return -1;     /* 必须是 R<id>- */
    return v;
}

/* 该角色是否从未入队：GameCharacterInstance.m_TeamMemberState(+0x130) == NULL。
 * 实测（老档）：苗彩蝶/曼陀罗/癞头癫 = NULL；猎户/铁匠/玩家 = 非 NULL（入过队）。
 *
 * 管理器实例缓存 60 秒。换档/重载后 GameCharacterManager 可能换实例，
 * 旧指针悬空会读到垃圾甚至崩，所以定期重找；换档时由 ensure_save() 主动清。 */
static void* g_char_mgr = nullptr;
static uint64_t g_char_mgr_t = 0;
#define CHAR_MGR_TTL_MS 60000

static void char_mgr_reset(void) { g_char_mgr = nullptr; g_char_mgr_t = 0; }

static void* char_mgr() {
    uint64_t now = rule_now_ms();
    if ((uintptr_t)g_char_mgr > 0x10000 && now - g_char_mgr_t < CHAR_MGR_TTL_MS) return g_char_mgr;
    /* GameCharacterManager 在 WuLin 命名空间下（dump 确认），namespace 传空会找不到 */
    void* k = find_class("WuLin", "GameCharacterManager");
    void* inst = k ? find_object_of_type(k) : nullptr;
    g_char_mgr = ((uintptr_t)inst > 0x10000) ? inst : nullptr;
    g_char_mgr_t = now;
    return g_char_mgr;
}

/* 判定缓存：条件节点 -> (是否按原版, 时间戳)，TTL 3 秒。
 * 入队状态会变，不能永久缓存；但条件检查是热路径，每次读图名+调 GetCharacter 会卡。 */
#define RULE_TTL_MS 3000
#define RULE_CACHE_N 256
static struct { void* node; int keep; uint64_t t; } g_rule_cache[RULE_CACHE_N];
static int g_rule_cache_i = 0;

static uint64_t rule_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

/* 2026-10-21 修正：拿不到实例 / 管理器未就绪时，旧实现返回 0（当作已入队 → 放宽）。
 * 新档里游戏还没创建大部分角色实例，判据于是集体失效 → 本命角色的个人剧情被放宽，
 * 不该触发的剧情提前触发（PC 端日志同源：`[nj] uid=10006 实例未创建`）。
 * 语义上「连实例都没有」必然没入过队，所以兜底只能是 1（不放宽，走原版）。
 * ⚠️ 这与 MapKit/CharKit 那类「查不到就放行」的兜底方向相反 —— 这里的放行等于放宽剧情。 */
static int g_nj_log = 0;

static int never_joined(int uid) {
    if (uid <= 0) return 0;

    void* m = char_mgr();
    if ((uintptr_t)m < 0x10000) {
        if (g_nj_log++ < 40) LOGI("[nj] uid=%d 管理器未就绪 → 按「从未入队」（不放宽）", uid);
        return 1;
    }
    typedef void* (*fn_getchar)(void*, int);
    void* c = ((fn_getchar)(g_base + RVA_GET_CHARACTER))(m, uid);
    if ((uintptr_t)c < 0x10000) {
        if (g_nj_log++ < 40) LOGI("[nj] uid=%d 实例未创建 → 从未入队（不放宽）", uid);
        return 1;
    }
    void* tms = *(void**)((char*)c + OFF_TEAM_STATE);
    int v = ((uintptr_t)tms < 0x10000) ? 1 : 0;
    if (g_nj_log++ < 40) LOGI("[nj] uid=%d ptr=%p tms=%p → neverJoined=%d", uid, c, tms, v);
    return v;
}

/* 这个「角色在队」条件节点是否要**按原版判断**（不放宽）。
 *
 * 规则（与 PC 端一致）：
 *   1) R{角色id}-xxx 这类角色剧情图：只有当条件里的 partnerID == 图名里的角色 id 时才走原版
 *      —— 该角色入队后才开启自己的故事线；条件是别的角色时照常放宽（用来触发其它彩蛋）。
 *   2) Fame-2 / Guide10099-6,11,13,15,16,18（主线与师父线）固定走原版。
 *   3) 手动名单 story_blacklisted() 仍然生效。
 *   4) 其余一律放宽。
 * 读不到图名时保守返回 0（放宽），避免误挡。 */
static int keep_original_for_uncached(void* condNode);

/* 带 TTL 缓存的入口：条件检查是热路径，若每次读图名 + 调 GetCharacter 会卡。
 * 入队状态会变，所以 TTL 3 秒，不能永久缓存。 */
static int keep_original_for(void* condNode) {
    if ((uintptr_t)condNode < 0x10000) return 0;

    uint64_t now = rule_now_ms();
    for (int i = 0; i < RULE_CACHE_N; ++i) {
        if (g_rule_cache[i].node == condNode) {
            if (now - g_rule_cache[i].t < (uint64_t)RULE_TTL_MS) return g_rule_cache[i].keep;
            break;
        }
    }

    int result = keep_original_for_uncached(condNode);

    g_rule_cache[g_rule_cache_i].node = condNode;
    g_rule_cache[g_rule_cache_i].keep = result;
    g_rule_cache[g_rule_cache_i].t = now;
    g_rule_cache_i = (g_rule_cache_i + 1) % RULE_CACHE_N;
    return result;
}

static int keep_original_for_uncached(void* condNode) {
    void* g = *(void**)((char*)condNode + OFF_NODE_GRAPH);
    if ((uintptr_t)g < 0x10000) return 0;

    char gb[128];
    const char* gn = obj_name(g, gb, sizeof(gb));
    if (!gn || !gn[0] || gn[0] == '?') return 0;

    for (size_t i = 0; i < sizeof(k_fixed_original) / sizeof(k_fixed_original[0]); ++i)
        if (strcmp(gn, k_fixed_original[i]) == 0) return 1;

    if (story_blacklisted(gn)) return 1;

    int selfId = parse_r_id(gn);
    if (selfId <= 0) return 0;                       /* 非角色剧情图：放宽 */

    int pid = *(int*)((char*)condNode + OFF_PARTNER_ID);
    if (pid != selfId) return 0;                     /* 条件是别的角色：放宽（彩蛋） */
    return never_joined(pid);                        /* 本命角色：仅「从未入队」时才挡 */
}

static bool hook_check_joined(void* self, int io, void* line) {
    if (F_STORY && base_check_all && !keep_original_for(self))
        return base_check_all(self, io, line);
    return orig_check_joined ? orig_check_joined(self, io, line) : false;
}

static bool hook_check_partner(void* self, int io, void* line) {
    if (F_STORY && base_check_all && !keep_original_for(self))
        return base_check_all(self, io, line);
    return orig_check_partner ? orig_check_partner(self, io, line) : false;
}

/* ---- ContinueArrange 递归看门狗 ------------------------------------------
 * 放宽「队友在队 / 有伙伴」之后，某些剧情图里的 Hold 节点会立刻满足：
 *   StoryManager.ContinueArrange <-> HoldNode.OnActive 在同一帧里互相递归，
 * 几千层后原生栈溢出，进程直接消失（PC 端实测：Windows 事件日志
 * Application Error id=1000，错误模块 GameAssembly.dll，异常代码 0xc00000fd）。
 *
 * 早先那两个 hook 里的「同一对象每秒 20 次」闸门不够用 —— 它按 self 分槽，
 * 递归时若每层命中的是不同条件实例，各自都能放行 20 次，总深度照样爆栈。
 * 这里改在递归的必经点 ContinueArrange 上按「真实调用栈深度」限深：
 * 超过 16 层就跳过这一层推进。深度用 __thread，下一帧从 0 开始，
 * 剧情继续往下走，不会把进度永久堵死。
 * ContinueArrange 栈帧 0xd0 字节，16 层约 3.3 KB，离 8 MB 的线程栈很远。 */
#define CA_MAX_DEPTH 16
static void (*orig_continue_arrange)(void* self, void* node, void* port) = nullptr;
static __thread int g_ca_depth = 0;
static int g_ca_warn = 0;

static const char* node_class_name(void* node) {
    if ((uintptr_t)node < 0x10000 || !A_obj_get_class || !A_class_get_name) return "?";
    void* k = A_obj_get_class(node);
    if ((uintptr_t)k < 0x10000) return "?";
    const char* n = A_class_get_name(k);
    return n ? n : "?";
}

static void hook_continue_arrange(void* self, void* node, void* port) {
    if (!F_STORY || !orig_continue_arrange) {
        if (orig_continue_arrange) orig_continue_arrange(self, node, port);
        return;
    }
    if (g_ca_depth >= CA_MAX_DEPTH) {
        if (g_ca_warn < 5) {
            g_ca_warn++;
            LOGI("剧情: ContinueArrange 已递归 %d 层，阻断这一层（条件放宽引起 Hold 空转）节点=%s",
                 g_ca_depth, node_class_name(node));
        }
        return;
    }
    g_ca_depth++;
    orig_continue_arrange(self, node, port);
    g_ca_depth--;
}

/* ---- Hold 只放行一次（治「剧情反复触发」） ------------------------------
 * HoldNode.checkSucessTimes(+0x70) 是游戏自己的「这个 Hold 已经成功过」标志：
 * addTimes() 置 1、clearTimes() 置 0。但 HoldNode.CheckCondition() 根本不读它 ——
 * 反汇编确认它只做条件端口 GetConnections → 逐个 CheckAllConnectNode。
 * 原版靠「条件为假」实现一次性；我们把条件放宽（走基类）之后，
 * times 早已被置 1，结果却照样 true，剧情于是反复触发。
 *
 * PC 端实测（2026-10-20 BepInEx 日志）：
 *   Hold "流程判断" times=0 result=true      ← 第一次，该放行
 *   Hold "流程判断" times=1 result=true      ← 已经成功过，还在放行 ← 就是这里
 *   Hold "流程判断" times=1 result=false     ← 原版本来就会挡住的
 * 且 frameHits 恒为 1，说明不是同帧空转（那条路径由上面的 ContinueArrange 看门狗管）。
 *
 * 对策：times 非 0 的 Hold，若这次仍被判为「可继续」，就压回 false。
 * 每个 Hold 只可能因为放宽而通过一次；游戏若用 clearTimes() 归零，之后还能再过一次，
 * 语义与原版一致。 */
static bool (*orig_hold_check)(void* self) = nullptr;
static int g_hold_once_warn = 0;

static const char* obj_name(void* o, char* buf, int cap) {
    buf[0] = 0;
    if ((uintptr_t)o < 0x10000) return "?";
    typedef void* (*fn_getname)(void*);
    void* s = ((fn_getname)(g_base + RVA_GET_NAME))(o);
    if ((uintptr_t)s < 0x10000) return "?";
    return str_to_buf(s, buf, cap);
}

/* 非拦截名单：名单内的剧情图，不做放宽、也不做 times 压制 —— 完全按原版判断。
 * 名单外：正常放宽（只放宽「角色在队」这一条），并对 times 非 0 的 Hold 做压制兜底。
 *
 * 留空 = 全部按「放宽 + 兜底压制」跑。
 * 已知候选（PC 端全量对比得出，存档里确实因为「角色在队」才通过的图）：
 *   R10049-0 / R10112-2 / R10136-5 / R20004-4 / Role3-1 / R10036-3 / R10060-2-1..6
 * 条目末尾带 * 时按前缀匹配（例如 "R10060-2-*"）。 */
static const char* k_story_blacklist[] = {
    "",
};
static const int k_story_blacklist_n =
    (int)(sizeof(k_story_blacklist) / sizeof(k_story_blacklist[0]));

static int story_blacklisted(const char* g) {
    if (!g || !g[0] || g[0] == '?') return 0;
    for (int i = 0; i < k_story_blacklist_n; ++i) {
        const char* e = k_story_blacklist[i];
        size_t L = strlen(e);
        if (L == 0) continue;
        if (e[L - 1] == '*') {
            if (strncmp(g, e, L - 1) == 0) return 1;
        } else if (strcmp(g, e) == 0) {
            return 1;
        }
    }
    return 0;
}

static bool hook_hold_check(void* self) {
    bool r = orig_hold_check ? orig_hold_check(self) : false;
    if (!F_STORY || !r) return r;
    if ((uintptr_t)self < 0x10000) return r;

    int times = *(int*)((char*)self + OFF_HOLD_TIMES);
    if (times == 0) return true;          /* 还没成功过：这次正常放行 */

    /* 哪些图不做 times 压制（交给原版条件判断）：
     *   - R{角色id}- 这类角色剧情图：入口条件已由 keep_original_for 精确控制，
     *     这里再压制会让「角色入队后」也只剩一次，违背原版语义；
     *   - 主线/师父线固定名单，同样按原版；
     *   - 手动名单。 */
    char gb[128];
    void* g = *(void**)((char*)self + OFF_NODE_GRAPH);
    const char* gn = ((uintptr_t)g > 0x10000) ? obj_name(g, gb, sizeof(gb)) : "?";
    if (parse_r_id(gn) > 0) return true;
    for (size_t i = 0; i < sizeof(k_fixed_original) / sizeof(k_fixed_original[0]); ++i)
        if (strcmp(gn, k_fixed_original[i]) == 0) return true;
    if (story_blacklisted(gn)) return true;

    /* 其余（非角色剧情图）：times 非 0 说明这个 Hold 已经成功过，再放行就是重复触发。
     * 这是兜底 —— 名单只能覆盖已知事件，未知的循环事件靠这里压住。 */
    if (g_hold_once_warn < 8) {
        g_hold_once_warn++;
        char nb[128];
        LOGI("[once] 剧情图 %s 的 Hold \"%s\" times=%d 已通过过，压制本次（防重复触发）",
             gn, obj_name(self, nb, sizeof(nb)), times);
    }
    return false;
}

/* ---- AssetBundle CRC 放行 ----
 * mod 的 *.bundle 被本工程改写过（数据移植 / UnityPy 重存），
 * catalog 里记录的 CRC 与实际文件对不上时 Unity 直接拒载：
 *   "CRC Mismatch. Provided xxxxxxxx, calculated xxxxxxxx from data.
 *    Will not load AssetBundle 'xxx.bundle'"
 * 这里把 AssetBundleRequestOptions.get_Crc() 恒返回 0（= 不校验），
 * 顺手把实例字段 m_Crc(+0x18) 清零，防止有人直接读字段。 */
static unsigned int (*orig_get_crc)(void* self) = nullptr;

static unsigned int hook_get_crc(void* self) {
    unsigned int v = orig_get_crc ? orig_get_crc(self) : 0;
    if (v) {
        LOGI("[mod] AssetBundle CRC %08x 放行（不校验）", v);
        *(unsigned int*)((char*)self + 0x18) = 0;
    }
    return 0;
}

/* get_Crc 常被 IL2CPP 内联，真正兜底的是这里：
 * AssetBundle.LoadFromFileAsync_Internal(string path, uint crc, ulong offset)
 * 把 crc 参数改成 0 = 不校验。 */
static void* (*orig_load_file_async)(void* path, unsigned int crc, unsigned long long offset) = nullptr;

static void* hook_load_file_async(void* path, unsigned int crc, unsigned long long offset) {
    if (crc) {
        LOGI("[mod] LoadFromFileAsync crc=%08x -> 0（跳过 CRC 校验）", crc);
        crc = 0;
    }
    return orig_load_file_async ? orig_load_file_async(path, crc, offset) : nullptr;
}

/* ---- DynamicModifier 懒加载 ----
 * mod 里的 DynamicModifier asset 没被提前注册进 cachedDynamicModifierDict 时，游戏会报
 *   "获取DynamicModifier失败：Item/ShiQuanDaBuWan"
 * 这里在查找失败后，自己按 "DynamicModifier/<key>.asset" 从 mod 的 Addressables 补加载。 */
static void* (*orig_get_modifier)(void* key) = nullptr;
static void* (*load_asset_from_mod)(void* type, void* key) = nullptr;
static void* g_dm_type = nullptr;

static void* hook_get_modifier(void* key) {
    void* r = orig_get_modifier ? orig_get_modifier(key) : nullptr;
    if (r) return r;

    char kbuf[192];
    str_to_buf(key, kbuf, sizeof(kbuf));
    if (!kbuf[0]) return nullptr;

    char full[256];
    snprintf(full, sizeof(full), "DynamicModifier/%s.asset", kbuf);

    if (!g_dm_type) {
        void* k = find_class("WuLin", "DynamicModifier");
        if (k) g_dm_type = class_type(k);
    }
    if (!g_dm_type || !load_asset_from_mod) {
        LOGI("[mod] GetModifier(\"%s\") 失败；懒加载不可用 type=%p fn=%p", kbuf,
             g_dm_type, (void*)load_asset_from_mod);
        return nullptr;
    }
    void* s = A_string_new(full);
    void* o = load_asset_from_mod(g_dm_type, s);
    LOGI("[mod] GetModifier(\"%s\") 未注册 → 懒加载 %s => %p", kbuf, full, o);
    return o;
}

/* ================================================= 暗取面板诊断 */
/* StealPanelUI.Init 里 ShowPack 传的是 npc.backPack(0x140)，
 * 而「暗取池」是 npc.stealBackPack(0x150) —— 两个不同的包。
 * 把两边内容都打进日志，才知道玩家在暗取面板里到底看得见哪一份。 */
static void (*orig_steal_init)(void* self) = nullptr;

static void dump_pack_tids(const char* tag, void* pack) {
    if ((uintptr_t)pack < 0x10000) { LOGI("暗取: %s = null", tag); return; }
    void* list = *(void**)((char*)pack + 0x48);
    if ((uintptr_t)list < 0x10000) { LOGI("暗取: %s 无 contents", tag); return; }
    void* arr = *(void**)((char*)list + 0x10);
    int n = *(int*)((char*)list + 0x18);
    int alen = ((uintptr_t)arr > 0x10000) ? *(int*)((char*)arr + 0x18) : 0;
    char buf[256];
    int o = 0, shown = 0;
    for (int i = 0; i < n && i < alen && shown < 12; ++i) {
        void* it = ((void**)((char*)arr + 0x20))[i];
        if ((uintptr_t)it < 0x10000) continue;
        int tid = *(int*)((char*)it + 0x1C);
        int st = *(int*)((char*)it + 0x20);
        if (o < (int)sizeof(buf) - 24)
            o += snprintf(buf + o, sizeof(buf) - o, "%d/%d ", tid, st);
        shown++;
    }
    buf[o] = 0;
    LOGI("暗取: %s n=%d [%s]", tag, n, buf);
}

static void hook_steal_init(void* self) {
    if (orig_steal_init) orig_steal_init(self);
    static void* k_sm = nullptr;
    if (!k_sm) k_sm = find_class("WuLin", "StealManager");
    void* sm = k_sm ? find_object_of_type(k_sm) : nullptr;
    if ((uintptr_t)sm < 0x10000) return;
    void* role = *(void**)((char*)sm + OFF_SM_SELECT_ROLE);
    if ((uintptr_t)role < 0x10000) return;
    void* gi = *(void**)((char*)role + OFF_ROLE_DATA);
    if ((uintptr_t)gi < 0x10000) return;
    dump_pack_tids("backPack", *(void**)((char*)gi + 0x140));
    dump_pack_tids("stealBackPack", *(void**)((char*)gi + 0x150));
    dump_pack_tids("dropOnDeadBackPack", *(void**)((char*)gi + 0x148));
}

/* ================================================================ 安装 */
static void install_tweaks() {
    g_i2cpp = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (g_i2cpp) {
        A_domain_get = (fn_i2c_get)dlsym(g_i2cpp, "il2cpp_domain_get");
        A_get_assemblies = (fn_i2c_asm)dlsym(g_i2cpp, "il2cpp_domain_get_assemblies");
        A_get_image = (fn_i2c_img)dlsym(g_i2cpp, "il2cpp_assembly_get_image");
        A_class_from_name = (fn_i2c_cls)dlsym(g_i2cpp, "il2cpp_class_from_name");
        A_class_get_type = (fn_i2c_cls_ty)dlsym(g_i2cpp, "il2cpp_class_get_type");
        A_type_get_object = (fn_i2c_ty_obj)dlsym(g_i2cpp, "il2cpp_type_get_object");
        A_object_new = (fn_i2c_new)dlsym(g_i2cpp, "il2cpp_object_new");
        A_string_new = (fn_i2c_str)dlsym(g_i2cpp, "il2cpp_string_new");
        A_array_new = (fn_i2c_arr)dlsym(g_i2cpp, "il2cpp_array_new");
        A_class_init = (fn_i2c_clsinit)dlsym(g_i2cpp, "il2cpp_runtime_class_init");
        A_obj_get_class = (fn_i2c_objcls)dlsym(g_i2cpp, "il2cpp_object_get_class");
        A_class_get_name = (fn_i2c_clsname)dlsym(g_i2cpp, "il2cpp_class_get_name");
        LOGI("il2cpp api: dom=%p asm=%p img=%p cls=%p ty=%p tyobj=%p new=%p str=%p",
             (void*)A_domain_get, (void*)A_get_assemblies, (void*)A_get_image,
             (void*)A_class_from_name, (void*)A_class_get_type, (void*)A_type_get_object,
             (void*)A_object_new, (void*)A_string_new);
    } else {
        LOGE("libil2cpp.so dlopen 失败");
    }
    cache_images();
    LOGI("程序集 %zu 个", g_nimg);

    /* ---- mod 的 bundle CRC 放行（必须早于任何 mod 加载） ---- */
    hook_install("Addressables.Crc", (void*)(g_base + RVA_ABRO_GET_CRC),
                 (void*)hook_get_crc, (void**)&orig_get_crc);
    hook_install("AssetBundle.LoadFromFileAsync", (void*)(g_base + RVA_AB_LOAD_FROM_FILE_ASYNC),
                 (void*)hook_load_file_async, (void**)&orig_load_file_async);

    /* ---- DynamicModifier 懒加载（patch 里的动态修改器没注册时兜底） ----
     * 实测这条路走不通：运行时 LoadAssetFromMod 取不到，返回 null 后
     * DynamicModifierInstance.get_IsValid 抛 NullReferenceException，
     * 导致「十全大补丸」连用都用不了。默认关闭。 */
#if WULIN_DM_LAZY
    load_asset_from_mod = (void* (*)(void*, void*))(g_base + RVA_MOD_LOAD_ASSET_FROM_MOD);
    hook_install("DynamicModifier.GetModifier", (void*)(g_base + RVA_DM_GET_MODIFIER),
                 (void*)hook_get_modifier, (void**)&orig_get_modifier);
#endif

    /* ---- 易物三件套 ---- */
    trade_install();

    /* ---- 暗取面板诊断（看清暗取面板显示的是 backPack 还是 stealBackPack） ---- */
    hook_install("暗取.Init", (void*)(g_base + RVA_STEAL_UI_INIT),
                 (void*)hook_steal_init, (void**)&orig_steal_init);

    /* ---- 水面可走 ---- */
    water_install();

    /* ---- 大地图 / 马车 ---- */
    hook_install("地图.Contains", (void*)(g_base + RVA_HASHSET_CONTAINS),
                 (void*)hook_contains, (void**)&orig_contains);
    hook_install("地图.IsMapEnable", (void*)(g_base + RVA_MAP_IS_ENABLE),
                 (void*)hook_is_map_enable, (void**)&orig_is_map_enable);
    hook_install("马车.IsPassIcon", (void*)(g_base + RVA_CAB_IS_PASS),
                 (void*)hook_is_pass_icon, (void**)&orig_is_pass_icon);
    hook_install("地图.initIcon", (void*)(g_base + RVA_INIT_ICON),
                 (void*)hook_initIcon, (void**)&orig_initIcon);
    hook_install("马车.UpdateIconShow", (void*)(g_base + RVA_CAB_UPDATE_ICON),
                 (void*)hook_cabIcon, (void**)&orig_cabIcon);

    /* ---- 剧情条件 ----
     * 两个子类 hook 都把「队友在队」判断换成基类实现，不再整函数恒真。 */
    base_check_all = (fn_check)(g_base + RVA_STORY_BASE);
    LOGI("剧情条件: 基类 CheckAllConnectNode @ %p", (void*)base_check_all);
    hook_install("剧情.IsJoinedTeam", (void*)(g_base + RVA_STORY_JOINED),
                 (void*)hook_check_joined, (void**)&orig_check_joined);
    hook_install("剧情.HavePartner", (void*)(g_base + RVA_STORY_PARTNER),
                 (void*)hook_check_partner, (void**)&orig_check_partner);

    /* ContinueArrange 递归看门狗：条件放宽后 Hold 空转会在这一层栈溢出 */
    hook_install("剧情.ContinueArrange", (void*)(g_base + RVA_STORY_CONTINUE),
                 (void*)hook_continue_arrange, (void**)&orig_continue_arrange);

    /* 兜底：名单外的剧情图，times 非 0 就不再放行（防未知的循环事件）。
     * 名单内的图在这里直接放行，交给原版判断。 */
    hook_install("剧情.HoldCheck", (void*)(g_base + RVA_HOLD_CHECK),
                 (void*)hook_hold_check, (void**)&orig_hold_check);

#if WULIN_PROBE
    probe_install();
#endif

    /* ---- 面板（最后装：它自己会建 UI） ---- */
#if WULIN_PANEL
    ui_install();
#else
    LOGI("面板已关闭（本版只测功能，全部默认开启）");
#endif

    LOGI("全部功能默认开启（面板可逐项关闭）");
}

/* ============================================================ 启动流程 */
static int phdr_cb(struct dl_phdr_info* info, size_t, void* data) {
    const char* n = info->dlpi_name;
    if (!n || !*n) return 0;
    const char* b = strrchr(n, '/');
    b = b ? b + 1 : n;
    if (strcmp(b, "libil2cpp.so") == 0) {
        *(uintptr_t*)data = (uintptr_t)info->dlpi_addr;
        return 1;
    }
    return 0;
}

static uintptr_t find_i2cpp() {
    uintptr_t base = 0;
    dl_iterate_phdr(phdr_cb, &base);
    return base;
}

static void* worker(void*) {
    LOGI("worker started, 等待 libil2cpp.so ...");
    void* h = nullptr;
    for (int i = 0; i < 1200 && !h; ++i) {
        h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
        if (!h) usleep(50000);
    }
    if (!h) { LOGE("libil2cpp.so 一直没出现"); return nullptr; }

    g_base = find_i2cpp();
    LOGI("libil2cpp base = 0x%lx", (unsigned long)g_base);

    fn_i2c_get domain_get = (fn_i2c_get)dlsym(h, "il2cpp_domain_get");
    if (!domain_get) { LOGE("找不到 il2cpp_domain_get"); return nullptr; }

    /* 运行时初始化完成前绝对不能碰 il2cpp API（会和 Runtime::Init 撞车） */
    sleep(8);
    for (int i = 0; i < 2400; ++i) {
        if (domain_get()) {
            LOGI("il2cpp runtime READY");
            install_tweaks();
            return nullptr;
        }
        usleep(50000);
    }
    LOGE("il2cpp 运行时超时未就绪");
    return nullptr;
}

__attribute__((constructor))
static void modkit_on_load() {
    unlink(LOG_PATH);
    LOGI("=========== libmodkit.so %s 已加载 (pid %d, 游戏 %s) ===========", MOD_VER, getpid(),
         GAME_VERSION);
    pthread_t t;
    if (pthread_create(&t, nullptr, worker, nullptr) != 0) {
        LOGE("pthread_create 失败");
        return;
    }
    pthread_detach(t);
}
