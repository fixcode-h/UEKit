# AssetForge

用 JSON 配置管理 UE 资产：配置是唯一真源，导入把配置写成资产，导出把资产写回配置。支持 UE 5.4 和 5.8（以 5.4 为基准，版本差异收在 `AssetForgeCompat.h`）；状态树（`state-tree`）依赖 5.6 才有的 PropertyBindingUtils，只在 5.8 上编，5.4 上没有这一类。要 UELink 2.4.0 起（之前的版本在 5.4 上编不过）。界面在 Agent 面板 UE 页的「AssetForge」页，Agent 通过面板的 `ue_forge` 工具用同一套操作。

## 支持的资产

17 种 kind，登记在 `Source/AssetForge/Registry/ForgeKindRegistry.cpp`（全插件唯一知道「有哪些资产类型」的地方）：UMG 控件蓝图、Actor 蓝图、对象蓝图、动画蓝图、动画序列（含蒙太奇）、行为树、状态树、数据资产、数据表、材质、Niagara 系统、纹理、字体、地图（世界设置）、骨骼、骨骼网格体、物理资产。`forge.kinds` 返回实际列表。

加一种资产：在登记表里加一行（kind 名、中文名、配置子目录、资产类、导入 / 导出函数、属性差异支持），其他地方都只读这张表。

## 配置

- 配置根默认跟着项目的文档根：项目外层同级有 `ProjectDoc/docs`（P4 / SVN 项目的文档常放在项目外）就是 `ProjectDoc/docs/AssetForge`，否则是项目里的 `docs/AssetForge`。要换就在项目设置 → 插件 → Asset Forge 里指定（相对项目根，`../` 可以指到项目外，也接受绝对路径），各 kind 在子目录里（`umg/`、`data/`……），路径和资产路径对应：`/Game/UI/WBP_HUD` ↔ `umg/UI/WBP_HUD.json`。
- 文件格式：`{ "kind", "schemaVersion": 1, "revision", "asset", "parent"?, "root": { "class", "name", "props", "slot"?, "children"? } }`，属性名用 UE 属性名原样。允许 `//` 注释。
- 同步清单在 `Saved/AssetForge/state.json`（本机状态，不进版本管理）：记每份配置的内容哈希和导入时资产文件的修改时间，用来快速判定状态。

### 同步状态

| 状态 id | 含义 |
| --- | --- |
| `synced` | 配置和资产一致 |
| `never-imported` | 有配置，本机还没导入过（资产不存在也算这个） |
| `config-changed` | 配置自上次导入后改过（可能只是格式变化，深度检查能确认） |
| `asset-changed` | 资产在编辑器里被改过 |
| `asset-missing` | 导入过，但配置指向的资产后来不存在了 |
| `pending` | 深度检查确认导入会改资产 |
| `parse-error` | 配置解析失败 |
| `foreign` | 不是本插件的配置 |
| `unmanaged` | 资产还没有配置（可以导出纳管） |
| `no-asset` | 配置里没写目标资产 |

## 操作（UELink `/rpc`）

全部在游戏线程执行。返回默认不超过 4000 字：列表分页，报告里的动作和诊断各最多 200 条，配置原文最多 12000 字。

| 操作 | 参数 | 返回 |
| --- | --- | --- |
| `forge.health` | — | `version`、`configRoot`、`configRootExists`、`scanRoots`、`kinds`（数量）、`jobRunning` |
| `forge.kinds` | `kind`?、`class`?、`filter`、`offset`、`limit`（默认 60，最多 150） | 不给参数：`kinds: [{kind, displayName, dir, assetClass, propertyDiff, propertyDiffReason?}]`；给 `kind`：`schema`（这种配置的 JSON Schema，见下）；给 `class`（类或结构体，全路径或短名）：`props: {class, isStruct, properties, total, offset, truncated?, next?}`，只列导入认的属性（和 `ForgePropertyCodec::IsCodecProperty` 同一判据）。两个可以一起给 |
| `forge.entries` | `unmanaged`（默认 true）、`deep`、`kind`、`state`（状态 id 或 `needsImport`）、`query`、`offset`、`limit`（默认 200，最多 1000） | `entries`、`total`、`offset`、`states`（各状态计数）、`kinds`（各 kind 计数） |
| `forge.describe` | `config` 或 `asset` | `managed`、`entry`、`stateLabel`、`stateDescription`、`configText`（超长时 `configTextTruncated`）；没纳管的资产回 `exportable`、`kind` |
| `forge.plan` | `config`、`configText`?（没存盘的内容） | `report`（`actions`、`diagnostics`、`created/updated/deleted`）、`state` |
| `forge.diff` | `config`、`configText`?、`baselineText`?（和这份配置比，面板从 git 取上次提交的版本） | `diff: {supported, reason?, baseline: asset\|config, changes: [{property, before, after}], total, truncated?}` |
| `forge.import` | `configs: [...]` 或 `pending: true`；`dryRun` | `job`、`total`（作业在后台跑） |
| `forge.export` | `assets: [...]` | `job`、`total` |
| `forge.job` | `id`?（不给取最近一个）、`reports`（最多 50） | `job: {id, title, mode, status, total, finished, succeeded, failed, skipped, round?, current?, reports?}` |
| `forge.stop` | `id` | `job` |

### 配置的 JSON Schema（`forge.kinds kind=`）

给 Agent 写配置前取（面板 `ue_forge action=schema`）。按 JSON Schema 2020-12 写：顶层的 `kind`、`schemaVersion`、`asset`、`parent`（用不用、必不必写因 kind 而异）、文档级扩展字段（`skeleton`、`blackboard`、`animations`……）；`$defs.root` 是根节点（固定的 `class` 写成 `const`，没有子节点的 kind 写 `children.maxItems: 0`），`$defs.node` 是子节点。`props` 里的属性不展开（一个类几百个属性），`x-forge.propsClass` 给出属性来自的类时，再用 `forge.kinds class=` 取属性表；`x-forge.notes` 是这种配置最容易踩的坑。

形状写在 `Source/AssetForge/Registry/ForgeKindSchema.cpp`，照各 kind 头文件开头的「配置形状」注释；加 kind 时同时加一项（`AssetForge.Schema` 用例会拦）。Schema 只描述结构，字段写没写对最终以 `forge.plan` 的诊断为准。

同一时间只跑一个作业。作业状态 `status`：`running`、`done`、`failed`（有失败项）、`cancelled`。

### 推送事件（UELink 推送通道，订阅主题）

| 主题 | 什么时候 | 字段 |
| --- | --- | --- |
| `forge.job` | 作业开始、每帧进度、结束 | 和 `forge.job` 返回的 `job` 一样，加 `phase`：`started` / `progress` / `finished` |
| `forge.changed` | 右键菜单导入 / 导出之后 | `source`、`count`：面板重新拉列表 |
| `forge.reveal` | 右键菜单「在 Agent 面板中显示」 | `asset`、`kind`：面板跳到这一项 |

## 示例测试界面

插件目录的 `docs/AssetForge/Test/` 带一套多页面 UMG 界面（给 GameTest 录制练手），编辑器启动时 `Service/ForgeSamples.cpp` 把它装进配置根的 `Test/`，其中 `_GameTest/` 的场景另装进 GameTest 场景根的 `AssetForgeSample/`（项目开了 GameTest 才装）。只补缺的、只更新上次装进去以后没被改过的，装过的文件哈希记在 `Saved/AssetForge/samples.json`，删掉的不再装回来；项目设置「安装示例测试界面」可关。

- 界面 `umg/WBP_ForgeSample.json` 的父类 `ForgeSampleScreen`、地图 `map/Map_ForgeSample.json` 的 GameMode `ForgeSampleGameMode` 都在测试模块 `AssetForgeTests/Sample/`（UncookedOnly，只在编辑器和 PIE 里有），所以示例资产不能打进包。
- 跳页按控件名约定（`Go_`、`Back`、`Open_` / `Close_`、`Pick_`、`Submit_`，`__` 后缀只为不重名），细节见 `docs/AssetForge/Test/README.md`。
- 配置由仓库根的 `docs/asset-forge-sample/gen.mjs` 生成（控件名和场景里的选择器路径一起算），改界面改脚本再生成。
- 控制台命令 `AssetForge.Sample`：在当前游戏世界打开 / 关掉示例界面。

## 其他入口

- **内容浏览器右键菜单**：从配置导入、导出到配置、在 Agent 面板中显示。
- **命令行**：`UnrealEditor-Cmd.exe <项目>.uproject -run=AssetForge -status | -pending | -config=<文件> | -export=<资产> [-dry-run] [-json]`，`-json` 时结果一行打到标准输出，前缀 `ASSETFORGE_JSON:`。

## 限制

- 云端编不了 C++：每一版都要在本地 UE 5.4 和 5.8 各完整编译一次验证，没验证过的版本号带 `-beta.N`。
- 自动化测试（`AssetForge.*`）里依赖项目资产的用例，在 `/Game/AssetForgeTest/` 下没有对应资产时跳过，见 `Source/AssetForgeTests/ForgeTestAssets.h`。
