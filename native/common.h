/*
 * ModKitTweaks · 手机端公共定义
 * =============================
 * 1) 日志 / 已解析地址 / 运行时开关（面板改的就是这些）
 * 2) 结构体字段偏移（绑定一份具体 dump，换版本/换游戏必须重新标定）
 * 3) il2cpp C API 指针（来自 libil2cpp.so 的导出符号，不写死偏移）
 * 4) 函数地址表在 addresses.h —— 由 addresses.example.h 复制后自行填写
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <android/log.h>

/* ------------------------------------------------------------------ 版本锚点 */
#define GAME_VERSION "0.0.0"
#define MOD_VER "dev"   /* 本库自己的版本号，只用来对日志 */
#define PKG_NAME "com.example.target"
#define LOG_PATH "/sdcard/Android/data/" PKG_NAME "/files/modkit.log"
/* 易物「已拿走」台账（避免同一件池子物件被反复复制） */
#define LEDGER_PATH "/sdcard/Android/data/" PKG_NAME "/files/trade-taken.txt"

/* ------------------------------------------------------------------ 日志 */
void glog(const char* fmt, ...);
#define LOGI(...) glog(__VA_ARGS__)
#define LOGE(...) glog("E: " __VA_ARGS__)

/* ------------------------------------------------------------------ 全局 */
extern uintptr_t g_base;   /* libil2cpp.so 加载基址 */
extern void* g_i2cpp;      /* libil2cpp.so 句柄（dlsym 用） */

/* 运行时开关：1 = 开。面板点一下就变，hook 里实时读。 */
extern volatile int F_TRADE_WIN;    /* 易物作价恒占优 */
extern volatile int F_TRADE_POOLS;  /* 易物显示池子 */
extern volatile int F_TRADE_TAKEALL;/* 易物确认搬空 */
extern volatile int F_MAP;          /* 大地图解锁 */
extern volatile int F_CAB;          /* 马车地图解锁 */
extern volatile int F_STORY;        /* 剧情/队伍条件放宽 */
extern volatile int F_WATER;        /* 水面可走（进水池 + 触发入水状态） */

/* ================================================================= 地址表 */
/* 不要在这里写死 RVA：全部集中到 addresses.h（gitignored），换客户端只改那一份 */
#include "addresses.h"

/* ================================================================= 偏移 */
#define OFF_PACK_CONTENTS 0x48   /* GameItemPack.contents (List<GameItemInstance>) */
#define OFF_ITEM_STACK 0x20      /* GameItemInstance.m_stack */
#define OFF_ITEM_TID 0x1C        /* GameItemInstance.m_templeteId */
#define OFF_CPL_DROP 0x188       /* CharacterPoolData.ItemsDropOnKill (KeyWithValues[]) */
#define OFF_CPL_STEAL 0x190      /* CharacterPoolData.StealPool */
#define OFF_KWV_KEY 0x10         /* KeyWithValues.key (string) */
#define OFF_KWV_VALUES 0x18      /* KeyWithValues.values (List<int>) */
#define OFF_UI_NPC_BAG 0xA8      /* TradingWithNpcUI.npcBackPack */
#define OFF_UI_NPC_TRADE 0xB8    /* TradingWithNpcUI.npcTrading */
#define OFF_HOLD_TIMES 0x70      /* HoldNode.checkSucessTimes（addTimes 置 1 / clearTimes 置 0） */
#define OFF_NODE_GRAPH 0x18      /* XNode.Node.graph -> NodeGraph（ScriptableObject，name 即剧情图 ID） */
#define OFF_PARTNER_ID 0x58      /* ConditionNode(IsJoinedTeam/HavePartner).partnerID */
#define OFF_PARTNER_NEGATE 0x5C  /* 同上 Negate（取反） */
#define OFF_TEAM_STATE 0x130     /* GameCharacterInstance.m_TeamMemberState（== NULL 表示从未入队） */

/* ================================================================= il2cpp API */
typedef void* (*fn_i2c_get)(void);
typedef void** (*fn_i2c_asm)(void*, size_t*);
typedef void* (*fn_i2c_img)(void*);
typedef void* (*fn_i2c_cls)(void*, const char*, const char*);
typedef void* (*fn_i2c_cls_ty)(void*);   /* il2cpp_class_get_type -> const Il2CppType* */
typedef void* (*fn_i2c_ty_obj)(void*);   /* il2cpp_type_get_object -> Il2CppReflectionType* */
typedef void* (*fn_i2c_new)(void*);
typedef void* (*fn_i2c_str)(const char*);
typedef void* (*fn_i2c_arr)(void*, size_t);
typedef void (*fn_i2c_clsinit)(void*);
typedef void* (*fn_i2c_objcls)(void*);        /* il2cpp_object_get_class -> Il2CppClass* */
typedef const char* (*fn_i2c_clsname)(void*); /* il2cpp_class_get_name -> const char* */

extern fn_i2c_objcls A_obj_get_class;
extern fn_i2c_clsname A_class_get_name;
extern fn_i2c_get A_domain_get;
extern fn_i2c_asm A_get_assemblies;
extern fn_i2c_img A_get_image;
extern fn_i2c_cls A_class_from_name;
extern fn_i2c_cls_ty A_class_get_type;
extern fn_i2c_ty_obj A_type_get_object;
extern fn_i2c_new A_object_new;
extern fn_i2c_str A_string_new;
extern fn_i2c_arr A_array_new;
extern fn_i2c_clsinit A_class_init;

/* 按命名空间 + 类名在所有程序集里查 Il2CppClass（找不到返回 0） */
void* find_class(const char* ns, const char* name);
/* 找类的 System.Type 对象（AddComponent / FindObjectOfType 用） */
void* class_type(void* klass);
/* FindObjectOfType：传入 Il2CppClass，返回实例（可能为 0） */
void* find_object_of_type(void* klass);
/* 32 位 int 的 Il2CppClass（造 List/数组用） */
void* class_int32(void);

/* Il2CppString -> 我们自己缓冲区（ASCII 兜底 '?'）；返回 buf */
const char* str_to_buf(void* il2str, char* buf, int cap);

/* ================================================== arm64 小工具（内联 asm 用） */
typedef struct { float x, y; } V2;
typedef struct { float x, y, z; } V3;
typedef struct { float x, y, w, h; } Rect4;
