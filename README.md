# il2cpp-modkit

给 **Unity + IL2CPP 的 Android 游戏**做原生层微调的骨架，外加一套「数据 mod 重建 + APK 重打包」的工具链。

最初是从一个单机 Unity 手游的移植工作里抽出来的，把跟那款游戏绑死的东西全部剥掉了：**仓库里没有任何游戏资源、没有二进制、没有函数地址表**。留下的都是可直接复用的部分。

---

## 这里有什么

### 1. `native/` — 原生层 hook 框架（C++ / arm64）

| 文件 | 内容 |
|---|---|
| `hook.cpp` / `hook.h` | arm64 内联 hook：覆盖目标函数前 16 字节写 `ldr x16,#8 / br x16`，被覆盖的 4 条指令搬进跳板。跳板里把 `adrp`、`b`/`bl`、`b.cond`、`cbz/cbnz` 全部重定位成绝对跳转，所以跳板落在多远都行 |
| `ui.cpp` / `ui.h` / `glyphs.h` | 运行时在游戏里画一块 UGUI 面板（软件光栅 + 自带点阵字体），不依赖任何 UI 资源，用来做功能开关 |
| `trade.cpp` / `trade.h` | 一个「把池子里的东西复制进背包给面板显示、被拿走时从原件扣掉」的完整实现，含按 NPC 实例记账的台账 |
| `water.cpp` / `water.h` / `water_mask.h` | 「水面可走 + 触发入水状态」：因为运行期地图实例里没有水面图层数据，改为构建期把水面格烤成坐标表（`water_mask.h`），只在表里的地图上放开阻挡判定与入水标记 |
| `common.h` | il2cpp C API 的查找封装（`find_class` / `find_object_of_type` / `class_type`）+ 结构体字段偏移；地址表统一从 `addresses.h` 引入 |
| `addresses.example.h` | **地址表占位**。所有 RVA 都是 `0x0`，注释里写着每个宏对应哪个方法，需要用你自己的 dump 填 |
| `main.cpp` | 入口：等 `libil2cpp.so` 加载 → 拿基址 → 装 hook → 起面板。含三类可复用的改法：容器查询型（地图解锁）、条件判断型（走基类而不是恒真）、状态型（Hold 只放行一次 + 递归限深） |

### 2. `tools/` — 打包与格式工具（Python）

| 文件 | 内容 |
|---|---|
| `cat_parse.py` | Addressables `ContentCatalogData` 四个 blob 的解析 / 重建自检。改 bundle 之后必须同步 catalog，这是唯一可靠的校验工具 |
| `swap_mod.py` | 把 mod 目录塞进 APK 的指定槽位、替换/新增 so，重打包 + zipalign + apksigner（可选直接 adb install）。路径、口令、build-tools 全部走参数或环境变量 |
| `mk_apk.py` | 底层 zip 重写：`.so` 4K 对齐、其余 4 字节对齐，输出未签名 APK |
| `armdis.py` | arm64 反汇编辅助，吃 Il2CppDumper 的 `script.json`，自动把 `bl` 目标、方法名、字符串字面量标出来 |

> 「以原生 bundle 打底、把另一平台的 mod 数据搬进去、再重建 catalog」这套流程没有做成脚本发出来 ——
> 它跟具体游戏的 mod 结构绑得太死（哪些 asset 共有、哪些是新增、槽位叫什么）。
> 流程和每一步的自检写在 `docs/porting-notes.md`，照着改自己的 `SHARED` / `NEW_ASSETS` 两张表即可。

### 3. `docs/`

| 文档 | 内容 |
|---|---|
| `addressables-catalog-format.md` | `ContentCatalogData` 四段二进制格式（逐字节验证过）、`m_resourceTypes` 索引、CRC 陷阱 |
| `porting-notes.md` | 移植时踩过的坑：引擎版本差异、MonoBehaviour raw data 的三处对齐、DynamicModifier 只能构建期注册、缓存、三个物品容器的区别 |
| `native-hook-notes.md` | 改游戏逻辑时的九条原则：先确认判定发生在哪一层、走基类而不是恒真、放宽条件的两道兜底、**容器指针型拦截的初始化时机**、共享泛型热路径不要碰 il2cpp、查找失败不要静默兜底、规则缓存要带 TTL、偏移要靠「读出来的值合不合理」验证、装机验证闭环 |

---

## 快速开始

```powershell
# 1) 生成地址表并填地址
cd native
copy addresses.example.h addresses.h
#   用 Il2CppDumper 对自己的客户端 dump，按 addresses.h 里每个宏后面的注释把 RVA 填上

# 2) 编译（需要 Android NDK r26+，改 build.ps1 里的 $ndk）
pwsh -File build.ps1

# 3) 把库塞进 APK（先只换 so，不动数据 mod）
cd ..\tools
python -X utf8 swap_mod.py --src C:\path\to\base.apk `
    --so C:\path\to\libil2cpp_patched.so --lib ..\native\libmodkit.so `
    --keystore C:\path\to\sign.jks --ks-pass 你的口令 --key-pass 你的口令 `
    --out C:\path\to\out.apk --no-install
```

要替换数据 mod 槽位就加 `--mod <mod 目录> --slot <槽位名>`；不带 `--mod` 时只处理 so。`--list-slots` 会列出底包里已存在的槽位名。

> 注入方式没放进仓库（每个游戏、每个加固方案都不一样）。要么用 Magisk/Zygisk 模块把 so `dlopen` 进去，要么改 APK 的 `libil2cpp.so` 加一段桩，两种都行 —— `hook.cpp` 只要求「进程里有你的 so」。

---

## 边界

- 仓库**不含**任何游戏本体、反编译产物、资源文件、函数地址。`addresses.example.h` 全是占位。
- 面向**单机 / 自娱**场景：改的是自己那份客户端，不涉及联网服务端。
- 每个游戏的 ABI、结构偏移、函数签名都不一样，`common.h` 里的字段偏移是从一份具体 dump 里量的，换游戏必须重新标定。

## License

MIT
