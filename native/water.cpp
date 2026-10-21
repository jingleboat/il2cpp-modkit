/*
 * ModKitTweaks · 水面可走 / 入水形态（仅限烤表地图）
 * ================================================
 * 设计意图：只对「烤表里有点」的那几个场景开放水面，
 * 其它地图一律保持原样 —— 不放行、能站水面上这种副作用也不要有。
 *
 * 结论来源（已在设备上逐层验证）：
 *   1) 漫游里能不能走进一格只看 MapManager.IsBlock（被挡 → Role.GetNoPathEnd，停在岸边）。
 *   2) 「站在水里」只有一条路：MapManager.IsSwimGrid 为真 → Role.CheckInBathWater
 *      → GameCharacterInstance.SetBathing(true) + RoamingManager.SetInBath(true)
 *      + GameUtil.CreateRoleGhost（水中倒影）+ Animator 参数 Bath/InWater/IsBath
 *      + 隐藏影子 + 移速换 CharMoveSpeedInWater。
 *   3) 手机版运行期地图实例里 `_SwimGrid` / `_SwimLadder` 两个图层根本不存在
 *      （实测运行期图层 24 个：SpecialLayer, Ground0..12, Wall, WallCorner, WallThing,
 *        BuildingStatic, Building, Gateway, Escapeway, TopRoof, TopSky, _Block），
 *      所以 mSwimGrid 全零、IsSwimGrid 恒 false。PC 上能下水靠的就是 prefab 里那份数据。
 *
 * 因此本模块的水面判定**只认** water_mask.h（构建期由 gen_water_mask.py 从 APK 的
 * 地图 prefab 抽出 `_SwimGrid` 烤成坐标表）：504 = 760 格、2160 = 177 格、2169 = 0 格。
 * 没在表里的地图 → 一行代码都不干预（既不放开 IsBlock，也不伪造 IsSwimGrid）。
 *
 * 战斗态（MapManager.mBattleState）同样一律原样返回。
 */
#include "common.h"
#include "hook.h"
#include "water.h"
#include "water_mask.h"

#include <dlfcn.h>

/* RVA_MM_ISBLOCK / RVA_MM_ISSWIM 在 addresses.h 里 */

/* 字段偏移（绑定具体 dump） */
#define OFF_MM_CURMAP 0x94 /* MapManager.mCurMapID */
#define OFF_MM_BATTLE 0xC0 /* MapManager.mBattleState */

typedef bool (*fn_block)(void*, uint64_t, uint32_t);

/* 入水形态开关（只对烤表地图生效，恒开；面板块控制的是 F_WATER，总闸） */
static int F_WATER_ANIM = 1;

static int g_log_block = 0, g_log_swim = 0;

/* ------------------------------------------------------------- 烤表查询 */
static const WaterMaskEntry* mask_for(int mapid) {
    for (int i = 0; i < WATER_MASK_COUNT; ++i) {
        if (WATER_MASKS[i].mapid == mapid) return &WATER_MASKS[i];
    }
    return nullptr;
}

static int mask_has(const WaterMaskEntry* e, int x, int y) {
    if (!e || e->count <= 0) return 0;
    int64_t key = ((int64_t)(uint32_t)y << 32) | (int64_t)(uint32_t)x;
    int lo = 0, hi = e->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (e->keys[mid] == key) return 1;
        if (e->keys[mid] < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* ------------------------------------------------------------------- hooks */
static fn_block orig_isblock = nullptr;
static fn_block orig_isswim = nullptr;

static bool hook_isblock(void* self, uint64_t xy, uint32_t z) {
    bool r = orig_isblock ? orig_isblock(self, xy, z) : false;
    if (!r || !F_WATER || !self) return r;
    if (*(unsigned char*)((char*)self + OFF_MM_BATTLE)) return r;

    const WaterMaskEntry* e = mask_for(*(int*)((char*)self + OFF_MM_CURMAP));
    if (!e) return r; /* 非烤表地图：完全不干预 */

    int x = (int)(uint32_t)(xy & 0xffffffffu);
    int y = (int)(uint32_t)(xy >> 32);
    if (mask_has(e, x, y)) {
        if (g_log_block++ < 10) LOGI("[water] IsBlock(%d,%d)=true→false（水面 map=%d）", x, y, e->mapid);
        return false;
    }
    return r;
}

static bool hook_isswim(void* self, uint64_t xy, uint32_t z) {
    bool r = orig_isswim ? orig_isswim(self, xy, z) : false;
    if (r) return true;
    if (!F_WATER || !F_WATER_ANIM || !self) return false;
    if (*(unsigned char*)((char*)self + OFF_MM_BATTLE)) return false;

    const WaterMaskEntry* e = mask_for(*(int*)((char*)self + OFF_MM_CURMAP));
    if (!e) return false; /* 非烤表地图：入水形态也不伪造，保持原版 */

    int x = (int)(uint32_t)(xy & 0xffffffffu);
    int y = (int)(uint32_t)(xy >> 32);
    if (mask_has(e, x, y)) {
        if (g_log_swim++ < 10) LOGI("[water] IsSwimGrid(%d,%d)=true（入水形态 map=%d）", x, y, e->mapid);
        return true;
    }
    return false;
}

void water_install() {
    if (!g_i2cpp) g_i2cpp = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    for (int i = 0; i < WATER_MASK_COUNT; ++i) {
        LOGI("[water] 烤表 map=%d 水面格=%d（其它地图不干预）", WATER_MASKS[i].mapid, WATER_MASKS[i].count);
    }
    if (hook_install("MapManager.IsBlock", (void*)(g_base + RVA_MM_ISBLOCK),
                     (void*)hook_isblock, (void**)&orig_isblock))
        LOGI("[water] 已挂 MapManager.IsBlock");
    else
        LOGE("[water] 挂 IsBlock 失败");

    if (hook_install("MapManager.IsSwimGrid", (void*)(g_base + RVA_MM_ISSWIM),
                     (void*)hook_isswim, (void**)&orig_isswim))
        LOGI("[water] 已挂 MapManager.IsSwimGrid");
    else
        LOGE("[water] 挂 IsSwimGrid 失败");
}
