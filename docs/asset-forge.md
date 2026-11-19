# AssetForge

用 JSON 配置管理 UE 资产：配置是唯一真源，导入把配置写成资产，导出把资产写回配置。支持 UE 5.4 和 5.8（以 5.4 为基准，版本差异收在 `AssetForgeCompat.h`）；状态树（`state-tree`）依赖 5.6 才有的 PropertyBindingUtils，只在 5.8 上编，5.4 上没有这一类。要 UELink 2.4.0 起（之前的版本在 5.4 上编不过）。界面在 Agent 面板 UE 页的「AssetForge」页，Agent 通过面板的 `ue_forge` 工具用同一套操作。

## 支持的资产

17 种 kind，登记在 `Source/AssetForge/Registry/ForgeKindRegistry.cpp`（全插件唯一知道「有哪些资产类型」的地方）：UMG 控件蓝图、Actor 蓝图、对象蓝图、动画蓝图、动画序列（含蒙太奇）、行为树、状态树、数据资产、数据表、材质、Niagara 系统、纹理、字体、地图（世界设置）、骨骼、骨骼网格体、物理资产。`forge.kinds` 返回实际列表。

加一种资产：在登记表里加一行（kind 名、中文名、配置子目录、资产类、导入 / 导出函数、属性差异支持），其他地方都只读这张表。

## 配置

- 配置根默认跟着项目的文档根：项目外层同级有 `ProjectDoc/docs`（P4 / SVN 项目的文档常放在项目外）就是 `ProjectDoc/docs/AssetForge`，否则是项目里的 `docs/AssetForge`。要换就在项目设置 → 插件 → Asset Forge 里指定（相对项目根，`../` 可以指到项目外，也接受绝对路径），各 kind 在子目录里（`umg/`、`data/`……），路径和资产路径对应：`/Game/UI/WBP_HUD` ↔ `umg/UI/WBP_HUD.json`。
- 文件格式：`{ "kind", "schemaVersion": 1, "revision", "asset", "parent"?, "root": { "class", "name", "props", "slot"?, "children"? } }`，属性名用 UE 属性名原样。允许 `//` 注释。
- **子界面（`ref`）**：UMG 配置里某个节点写 `"ref": "相对路径.json"`（不写 `class`）表示这个位置放另一份 UMG 配置生成的子 WBP，类名由被引用配置的 `asset` 推出（`<asset>.<资产名>_C`），一个页面太大时按块拆成几份。路径先按引用它的配置所在目录找，找不到再按项目根、配置根找；引用成环报错。**导入父配置时先导入它引用的子配置**（批量导入按依赖排序，子界面在前，深度优先、去重），父界面生成时子界面的类已经在了；演练时子界面还没导入只给警告（演练看不到子界面里的控件），真导入时子界面导入失败就报错停下。
- **按段排列**（1.6.0 起）：一个资源一个 JSON，顶层字段就是「段」，按固定顺序：头（`$schema`、`//`、`kind`、`schemaVersion`、`revision`、`asset`、`parent`，以及 `skeleton`、`blackboard` 这类小字段）→ `styles` → `root` → 各 kind 登记的大段（UMG 的 `animations`；状态树的 `parameters`、`evaluators`、`globalTasks`、`bindings`）。手写顺序不对也能读；导出到配置时按这个顺序重排顶层字段（字段内容不动），和磁盘上内容相同的文件不重写（不动修改时间，P4 不用签出）。每种 kind 的段登记在 `Registry/ForgeKindSchema.cpp` 的形状里（`Sections`），`forge.kinds kind=` 的 schema 在 `x-forge.sections` 列出 `[{name, description}]`；`forge.describe section=<段名>` 只读一段（头段叫 `header`）。
- **关键帧简写**（1.6.0 起）：UMG 动画的 `keys` 里 `[时间, 值]` 或 `[时间, 值, "cubic|linear|constant"]` 等价于 `{ "time", "value", "interp" }`，两种可以混写；导出一律写简写（插值是缺省的 cubic 时只写两项）。
- **样式复用**（1.6.0 起）：顶层 `styles: { "NavButton": { "props": {…}, "slot": {…} } }` 定义命名样式，节点写 `"style": "NavButton"` 或 `["Base", "NavButton"]`（后面的覆盖前面的）引用，节点自己写的 `props` / `slot` 再覆盖样式。合并时对象按字段递归（`"Padding": { "Left": 8 }` 只改左边，其余取样式），数组和标量整个覆盖，字段名按导入的规则认（`bIsEnabled` 和 `IsEnabled` 是同一个）。样式在文档解析阶段展开，对所有 kind 都生效（props / slot 是通用的节点字段），后面的演练、差异、导入看到的都是展开后的。样式里只能写 `props`、`slot`；引用不存在的样式名报错，路径指到节点的 `style`。导出不反推样式，但保留原配置里手写的：`styles` 原样写回，节点按 name 认回原来的 `style` 引用，和样式相同的属性不再重复写。注意：资产里某个属性回到了默认值（导出时省掉），而样式里写了它，重新导入时还是样式的值。
- **include**（1.6.0 起）：某一段或某个子节点可以放进片段文件，导入时原样展开，还是同一个资产（和生成子 WBP 的 `ref` 不同）。
  - 段：`"animations": { "include": "WBP_Shop.parts/animations.json" }`，片段写 `{ "fragment": "animations", "animations": [ … ] }`；任何大段（`styles`、`root`、`animations`……）都行，头字段不行。
  - 节点：`children` 里写 `{ "include": "WBP_Shop.parts/Side.json" }`，片段写 `{ "fragment": "node", "node": 一个节点或节点数组 }`，展开到 include 的位置。
  - 片段顶层有 `fragment`、没有 `kind`，两者互斥（同时有报错）；扫描配置根时片段永远不当资源。`fragment` 说明它能放在哪，放错报错（动画片段不能 include 到控件树里）。片段只能写 `fragment`、同名字段、`//` 和 `$schema`；include 对象只能写 `include`（和 `//`）。
  - 路径相对引用它的文件，约定放在主文件旁边的 `<资产名>.parts/`（不强制）。片段还能再 include 片段，成环报错；同一个片段在一份配置里引用多次、或被几份配置引用都给警告（复用请用 `styles` 或 `ref`）。
  - 读的时候先展开 include 再走后面的流程，演练、差异、导入、哈希都按展开后的文档；来自片段的诊断路径带上片段文件名：`WBP_Shop.parts/animations.json: animations[0].tracks[0].channel`。
  - 写回（导出到配置）保留 include 结构：整段 include 的新内容写回那个片段；子节点按 name 认回原来的片段（include 留在它第一个节点的位置），片段里没有的新节点写在引用它的文件；片段的节点都没了时 include 还留着、片段写成空数组。片段读不了时那部分写回引用它的文件并给警告，片段成环时整份写主文件。没变的片段不重写。
  - 实现集中在文档来源层 `Core/ForgeDocSource`（展开、写回、诊断加文件名），别处看不见 include。
- **编辑器边写边检查**（1.6.0 起）：编辑器启动时把每种 kind 的 JSON Schema 写到配置根 `.schema/<kind>.schema.json`，片段用的是 `.schema/fragment.schema.json`（内容和 `forge.kinds kind=` 回的一样；没变不重写；`.` 开头的目录扫描时跳过）。导出时配置和片段头部写 `"$schema": "<到 .schema 的相对路径>/<kind>.schema.json"`，VS Code / Rider 改配置时字段拼错标红、通道和插值给补全；读的时候忽略 `$schema`。Schema 里有段、include / fragment、关键帧两种写法、UMG 动画通道枚举、节点 `style`。
- 同步清单在 `Saved/AssetForge/state.json`（本机状态，不进版本管理）：记每份配置的内容哈希和导入时资产文件的修改时间，用来快速判定状态。没有 include 的配置按去掉注释的文本算哈希，有 include 的按展开后的文档算（片段改了也算「配置已改」）。

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
| `forge.entries` | `unmanaged`（默认 true）、`deep`、`kind`、`state`（状态 id 或 `needsImport`）、`query`、`offset`、`limit`（默认 200，最多 1000） | `entries`、`total`、`offset`、`states`（各状态计数）、`kinds`（各 kind 计数）。条目有 include 时带 `includes`（片段路径，相对配置根）；片段被几份配置引用时 `message` 里提示 |
| `forge.describe` | `config` 或 `asset`；`section`?（段名，`header` 是头段） | `managed`、`entry`、`stateLabel`、`stateDescription`、`sections: [{name, include?}]`（文档里有的段，整段来自片段时 `include` 是片段路径）、`configText`（主文件原文，超长时 `configTextTruncated`）；给了 `section` 时不回 `configText`，改回 `section`、`sectionText`（展开 include 后这一段的 JSON：`{ "段名": 内容 }`，头段是头字段组成的对象；超长时 `sectionTextTruncated`），没有这段时报错并列出有的段；没纳管的资产回 `exportable`、`kind` |
| `forge.plan` | `config`、`configText`?（没存盘的内容） | `report`（`actions`、`diagnostics`、`created/updated/deleted`）、`state` |
| `forge.diff` | `config`、`configText`?、`baselineText`?（和这份配置比，面板从 git 取上次提交的版本） | `diff: {supported, reason?, baseline: asset\|config, changes: [{property, before, after}], total, truncated?}` |
| `forge.import` | `configs: [...]` 或 `pending: true`；`dryRun` | `job`、`total`（作业在后台跑） |
| `forge.export` | `assets: [...]` | `job`、`total` |
| `forge.job` | `id`?（不给取最近一个）、`reports`（最多 50） | `job: {id, title, mode, status, total, finished, succeeded, failed, skipped, round?, current?, reports?}` |
| `forge.stop` | `id` | `job` |

### 配置的 JSON Schema（`forge.kinds kind=`）

给 Agent 写配置前取（面板 `ue_forge action=schema`），也写成配置根 `.schema/<kind>.schema.json` 给编辑器用。按 JSON Schema 2020-12 写：顶层的 `$schema`、`kind`、`schemaVersion`、`asset`、`parent`（用不用、必不必写因 kind 而异）、`styles`、文档级扩展字段（`skeleton`、`blackboard`、`animations`……，大段都可以写成 `{ "include": … }`）；`$defs.include` 是 include 对象；`x-forge.sections` 是这种 kind 的段和顺序；`$defs.root` 是根节点（固定的 `class` 写成 `const`，没有子节点的 kind 写 `children.maxItems: 0`），`$defs.node` 是子节点。`props` 里的属性不展开（一个类几百个属性），`x-forge.propsClass` 给出属性来自的类时，再用 `forge.kinds class=` 取属性表；`x-forge.notes` 是这种配置最容易踩的坑。

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

- 界面 `umg/WBP_ForgeSample.json` 只有外壳（侧栏、顶栏、弹窗），每一页是 `umg/pages/WBP_ForgeSample_<页>.json` 的子 WBP（1.5.0 起，用 `ref` 引用，导入父界面会连带导入）。父类按名字在父界面和所有子界面里找控件，所以控件名在整套界面里不能重名（生成脚本会查）。
- 界面 `umg/WBP_ForgeSample.json` 的父类 `ForgeSampleScreen`、地图 `map/Map_ForgeSample.json` 的 GameMode `ForgeSampleGameMode` 都在测试模块 `AssetForgeTests/Sample/`（UncookedOnly，只在编辑器和 PIE 里有），所以示例资产不能打进包。
- 跳页按控件名约定（`Go_`、`Back`、`Open_` / `Close_`、`Pick_`、`Submit_`，`__` 后缀只为不重名），细节见 `docs/AssetForge/Test/README.md`。
- 配置由仓库根的 `docs/asset-forge-sample/gen.mjs` 生成（控件名和场景里的选择器路径一起算），改界面改脚本再生成：页面在 `ui-pages.mjs`、`ui-new-pages.mjs`，积木在 `ui-kit.mjs`，场景在 `scenarios.mjs`。生成后把 `_GameTest/` 整个目录拷到面板插件 `agent-plugins` 的 `ue/server/uekit/fixtures/forge-sample/`，跑那边的 `gt-samples.test.ts` 做离线校验。
- 「控件」页（1.5.0 起）每种能操作的控件各一个：普通 / 禁用到可用 / 长按 / 悬停按钮、单行和多行输入框、滑条和进度条、微调框、下拉框、复选框、单选按钮组、按键选择器、长滚动框、切换器、菜单锚点（菜单内容是 C++ 界面 `ForgeSampleMenu`）。「流程」页给脚本当靶子：计数、领取（点过变「已领取」）、延时出现、显示切换、定时弹出的公告。名字约定见 `docs/AssetForge/Test/README.md`，实现在 `Sample/ForgeSampleFlow.cpp`。
- 控制台命令 `AssetForge.Sample`：在当前游戏世界打开 / 关掉示例界面。
- 「列表条目」页（1.4.0 起）是 GameTest「条目标识」的靶子：仓库 TileView（数据对象有 `ItemId`，录制时自动认）、邮件 ListView（只有 `SentAt` 不重复，录制时暂停问）、技能栏 WrapBox（运行时生成的格子，要 `SkillType` + `Level` 组合，录制时暂停问）。数据对象和条目控件在 `Sample/ForgeSampleLists.cpp`，条目控件是 C++ 类、控件树在代码里搭，配置里只有空容器；配套场景 `_GameTest/ForgeSample-条目标识.json`。
- 配套场景（GameTest 1.9.0 起的 v3 脚本）：`冒烟`（跳页、弹窗、返回）、`条目标识`（按数据认条目、select、scroll）、`控件操作`（每种控件的 set / type / 点击 / 菜单和观察类动作）、`流程脚本`（块、条件、表达式、守护、invoke、接口、skipIf）、`真实输入`（真实鼠标键盘：悬停、长按、拖动、滚轮、下拉框选项、按键），都先 `call AssetForgeSample/common/打开示例界面`，单独跑哪份都行。

## 其他入口

- **内容浏览器右键菜单**：从配置导入、导出到配置、在 Agent 面板中显示。
- **命令行**：`UnrealEditor-Cmd.exe <项目>.uproject -run=AssetForge -status | -pending | -config=<文件> | -export=<资产> [-dry-run] [-json]`，`-json` 时结果一行打到标准输出，前缀 `ASSETFORGE_JSON:`。

## 限制

- 云端编不了 C++：每一版都要在本地 UE 5.4 和 5.8 各完整编译一次验证；没验证过的版本照样发正式版，在 CHANGELOG 那一节开头写明。
- 自动化测试（`AssetForge.*`）里依赖项目资产的用例，在 `/Game/AssetForgeTest/` 下没有对应资产时跳过，见 `Source/AssetForgeTests/ForgeTestAssets.h`。
