# UEKit · 开发约定

给在这个仓库里工作的 AI 编码助手（Codex、Claude Code 等）和开发者看。`CLAUDE.md` 只引用本文件，规则只改这一份。

这份文件每次对话都会读，只放所有改动都适用的规则；某个插件、某块协议的细节在「文档地图」里，用到再读。

## 开工前

1. **切到 `dev`**：`git fetch origin && git switch dev`（本地没有就 `git switch -c dev origin/dev`），`git status --short --branch` 确认在 `dev` 上再动手。
2. **想清楚改哪个插件**：一次改动尽量只动一个插件；跨插件的（例如 UELink 加接口、扩展插件用上它）分成两个提交，先改被依赖的那个。
3. **读专题文档**：按下面的文档地图，只读这次用得上的。改协议、加操作前一定先读 [docs/protocol.md](docs/protocol.md)。

## 项目

UEKit 是放进 UE 项目 `Plugins/` 的插件合集。Agent 面板的 UE 页可以按项目安装、升级、卸载这里的插件：

| 插件 | 类型 | 引擎 | 说明 |
| --- | --- | --- | --- |
| `plugins/UELink` | 基础 | 5.4+ | 让 Agent 面板连上 UE 进程（HTTP `/rpc` + 推送通道），其他插件也通过它和面板通信。协议见 [docs/protocol.md](docs/protocol.md)，**改协议先改这份文档** |
| `plugins/AssetForge` | 扩展 | 5.4+（状态树只在 5.8） | 用 JSON 配置管理资产：导入、导出、演练、同步状态、属性差异。见 [docs/asset-forge.md](docs/asset-forge.md) |
| `plugins/GameTest` | 扩展 | 5.4+ | 用 JSON 场景跑游戏测试、录制回放。见 [docs/game-test.md](docs/game-test.md) |

### 谁在用这些插件

- **Agent 面板的 UE 插件**（`fixcode-h/agent-plugins` 的 `ue/`）：走 HTTP `/rpc`，负责安装 / 卸载、网页界面（AssetForge 页、GameTest 页），并把能力做成 MCP 工具（`ue_*`、`ue_forge`、`ue_gametest`）交给 Agent。**所有 UE 侧能力都经 UELink 到面板，再由面板给 Agent**，扩展插件不直接面向 Agent。
- **UEToVS 不连 UE**（0.7.0 起）：它只管 VS 里的代码体验和给 Agent 的代码信息，日志、Live Coding、测试结果都不经过它。推送通道里那几条旧的一次性命令（`openAsset`、`liveCoding`、`runTests`）是给旧版 UEToVS 留的，新功能不要再往那边加。
- 面板按 `catalog.json` 查新版本。改了操作名、参数、返回格式，要同时在协议文档里写清楚，并去面板那边跟上（工具的说明、`useWhen` / `avoidWhen`）。

### 目录

```
plugins/<插件>/
  <插件>.uplugin        UE 认的清单（字段由 UE 定，不往里加自己的字段）
  kit.json              插件库自己的元数据：说明、类型、引擎范围、依赖、面板工具……
  CHANGELOG.md          这个插件的更新日志，发布说明从这里来
  Source/<模块>/        每个模块一个目录，和 .uplugin 的 Modules 一一对应
scripts/                check / package / catalog（只用 Node 自带模块，CI 不 npm install）
docs/                   协议、各插件说明、更新日志写法
catalog.json            发布后 CI 生成并提交回 main，**不手改**
dist/                   出包产物（已忽略）
```

### kit.json 字段

| 字段 | 说明 |
| --- | --- |
| `displayName` / `description` | 面板「UE 插件」列表上显示的名字和一句话说明（中文） |
| `kind` | `base`（UELink）或 `extension`（挂在 UELink 上的扩展） |
| `versionHeader` | `{ file, macro }`：版本宏的位置，check 会核对它和 `.uplugin` 一致 |
| `engineMin` / `engineTested` | 最低引擎版本（「主.次」，面板安装时按它拦）、实测过的版本。`.uplugin` 不写 `EngineVersion`：写了别的引擎版本打开工程会弹「插件不兼容」（`check.mjs` 会拦） |
| `requires` | 依赖的本库插件和最低版本，如 `{ "UELink": ">=2.2.0" }`；扩展必填 UELink |
| `enginePlugins` / `optionalPlugins` | 依赖的引擎插件、可选的引擎或项目插件（没有时对应模块不编） |
| `replaces` | 这个插件替代的旧插件目录名，面板安装时提示删掉 |
| `panelTools` | 面板上用到这个插件的 MCP 工具名，装了才挂 |

## 硬规则

- **不改引擎源码，也不要求改项目代码**：只用引擎公开的扩展点（子系统、委托、`IInputProcessor`、`UDeveloperSettings`、ToolMenus……），所有代码收在插件里。不做 vtable / detour 之类的二进制 hook。
- **扩展插件只通过 UELink 对外**：
  - `.uplugin` 的 `Plugins` 里声明依赖 UELink，`kit.json` 的 `requires` 写 UELink 的最低版本。
  - 模块启动时 `FAgentPanelBridge::Get().RegisterExtension(名字, 版本宏)`，操作用 `AddExtensionOp("前缀.名字", ...)` 挂上；关闭时 `RemoveOp` / `UnregisterExtension`。
  - **不自己开 HTTP 服务、不自己写名片、不自己读令牌**（`check.mjs` 会拦 `GetHttpRouter`）：UELink 已经管了令牌、只听本机和多实例。
  - 操作名前缀一个插件一个：AssetForge 用 `forge.`，GameTest 用 `gt.`。新插件先在协议文档里登记前缀。
- **界面在 Agent 面板的网页里做**：UE 里不放 Slate 窗口和停靠页；命令行（commandlet）和内容浏览器右键菜单可以留，右键菜单只做「马上执行」的动作，不弹窗口。插件自己的界面状态（筛选、分组、字号）归网页，插件里不加 `UDeveloperSettings` 存个人偏好。
- **引擎版本差异只写在 `<插件>Compat.h`**：用 UE 自带的 `ENGINE_MAJOR_VERSION` / `ENGINE_MINOR_VERSION` 或 `UE_VERSION_NEWER_THAN` / `UE_VERSION_OLDER_THAN` 定义成插件自己的宏或内联函数；Build.cs 里用 `Target.Version`。业务代码不直接判断版本（`check.mjs` 会拦）。插件都以 5.4 为基准、同时兼容 5.8：默认写 5.4 的写法，新接口只在实测编过的 5.8 上用，中间版本不去猜。
- **可选依赖拆成单独模块**：CommonUI、GAS、EnhancedInput、UnLua 这类不一定每个项目都开的插件，放进 `<插件><依赖>` 模块，`.uplugin` 里依赖标 `Optional`，没有时整块不编；主模块只认接口（注册表、委托），不 include 可选模块的头。
- **UnLua 是可选的**：`UELinkCore.Build.cs` 扫项目里 UnLua 的头文件选接口（用户的 UnLua 可能是二次开发过的分支），不要假设官方 UnLua 的接口。
- **项目侧接口不随手改名**：UELink 的类名 `FAgentPanel*`、`UAgentPanel*`、头文件名、commandlet 名 `-run=AgentPanel`、元数据 `meta=(AgentPanel)` 是项目代码在用的接口，改名要先在协议文档里写好迁移办法，并且面板跟上。
- **不带某个项目的东西**：插件代码、默认配置、注释、测试里不出现具体游戏项目的名字、路径和资产（原项目前缀 `Tk`、`/Game/某项目/...` 之类）。JSON 配置、场景这类给人看和改的文件跟着项目的文档根走（和 Agent 面板的约定一致：项目外层同级有 `ProjectDoc/docs` 就放 `ProjectDoc/docs/<插件>`，否则放项目里的 `docs/<插件>`），不放 UE 的 Config；设置项留空表示自动，填了按填的；依赖项目资产的测试在资产不存在时跳过，不失败。`check.mjs` 会拦 `Tk` 前缀。
- **线程**：所有操作都在游戏线程上执行；推送通道的套接字是非阻塞的，客户端卡住不能卡编辑器。超过几百毫秒的操作（批量导入、跑测试场景）马上返回作业号，每帧做一点，客户端用 `<前缀>.job` 查进度、`<前缀>.stop` 取消，进度变化走推送事件，不让客户端轮询。
- **Shipping 不加载**：UELink 的 `UELinkCore` 在 Shipping 下不加载，扩展插件同样不进 Shipping（模块类型用 Editor / UncookedOnly / DeveloperTool，或 `TargetConfigurationDenyList` 拦 Shipping；`TargetDenyList` 只能填 Game、Editor 这类目标类型，写 Shipping 会让 UBT 直接报错，`check.mjs` 会拦）。
- **凭据不进仓库**：令牌、密码、API key 不写进代码、测试、文档和提交说明。临时文件只放 `.tmp/`（已忽略）或系统临时目录，用完清理。
- 注释用中文，写「为什么」。文件一律 UTF-8 不带 BOM（`check.mjs` 会拦）。

## 操作（/rpc）的写法

- 加 HTTP 操作：在对应文件里 `AddOp(...)`（UELink 自己）或 `AddExtensionOp(...)`（扩展），一个领域一个 `*Ops.cpp`，不堆进模块入口文件。
- **出错返回 false 并写清楚原因**，原因用中文、说明怎么办（会原样显示给用户和 Agent）。旧 UEToVS 协议里那几句英文报错（`unknown command`、`missing path` 等）是客户端判断用的，不要改。
- **返回要短**：默认不超过 4000 字；读结构的（蓝图、属性表、场景）最多 12000 字，并且要能分页或过滤；日志、报告、截图这类长内容写进 `Saved/<插件>/` 下的文件，只回路径和条数（和面板 ADR 0067 一致）。
- 会改资产或项目文件的操作要支持演练（`dryRun: true` 只返回计划不落盘），并在返回里写清楚改了哪些文件。
- 加推送事件或能力：往 `FUELinkStream::Features()` 里加名字，旧客户端看不到不认识的名字就不会用；扩展的事件名也带自己的前缀。
- 改了操作名、参数或返回字段：同一个提交改 [docs/protocol.md](docs/protocol.md)（或插件文档里的操作表），删掉或改名的操作让旧调用返回一句「改用哪个」的错误。

## 版本和发布

- 改了哪个插件就升哪个插件的版本：CHANGELOG 最上面加 `## vX.Y.Z`，同时改 `.uplugin` 的 `VersionName`、`Version`（加 1）和版本宏。最上面的版本已经发布过（有 `<插件>-v<版本>` 标签）就新开一节，不往已发布的版本里加条目。写法见 [docs/changelog.md](docs/changelog.md)，`node scripts/check.mjs` 会核对。
- **云端编不了 UE 的 C++**：改了 C++ 要在本地对应版本的 UE **完整编译**（Live Coding 不够）验证后才能发正式版；没验证过的版本号必须带 `-beta.N`，合到 `main` 后只发成预发布，客户端查更新默认只认正式版。编译通过后去掉 `-beta` 再发正式版。
- **每次更新，用户都要在本地重新编译**，所以攒一批改动再发版，不要一个小改动发一版。
- 发布由 CI 做（`.github/workflows/release.yml`）：合到 `main` 后按每个插件 CHANGELOG 最上面的版本，标签不存在就打包发 Release，说明 = CHANGELOG 这一节 + 上一版以来改了这个插件目录的提交；然后重写 `catalog.json` 提交回 `main`。只有 UELink 的正式版设成 Latest（老客户端查 `releases/latest` 用）。也可以在 Actions 页手动运行「发布」，选插件、选只打包不发布（包作为运行产物下载）。
- 出包只打源码：`kit.json`、`CHANGELOG.md` 不进包；`Binaries/`、`Intermediate/`、`Saved/` 不进包也不进仓库。

## 加一个新插件

1. 建 `plugins/<名字>/`：`<名字>.uplugin`、`kit.json`、`CHANGELOG.md`（第一版 `## v1.0.0-beta.1`）、`Source/<名字>/`（含 `<名字>.Build.cs` 和 `IMPLEMENT_MODULE`）、版本头 `<名字>Version.h`、`<名字>Compat.h`。
2. 扩展插件：`.uplugin` 依赖 UELink，`kit.json` 写 `requires.UELink`，在协议文档登记操作前缀。
3. 写 `docs/<插件>.md`（能力、配置格式、操作表、限制），在本文件和 README 的插件表里加一行。
4. 面板那边（`agent-plugins` 的 `ue/`）跟上：「UE 插件」列表认得它、需要的网页和工具按准入四问加。
5. `node scripts/check.mjs && node scripts/package.mjs` 通过再提交。

## 测试

- 自动化测试放进插件的测试模块（`<插件>Tests`，类型 `UncookedOnly` 或 `DeveloperTool`，不进 Shipping），测试名用 `<插件>.<功能>.<用例>`，在编辑器「会话前端 → 自动化」或 `-ExecCmds="Automation RunTests <插件>"` 里跑。
- 测试用的探针控件、示例资产放测试模块或 `Content/Test/`，不放进运行时模块。
- 云端只能跑 `scripts/` 的检查；C++ 和自动化测试要本地跑，提交说明里写清楚跑没跑。

## 提交前

1. `git status --short` 看清楚要提交的文件：新文件都 add 了，`dist/`、编译产物、临时文件没混进来。
2. `node scripts/check.mjs && node scripts/package.mjs`，看一眼 `dist/<插件>/notes.md` 的更新说明对不对。
3. 改了 C++ 的，提交说明里写清楚本地有没有编译过、用的哪个 UE 版本、跑了哪些自动化测试。
4. 改了操作或协议的，确认 `docs/protocol.md` / 插件文档同一个提交里改了。

## 提交说明

Release 说明会列出「上一版以来改了这个插件目录的提交」，所以提交标题写给用户看：概括这次改动，前缀写插件名，例如 `AssetForge：导入支持演练`、`UELink：名片加扩展列表`。用户感受不到的改动（文档、脚本、CI、格式）标题以 `内部：` 开头，不进更新说明。

## 分支和合并

- 所有开发都在 `dev` 上做，不在 `main` 上直接改、提交或推送。误在 `main` 上改了，先切回 `dev`，不用 `git checkout .`、`git reset --hard` 清掉别人的改动。
- 推送前先 `git fetch origin && git merge origin/dev`，不用 rebase（含 `git pull --rebase`），除非用户明确要求。
- 合并到 `main` 走 `dev` → `main` 的 PR（已有未合并的就复用），检查通过后普通合并（merge commit），**合 main 前先问用户**。PR 冲突、检查失败或分支保护拦截时停下来报告，不绕过检查、不直接往 `main` 推。
- 推到 `dev` 和每个 PR 都跑「检查」（`.github/workflows/ci.yml`：check + package）；发往 `main` 的 PR 还会检查改过的插件是不是升了版本。

## 文档地图

| 要做的事 | 先读 |
| --- | --- |
| 协议：名片、令牌、`/rpc`、推送通道、扩展接口、安装约定 | [docs/protocol.md](docs/protocol.md) |
| 写 CHANGELOG、定版本号 | [docs/changelog.md](docs/changelog.md) |
| AssetForge 的配置格式、kind、操作 | [docs/asset-forge.md](docs/asset-forge.md) |
| GameTest 的场景格式、选择器、动作、录制回放 | [docs/game-test.md](docs/game-test.md) |
| 面板那边的工具规则（准入四问、长度上限） | 面板仓库 `docs/adr/0067-mcp-tool-rules.md` |

## 常用命令

```bash
node scripts/check.mjs [--plugin A,B]     # 版本四处一致、模块和清单对得上、扩展规则、旧前缀、BOM
node scripts/package.mjs [--plugin A,B]   # 出包到 dist/<插件>/：源码 zip、manifest.json、notes.md、SHA256SUMS
node scripts/catalog.mjs                  # 用 dist/ 的 manifest 重写 catalog.json（CI 发布后跑，本地一般不用）
```
