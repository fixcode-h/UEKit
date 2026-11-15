# UEKit · 开发约定

给在这个仓库里工作的 AI 编码助手和开发者看。`CLAUDE.md` 只引用本文件。

UEKit 是放进 UE 项目 `Plugins/` 的插件合集，Agent 面板的 UE 页可以按项目安装、升级、卸载这里的插件：

| 插件 | 类型 | 引擎 | 说明 |
| --- | --- | --- | --- |
| `plugins/UELink` | 基础 | 5.4+ | 让本机工具连上 UE 进程（HTTP `/rpc` + 推送通道），Agent 面板和 UEToVS 都用它。协议见 [docs/protocol.md](docs/protocol.md)，**改协议先改这份文档** |
| `plugins/AssetForge` | 扩展 | 5.8 | 用 JSON 配置管理资产：导入、导出、演练、同步状态、属性差异。见 [docs/asset-forge.md](docs/asset-forge.md) |
| `plugins/GameTest` | 扩展 | 5.8 | 用 JSON 场景跑游戏测试、录制回放。见 [docs/game-test.md](docs/game-test.md) |

## 分支和发布

- 所有开发都在 `dev` 上做：`git switch dev`（本地没有就 `git switch -c dev origin/dev`），确认在 `dev` 上再动手；不在 `main` 上直接改。
- 推送前先 `git fetch origin && git merge origin/dev`，不用 rebase。
- 合并到 `main` 走 `dev` → `main` 的 PR，检查通过后普通合并，**合 main 前先问用户**。合并后 CI 按每个插件 CHANGELOG 最上面的版本发 Release（`.github/workflows/release.yml`），说明自动附上这一版包含的提交；也可以在 Actions 页手动运行「发布」，选插件、选只打包不发布。
- 改了哪个插件就升哪个插件的版本：CHANGELOG 最上面加 `## vX.Y.Z`，同时改 `.uplugin` 的 `VersionName`、`Version`（加 1）和版本宏。写法见 [docs/changelog.md](docs/changelog.md)。`node scripts/check.mjs` 会核对。
- **每次更新，用户都要在本地重新编译**，所以攒一批改动再发版。

## 代码

- 云端编不了 UE 的 C++：改了 C++ 要在本地对应版本的 UE **完整编译**（Live Coding 不够）验证后才能发正式版，没验证过的只能发 `-beta.N` 预发布。提交说明里写清楚本地有没有编译过。
- 注释用中文，写「为什么」。发给客户端、会原样显示给用户的报错用中文。
- 所有操作都在游戏线程上执行；长任务马上返回作业号，客户端用 `*.job` 查进度，不阻塞游戏线程。
- **扩展插件**（kind=extension）：
  - `.uplugin` 的 `Plugins` 里声明依赖 UELink，`kit.json` 的 `requires` 写 UELink 的最低版本。
  - 操作用 `FAgentPanelBridge::Get().AddExtensionOp("前缀.名字", ...)` 挂到 UELink 上，模块启动时 `RegisterExtension(名字, 版本)`。**不自己开 HTTP 服务**（check.mjs 会拦）。
  - 界面在 Agent 面板的网页里做，UE 里不放窗口（命令行和内容浏览器右键菜单可以留）。
  - 引擎版本差异只写在 `<插件>Compat.h`，用 UE 自带的 `ENGINE_MAJOR_VERSION` / `ENGINE_MINOR_VERSION` 或 `UE_VERSION_NEWER_THAN` / `UE_VERSION_OLDER_THAN`；Build.cs 用 `Target.Version`。业务代码不直接判断版本（check.mjs 会拦）。
  - 可选的引擎/项目插件（CommonUI、GAS……）放进单独的模块，`.uplugin` 里标 `Optional`，没有时整块不编。
  - **不改引擎源码，也不要求改项目代码**：只用引擎公开的扩展点。
- UELink 的类名 `FAgentPanel*`、`UAgentPanel*`、commandlet 名 `-run=AgentPanel`、元数据 `meta=(AgentPanel)` 是项目侧接口，改名要先在协议文档里写好迁移办法。
- UnLua 是可选的：UELinkCore.Build.cs 扫项目里 UnLua 的头文件选接口（用户的 UnLua 可能是二次开发过的分支），不要假设官方 UnLua 的接口。
- 凭据不进仓库。临时文件放 `.tmp/`（已忽略）。

## 提交前

1. `node scripts/check.mjs && node scripts/package.mjs`（看一眼 `dist/<插件>/notes.md` 的更新说明对不对）。
2. 改了 C++ 的，说明里写清楚本地有没有编译过、用的哪个 UE 版本。
