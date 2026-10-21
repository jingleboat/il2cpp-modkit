#pragma once
/* 地址表（示例 / 占位）
 * =====================================================================
 * 这里是原项目里「函数地址表」的位置。为了不把针对某一版本客户端的具体
 * 地址固化进仓库，本文件的所有地址都已置 0。
 *
 * 你需要：
 *   1) 用 Il2CppDumper 之类工具对自己的客户端做一次 dump，拿到 dump.cs；
 *   2) 按下面的注释（每个宏后面写着它对应哪个方法）把真实地址填回来；
 *   3) 复制本文件为 addresses.h（addresses.h 已被 .gitignore 忽略）。
 *
 * 找不到地址时编译能过、运行时会崩，所以务必逐个核对。
 * ===================================================================== */

/* ================================================================== RVA */
/* ---- il2cpp 通用 ---- */
#define RVA_FIND_OBJECT_TYPE 0x0   /* Object.FindObjectOfType(Type) —— 真货，参数必须是 System.Type 对象 */
#define RVA_SINGLETON_GET 0x0      /* MonoSingleton<object>.get_Instance —— 参数按 MethodInfo 传（传元数据槽里的
                                          * 泛型方法用法槽，就能拿到对应单例）。resolve_save 一直用它，别改成别的。 */
#define RVA_GET_NAME 0x0           /* Object.get_name() */
#define RVA_GO_CHILD_OBJ 0x0       /* Component.get_transform()（拿子节点 Transform 时也用它） */
#define RVA_COMP_GET_GO 0x0        /* Component.get_gameObject() */
#define RVA_GO_GET_COMPONENT 0x0   /* GameObject.GetComponent(Type) */
#define RVA_COMP_GET_TRANSFORM 0x0 /* Component.get_transform() */
#define RVA_T_SET_LAST 0x0         /* Transform.SetAsLastSibling() */
#define RVA_CANVAS_SCALE 0x0       /* Canvas.get_scaleFactor() */

/* ---- 易物 TradingWithNpcManager ---- */
#define RVA_TR_START 0x0           /* static void StartTrading(GameCharacterInstance) */
#define RVA_TR_CANCEL 0x0          /* static void CancelTrading() */
#define RVA_TR_CONFIRM 0x0         /* static void ConfirmTrading() */
#define RVA_TR_VALUE_OFFSET 0x0    /* static long ValueOffset() */
#define RVA_TR_GET_NPC 0x0         /* static GameCharacterInstance get_npc() */
#define RVA_TR_PLAYER_ZONE 0x0     /* static GameItemPack get_playerTradingZone() */
#define RVA_TR_NPC_ZONE 0x0        /* static GameItemPack get_npcTradingZone() */

/* ---- 背包 / 物品 ---- */
#define RVA_PACK_GET_TOTAL 0x0     /* long GameItemPack.GetTotalValue(float) */
#define RVA_PACK_ADD_INDEP 0x0     /* bool AddItemAsIndependent(GameItemInstance) */
#define RVA_PACK_REMOVE 0x0        /* bool RemoveItem(GameItemInstance,int) */
#define RVA_ITEM_CTOR_ID 0x0       /* GameItemInstance..ctor(int id,int stack,int craftLevel) */
#define RVA_ITEM_CTOR_KEY 0x0      /* GameItemInstance..ctor(string key,int stack,int craftLevel) */
#define RVA_ITEM_ISVALID 0x0       /* bool get_IsValid() */

/* ---- 角色 / 池子 ---- */
#define RVA_CHAR_BACKPACK 0x0      /* GameItemPack get_BackPack() */
#define RVA_CHAR_DROP_PACK 0x0     /* GameItemPack get_DropOnDeadBackPack() */
#define RVA_CHAR_STEAL_PACK 0x0    /* GameItemPack get_StealBackPack() */
#define RVA_CHAR_TEMPLETE 0x0      /* CharacterPoolData get_Templete() */
#define RVA_CHAR_TPL_NAME 0x0      /* string get_TempleteName() */
#define RVA_CHAR_INSTANCE_ID 0x0   /* int get_InstanceId() */
#define RVA_CHAR_TPL_UID 0x0       /* int get_TempleteUid() */

/* ---- 队伍背包 ---- */
#define RVA_TEAM_INVENTORY 0x0     /* PlayerTeamManager.get_TeamInventory() */

/* ---- 易物 UI 刷新 ---- */
#define RVA_SHOWER_SETDIRTY 0x0    /* ItemPackShower.SetDirty() */

/* ---- 暗取（潜行偷窃） ---- */
#define RVA_STEAL_UI_INIT 0x0      /* StealPanelUI.Init()：面板里 ShowPack 的就是 npc.backPack */
#define OFF_SM_SELECT_ROLE 0x0          /* StealManager.SelectRole */
#define OFF_ROLE_DATA 0x0               /* Role.gameCharacterInstance */

/* ---- 大地图 / 马车 ---- */
#define RVA_MAP_IS_ENABLE 0x0      /* UIWorldMapManager.IsMapEnable(int) */
#define RVA_CAB_IS_PASS 0x0        /* UICabMapManager.IsPassIcon(int) */
#define RVA_HASHSET_CONTAINS 0x0   /* HashSet<T>.Contains(容器,id,类型信息) */
#define RVA_INIT_ICON 0x0          /* UIWorldMapManager.initIcon() */
#define RVA_CAB_UPDATE_ICON 0x0    /* UICabMapManager.UpdateIconShow() */
#define RVA_T_CHILD_COUNT 0x0      /* Transform.get_childCount() */
#define RVA_T_GET_CHILD 0x0        /* Transform.GetChild(int) */
#define RVA_GET_OR_CREATE_MAPDATA 0x0 /* SaveObjectMapManager.GetOrCreateMapData(int) */
#define VA_CLASS_CELL_UIMAP 0x0    /* UIWorldMapManager 的 Il2CppClass 元数据槽 */

/* ---- 剧情条件 ---- */
#define RVA_STORY_JOINED 0x0       /* IsJoinedTeamCondition.CheckAllConnectNode */
#define RVA_STORY_PARTNER 0x0      /* HavePartnerCondition.CheckAllConnectNode */
#define RVA_STORY_BASE 0x0         /* ConditionNode.CheckAllConnectNode（基类实现）—— 走它才只丢掉子类那一条判断 */
#define RVA_STORY_CONTINUE 0x0     /* StoryManager.ContinueArrange(Node,string) —— 递归看门狗挂这里 */
#define RVA_GET_CHARACTER 0x0      /* GameCharacterManager.GetCharacter(int) —— 从 uid 拿实例 */
#define RVA_HOLD_CHECK 0x0         /* HoldNode.CheckCondition() —— 「Hold 只放行一次」挂这里 */

/* ---- 水面可走 ---- */
#define RVA_MM_ISBLOCK 0x0         /* bool MapManager.IsBlock(Vector3Int) */
#define RVA_MM_ISSWIM 0x0          /* bool MapManager.IsSwimGrid(Vector3Int) */

/* ---- 剧情里的「图标显示」类条件（判定期间抑制地图解锁，见 main.cpp 注释） ---- */
#define RVA_CITY_VISIBLE_CHECK 0x0 /* WorldMapCityVisibleCondition.CheckAllConnectNode（判断大地图图标显示） */
#define RVA_CAB_PASS_COND 0x0      /* CabPassIconCondition.CheckAllConnectNode（判断马车图标是否通过） */

/* ---- Addressables / AssetBundle ---- */
#define RVA_ABRO_GET_CRC 0x0       /* AssetBundleRequestOptions.get_Crc() */
#define RVA_AB_LOAD_FROM_FILE_ASYNC 0x0 /* AssetBundle.LoadFromFileAsync_Internal(string,uint,ulong) */

/* ---- DynamicModifier 懒加载 ---- */
#define RVA_DM_GET_MODIFIER 0x0        /* DynamicModifierHelper.GetModifier(string) */
#define RVA_MOD_LOAD_ASSET_FROM_MOD 0x0 /* ModManager.LoadAssetFromMod(Type,string) */

/* ---- 每帧 tick（面板用） ---- */
#define RVA_MTSC_UPDATE 0x0        /* MainThreadSynchronizationContext.Update() */

/* ---- UGUI 构建（面板） ---- */
#define RVA_GO_CTOR_NAME 0x0       /* GameObject..ctor(string) */
#define RVA_GO_ADD_COMP 0x0        /* GameObject.AddComponent(Type) */
#define RVA_GO_GET_TRANSFORM 0x0   /* GameObject.get_transform() */
#define RVA_TR_SETPARENT 0x0       /* Transform.SetParent(Transform,bool) */
#define RVA_RT_ANCHOR_MIN 0x0      /* RectTransform.set_anchorMin(Vector2) */
#define RVA_RT_ANCHOR_MAX 0x0      /* set_anchorMax(Vector2) */
#define RVA_RT_ANCHORED_POS 0x0    /* set_anchoredPosition(Vector2) */
#define RVA_RT_SIZE_DELTA 0x0      /* set_sizeDelta(Vector2) */
#define RVA_RT_PIVOT 0x0           /* set_pivot(Vector2) */
#define RVA_TEX_CTOR 0x0           /* Texture2D..ctor(int,int,TextureFormat,int,bool) */
#define RVA_TEX_LOAD_RAW 0x0       /* LoadRawTextureData(IntPtr,int) */
#define RVA_TEX_APPLY 0x0          /* Apply() */
#define RVA_TEX_SET_FILTER 0x0     /* Texture.set_filterMode(FilterMode) */
#define RVA_TEX_SET_WRAP 0x0       /* Texture.set_wrapMode(TextureWrapMode) */
#define RVA_RAWIMG_SET_TEX 0x0     /* RawImage.set_texture(Texture) */
#define RVA_RAWIMG_SET_UVRECT 0x0  /* RawImage.set_uvRect(Rect) */
#define RVA_GRAPHIC_SET_RAYCAST 0x0/* Graphic.set_raycastTarget(bool) */
#define RVA_CANVAS_SET_ORDER 0x0   /* Canvas.set_sortingOrder(int) */
#define RVA_CANVAS_SET_MODE 0x0    /* Canvas.set_renderMode(int) */
#define RVA_DONTDESTROY 0x0        /* Object.DontDestroyOnLoad(Object) */
#define RVA_INPUT_MOUSE_DOWN 0x0   /* Input.GetMouseButtonDown(int) */
#define RVA_INPUT_MOUSE_POS 0x0    /* Input.get_mousePosition() -> Vector3 */
#define RVA_SCREEN_HEIGHT 0x0      /* Screen.get_height() */
#define RVA_SCREEN_WIDTH 0x0       /* Screen.get_width() */

