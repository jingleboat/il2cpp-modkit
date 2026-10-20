/*
 * ModKitTweaks · Android 原生插件（v4：易物三件套 + 游戏内面板）
 * ==============================================================
 * 通道：il2cpp_init 首指令被静态改成 B 到插页里的引导桩，桩 dlopen 本库。
 * 本文件负责：
 *   1) 等 libil2cpp.so / il2cpp 运行时就绪，解析出全部 RVA 与 il2cpp C API
 *   2) 装 hook：
 *        易物三件套（trade.cpp）
 *        大地图 / 马车地图（IsMapEnable / IsPassIcon / HashSet.Contains / initIcon / cabIcon）
 *        剧情条件放宽（两个 CheckAllConnectNode）
 *        游戏内面板（ui.cpp，每帧 tick 挂在 MainThreadSynchronizationContext.Update）
 *   3) 全部功能都由运行时开关控制，面板点一下即时生效（默认全开）
 *
 * 静态补丁已全部取消（除了「鸣谢 → 模组页」那条 4 字节补丁，在 libil2cpp_intl_ui.so 里），
 * 因为静态补丁写死不可关，而面板要求每个开关都能实时切换。
 */
#include "common.h"
#include "hook.h"
#include "trade.h"
#include "ui.h"
/* probe / water 两个模块不进本仓库：
 *   probe.cpp 是调试用的状态导出，绑定了特定版本的结构布局；
 *   water.cpp 依赖 water_mask.h（按地图烘焙出来的可走格掩码，属于游戏数据）。
 * 需要时按同样的方式自己实现，然后在 install_tweaks() 里挂上。 */

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
#define MK_PANEL 1

/* 排查探针：1 = 往 modkit.log 打水面/阻挡诊断，正式包改回 0 */
#define MK_PROBE 0

/* DynamicModifier 运行时懒加载兜底：实测不可用（见 install_tweaks 注释） */
#define MK_DM_LAZY 0

/* ------------------------------------------------------------------ 全局 */
uintptr_t g_base = 0;
void* g_i2cpp = nullptr;

volatile int F_TRADE_WIN = 1;
volatile int F_TRADE_POOLS = 1;
volatile int F_TRADE_TAKEALL = 1;
volatile int F_MAP = 1;
volatile int F_CAB = 1;
volatile int F_STORY = 0;
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

static int hook_contains(void* set, int id, void* type) {
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
    bool r = orig_is_map_enable ? orig_is_map_enable(self, id) : false;
    return F_MAP ? true : r;
}

/* UICabMapManager.IsPassIcon(int) */
static bool (*orig_is_pass_icon)(void* self, int id) = nullptr;
static bool hook_is_pass_icon(void* self, int id) {
    bool r = orig_is_pass_icon ? orig_is_pass_icon(self, id) : false;
    return F_CAB ? true : r;
}

static void (*orig_initIcon)(void* self) = nullptr;
static void hook_initIcon(void* self) {
    if (orig_initIcon) orig_initIcon(self);
    if (self && !g_uimap) {
        g_uimap = self;
        LOGI("大地图: uimap=%p listWorldMap=%p", g_uimap, *(void**)((char*)self + 0xE0));
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
    resolve_save();
    if (orig_cabIcon) orig_cabIcon(self);
    if (F_CAB) inject_mapdata(self);
}

/* ============================================================== 剧情条件 */
static bool (*orig_check_joined)(void* self, int io, void* line) = nullptr;
static bool (*orig_check_partner)(void* self, int io, void* line) = nullptr;
typedef bool (*fn_check)(void*, int, void*);

/* 「剧情条件放宽」的自旋闸门 -------------------------------------------------
 * 直接让 IsJoinedTeam / HavePartner 恒真会把剧情 Hold 节点变成"条件随时满足"，
 * StoryManager.ContinueArrange <-> HoldNode.OnActive 互相递归，切场景时栈溢出闪退
 * （实测 crash 栈：#n 反复出现 HoldNode.OnActive / HoldNode.Continue / ContinueArrange）。
 * 这里按「同一条件对象每秒最多放行 20 次」限流：正常剧情判定照旧放行，
 * 一旦出现空转就退回原逻辑，让 Hold 节点正常等待。 */
#define GATE_SLOTS 16
typedef struct { void* p; long long t; int n; } StoryGate;
static StoryGate g_gate[GATE_SLOTS];

static long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int story_pass_once(void* self) {
    long long t = now_ms();
    for (int i = 0; i < GATE_SLOTS; ++i) {
        if (g_gate[i].p == self) {
            if (t - g_gate[i].t > 1000) { g_gate[i].t = t; g_gate[i].n = 1; return 1; }
            g_gate[i].n++;
            return g_gate[i].n <= 20;
        }
    }
    for (int i = 0; i < GATE_SLOTS; ++i) {
        if (!g_gate[i].p) { g_gate[i].p = self; g_gate[i].t = t; g_gate[i].n = 1; return 1; }
    }
    return 1;
}

static bool hook_check_joined(void* self, int io, void* line) {
    if (F_STORY && story_pass_once(self)) return true;
    return orig_check_joined ? orig_check_joined(self, io, line) : false;
}

static bool hook_check_partner(void* self, int io, void* line) {
    if (F_STORY && story_pass_once(self)) return true;
    return orig_check_partner ? orig_check_partner(self, io, line) : false;
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
 *   "获取DynamicModifier失败：Item/SomeDynamicModifier"
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
     * 导致「某个物品」连用都用不了。默认关闭。 */
#if MK_DM_LAZY
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
    /* water_install();  // 见上方说明：依赖自备的地图掩码数据 */

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

    /* ---- 剧情条件 ---- */
    hook_install("剧情.IsJoinedTeam", (void*)(g_base + RVA_STORY_JOINED),
                 (void*)hook_check_joined, (void**)&orig_check_joined);
    hook_install("剧情.HavePartner", (void*)(g_base + RVA_STORY_PARTNER),
                 (void*)hook_check_partner, (void**)&orig_check_partner);

#if MK_PROBE
    /* probe_install(); */
#endif

    /* ---- 面板（最后装：它自己会建 UI） ---- */
#if MK_PANEL
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
    LOGI("=========== libmodkit.so v4 已加载 (pid %d, %s) ===========", getpid(), GAME_VERSION);
    pthread_t t;
    if (pthread_create(&t, nullptr, worker, nullptr) != 0) {
        LOGE("pthread_create 失败");
        return;
    }
    pthread_detach(t);
}
