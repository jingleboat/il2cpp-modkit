# 把一个 Unity 数据 mod 从 PC 搬到安卓：踩过的坑

前提：游戏两端都是 Unity + IL2CPP，但**版本不同、资源包版本不同、catalog 版本不同**。直接拷 mod 目录一定失败。下面是按「先比差异再动手」的顺序整理的。

---

## 0. 先比这五样，能省一天

| 项 | 怎么比 | 典型差异 |
|---|---|---|
| 引擎 / 资源包版本 | `UnityFS` 头的 `m_Version` 与 `m_EngineVersion` | 一端 v7 / 2020.x，另一端 v8 / 2022.x |
| catalog 文件名 | 目录里实际的文件名 | `catalog_0.1.0.json` vs `catalog_0.1.json` |
| catalog 里的资源路径 | `m_InternalIds` 里是 `\` 还是 `/` | Windows 端导出常带反斜杠 |
| 冗余文件 | 有没有 `*.meta` | Unity 工程残留，带上就报错 |
| catalog 字段集 | 有没有 `m_BuildResultHash`、provider 数量 | 新版多几个字段 |

## 1. `.bundle` 本身通常不是问题

两端的 bundle 是同一套 Unity 序列化格式，`UnityPy` 能读能写。真正要小心的只有三件事（下面 §3）。

**基底要选对**：以「目标平台 APK 里原生的那个 mod bundle」为基底去改，别拿 PC 的当容器。原因是 `m_Script`（MonoScript 的 PPtr pathID）是**包内局部 ID**，同一份代码在两个包里编号不同，跨包搬会指向空的脚本，加载出来是一堆 null 字段。

## 2. catalog 必须跟着重建

详见 `addressables-catalog-format.md`。最低要求：

- 新增/删除资源 → `m_InternalIds`、`m_KeyDataString`、`m_BucketDataString`、`m_EntryDataString` 四处同步；
- 改了任何 bundle → `m_Crc` 置 0、`m_BundleSize` 改真实大小。

## 3. `MonoBehaviour` raw data 的三处对齐

Unity 里 `MonoBehaviour` 的 raw data 是一段「头部 + 尾巴」：

```
m_GameObject(12)  m_Enabled(4)  m_Script(12)  m_Name(len+utf8+pad4)  typeName(len+utf8)
[int32][int32 count] (重复 count 次: [klen][key][jlen][json])  [4 个 0 字节]
```

三个必踩：

1. **`m_Name` 之后要补 4 字节对齐**，否则 `typeName` 的长度读错，整个对象错位；
2. **`m_Script` 的 pathID 不能跨包搬**（见 §1）；
3. **JSON 后面固定跟 4 个 0 字节**，少了就是 `The file 'archive:/CAB-…' is corrupted! [Position out of bounds!]`。

> 自检方法：改完先做「拆 → 重建」，要求重建结果与原 raw **逐字节相同**。这条能拦住上面全部三种错。

## 4. 缓存：改了 catalog 却没生效的隐形原因

Addressables 首次读到某个 mod 的 catalog 后会缓存到

```
/sdcard/Android/data/<包名>/files/com.unity.addressables/<数字>.json + .hash
```

`<数字>` 是由 **mod 路径**算出来的，跟内容无关。所以换了 mod 内容、缓存名不变，游戏继续用旧 catalog。

**每次换 mod 都要删这两个文件**，否则你会以为改动没生效。

## 5. 运行时补注册是死路

如果 mod 里用到了游戏自定义的「动态属性修改器」之类的资源（通常由某个 helper 用字符串 key 从静态字典里取），那个字典只在**mod 初始化**时被填充。

- 运行时用 `LoadAssetFromMod` 之类去取一个「后来才加进去的资源」→ 取不到；
- 强行返回 null → 调用方 `.IsValid` 抛 `NullReferenceException` → 连物品都用不了。

**唯一可行的路：复用包里在构建期就已经注册好的那个槽位**，把数据改写成你要的。这种槽位往往只有几个，先用 dump 把所有 `DynamicModifier` / `Patch` 类资源列出来，挑一个不用的改。

## 6. 「空 patch」是个隐形炸弹

数据 mod 编辑器通常会把**整张表**导出，即使一条都没改。这类空 patch 作为 patch 资源加载后会**覆盖**游戏原表，等于把那张表清空。

移植前逐张看一眼：内容全是 `{"key": [], "value": ""}` 这种的，直接别带。

## 7. 三个物品容器不是一回事

游戏里的角色对象常有两个以上「非背包」容器，命名相近但用途完全不同（例如「暗取池」「死亡掉落池」）。它们的字段偏移通常只差 8 字节，容易抄错。

要判断改动影响哪个功能，看**谁在读**：

- 面板/UI 读哪个字段，就把东西放进哪个包；
- 但拿走的动作可能作用在另一个包上 —— 只「复制给 UI 看」而不扣原件，就会出现「同一件东西能拿两次」；
- 正确做法：复制一份给 UI 显示，**真的被拿走时按数量从原件扣掉**；扣多少 = 造副本时的数量 − 成交后的剩余数量。

## 8. 装机闭环

- 换 mod 后删缓存（§4）；
- 装完核对 `lastUpdateTime`（`dumpsys package <包名>`），确认跑的是新包而不是上一版 —— 这一条救过好几次「改了没效果」；
- 抓日志：`adb logcat -s Unity:V <你的 tag>:V *:S`，盯 `CRC Mismatch`、`corrupted`、`Position out of bounds`、`Exception`、以及你自己打的诊断行。

## 9. 诊断日志要写在「静默 continue」旁边

最浪费时间的错误是「什么也没发生」—— 读到了但跳过、复制失败、加包失败，如果这些分支都是 `continue`，你只能靠猜。

凡是有「跳过」的地方，都打一行 `谁 / 哪个 id / 为什么跳过`。这次排查一轮可疑功能，就是靠补了这三类日志才一眼定位。
