# AssetForge

用 JSON 配置管理 UE 资产：配置是唯一真源，导入把配置写成资产，导出把资产写回配置。只支持 UE 5.8，依赖 UELink 2.2 起的扩展接口。界面在 Agent 面板 UE 页的「AssetForge」页，Agent 通过面板的 `ue_forge` 工具用同一套操作。

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
| `never-imported` | 有配置，还没导入过 |
| `config-changed` | 配置自上次导入后改过（可能只是格式变化，深度检查能确认） |
| `asset-changed` | 资产在编辑器里被改过 |
| `asset-missing` | 配置指向的资产不存在 |
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
| `forge.kinds` | — | `kinds: [{kind, displayName, dir, assetClass, propertyDiff, propertyDiffReason?}]` |
| `forge.entries` | `unmanaged`（默认 true）、`deep`、`kind`、`state`（状态 id 或 `needsImport`）、`query`、`offset`、`limit`（默认 200，最多 1000） | `entries`、`total`、`offset`、`states`（各状态计数）、`kinds`（各 kind 计数） |
| `forge.describe` | `config` 或 `asset` | `managed`、`entry`、`stateLabel`、`stateDescription`、`configText`（超长时 `configTextTruncated`）；没纳管的资产回 `exportable`、`kind` |
| `forge.plan` | `config`、`configText`?（没存盘的内容） | `report`（`actions`、`diagnostics`、`created/updated/deleted`）、`state` |
| `forge.diff` | `config`、`configText`?、`baselineText`?（和这份配置比，面板从 git 取上次提交的版本） | `diff: {supported, reason?, baseline: asset\|config, changes: [{property, before, after}], total, truncated?}` |
| `forge.import` | `configs: [...]` 或 `pending: true`；`dryRun` | `job`、`total`（作业在后台跑） |
| `forge.export` | `assets: [...]` | `job`、`total` |
| `forge.job` | `id`?（不给取最近一个）、`reports`（最多 50） | `job: {id, title, mode, status, total, finished, succeeded, failed, skipped, round?, current?, reports?}` |
| `forge.stop` | `id` | `job` |

同一时间只跑一个作业。作业状态 `status`：`running`、`done`、`failed`（有失败项）、`cancelled`。

### 推送事件（UELink 推送通道，订阅主题）

| 主题 | 什么时候 | 字段 |
| --- | --- | --- |
| `forge.job` | 作业开始、每帧进度、结束 | 和 `forge.job` 返回的 `job` 一样，加 `phase`：`started` / `progress` / `finished` |
| `forge.changed` | 右键菜单导入 / 导出之后 | `source`、`count`：面板重新拉列表 |
| `forge.reveal` | 右键菜单「在 Agent 面板中显示」 | `asset`、`kind`：面板跳到这一项 |

## 其他入口

- **内容浏览器右键菜单**：从配置导入、导出到配置、在 Agent 面板中显示。
- **命令行**：`UnrealEditor-Cmd.exe <项目>.uproject -run=AssetForge -status | -pending | -config=<文件> | -export=<资产> [-dry-run] [-json]`，`-json` 时结果一行打到标准输出，前缀 `ASSETFORGE_JSON:`。

## 限制

- 云端编不了 C++：每一版都要在本地 UE 5.8 完整编译验证，没验证过的版本号带 `-beta.N`。
- 自动化测试（`AssetForge.*`）里依赖项目资产的用例，在 `/Game/AssetForgeTest/` 下没有对应资产时跳过，见 `Source/AssetForgeTests/ForgeTestAssets.h`。
