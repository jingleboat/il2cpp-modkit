# Addressables `ContentCatalogData` 二进制格式

Unity Addressables 把 catalog 存成 JSON（这里叫 `catalog_0.1.json`），但里面的关键索引不是明文，而是四个 base64 字段装的**二进制 blob**。改 bundle 之后 catalog 必须同步，否则游戏读到的还是旧索引。

下面四段的格式是对照真实 catalog 逐字节验证过的（用「解析 → 原样重建 → 与原文比对」的方式确认）。

---

## 1. `m_KeyDataString` — 键表

```
int32   count
repeat count 次:
    uint8   type        // 0 = ASCII/UTF8；其它值表示带 byte[] 的复杂 key
    int32   len
    bytes   utf8 (len 字节，不含结尾 0)
```

每个 key 在 bucket 里用**偏移**引用，所以 key 的长度会影响所有 bucket 的 `keyOffset`。

## 2. `m_BucketDataString` — 键 → 条目 的映射

```
int32   count
repeat count 次:
    int32   keyOffset        // 指向 KeyDataString 里那个 key 的起始字节
    int32   entryCount
    int32[entryCount]        // 条目下标（指向 EntryDataString 的第几条）
```

**这个「偏移」是本格式最容易翻车的地方**：它不是 key 的序号，而是 key 在 KeyDataString 里的字节位置。所以插入一个新 key 之后，排在它后面的所有 bucket 的 `keyOffset` 都要整体后移。

## 3. `m_EntryDataString` — 条目表

```
int32   count
repeat count 次，每条 7 个 int32（共 28 字节）:
    int32  internalIdIndex    // 指向 JSON 里 m_InternalIds 的下标
    int32  providerIndex      // 指向 m_ProviderIds
    int32  dependencyKeyIndex // 指向 KeyDataString 的 key 下标（不是 byte offset！）
    int32  depHash            // 依赖哈希
    int32  extraDataIndex     // 指向 m_ExtraDataString 里的位置
    int32  primaryKey         // 指向 KeyDataString 的 key 下标
    int32  resourceTypeIndex  // 指向 m_resourceTypes
```

注意 `internalIdIndex` 是**数组下标**，而 `dependencyKeyIndex` / `primaryKey` 是 **key 下标**，两者语义不同，别搞混。

## 4. `m_ExtraDataString` — provider 的附加数据

```
uint8     7                       // 长度前缀（下面这个名字的字节数）
bytes     "AssemblyQualifiedTypeName, Assembly, Version=..., Culture=neutral, PublicKeyToken=null"
bytes     "ClassName"
int32     byteLen                 // 后面 JSON 的字节数
bytes     UTF-16LE JSON           // AssetBundleRequestOptions
```

`AssetBundleRequestOptions` 的 JSON 长这样（字段顺序不固定，按名字匹配）：

```json
{
  "m_Hash": "…32 hex…",
  "m_Crc": 3041377394,
  "m_BundleSize": 109283,
  "m_UseCrcForCachedBundles": true,
  "m_ChunkedTransfer": false,
  "m_RetryCount": 0,
  …
}
```

### 坑：CRC

`m_Crc` 是 bundle 内容的 CRC32。**改了 bundle 内容却不同步这里，游戏会静默拒绝加载整包**，日志只有一行：

```
CRC Mismatch. Provided b547bc72, calculated 177a2799 … Will not load AssetBundle 'xxx.bundle'
```

正确做法（两手）：

1. `m_Crc` 置 **0**（表示不校验）；
2. `m_BundleSize` 改成新 bundle 的真实大小；
3. 如果还有 `m_UseCrcForCachedBundles`，置 `false`。

改完之后还要修那个 `int32 byteLen` —— UTF-16LE 编码后字节数变了，长度前缀不改会读到垃圾。

> 别指望 hook `AssetBundleRequestOptions.get_Crc()` 绕过去。IL2CPP 里这个属性通常被内联，hook 装上了也不会被调用；`AssetBundle.LoadFromFileAsync_Internal` 同理不是实际入口。数据层改是唯一稳的办法。

---

## 5. 新增一个 asset 的完整清单

假设要在已有 mod 的 bundle 里加一个 `MonoBehaviour` 资源：

1. **bundle 内**：新建对象（分配一个不冲突的 `pathID`），加到 `AssetBundle.m_PreloadTable`，再往 `m_Container` 里加一条 `("Assets/_Mods/xxx/路径.asset", AssetInfo(pathID, preloadIndex, preloadSize))`。
   - `preloadIndex` 要指向刚 append 的 `m_PreloadTable` 位置，`preloadSize` 通常 2。
2. **catalog 的 `m_InternalIds`**：append 同样的资源路径字符串。
3. **`m_KeyDataString`**：append 两个 key —— 一个是 `<子目录>/<名字>.asset`（Addressables 用它找资源），一个是资源 hash（32 位 hex）。
4. **`m_EntryDataString`**：append 一条，`internalIdIndex` = 新下标，`primaryKey` = 前面那个路径 key 的下标，`dependencyKeyIndex` = 0，`depHash` 抄一个已有条目的。
5. **`m_BucketDataString`**：append 两个 bucket，分别指向上一步的两条 entry，`keyOffset` 用新 key 的真实字节偏移。
6. **`m_ExtraDataString`**：如果新资源所在 bundle 与已有的一样，可以不动；否则要为它补一份 `AssetBundleRequestOptions`。
7. **自检**：解析 → 重建 → `assert 重建 == 原文`。这一步能拦住 90% 的「改了以后游戏报 corrupted」。

## 6. `m_resourceTypes`

是字符串数组，索引即 `resourceTypeIndex`。每个游戏不一样，必须自己 dump 出来看。常见的有：`IAssetBundleResource`（0）、各种自定义 `AssetReference` 派生类型、以及游戏自己定义的数据 patch 类。

改 `resourceTypeIndex` 就是在告诉 Addressables「这个资源用什么 provider 加载」，填错会得到「资源找到但加载出 null」。
