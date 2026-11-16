# UELink 更新日志

格式说明见仓库根目录的 `docs/changelog.md`（`scripts/check.mjs` 和发布流程依赖这个格式）。

## v2.3.1-beta.1

> 还没在本地用 UE 完整编译验证过，先发预发布。

### Bug 修复
- **UE 5.4 下编译不过** — 编辑器模块单体构建时，trace 分析那份代码的 `using namespace TraceServices` 漏进了同一编译单元后面的文件，引擎 ContentBrowserData 头里的 `FFrame` 报「不明确的符号」（C2872）。改成只在自己的命名空间里生效
- **资源校验的弃用警告** — `asset.validate` 改调 const 版的 `IsDataValid`，5.4 上不再报 C4996

### 改进
- **以 UE 5.4 为基准** — 各处 5.8 才有的接口统一收进 `UELinkCompat.h` 的版本宏，默认走 5.4 的写法

## v2.3.0

> 按用户要求直接发正式版：发布前还没在本地用 UE 完整编译验证过，装好后编译有问题请反馈。

### 新功能
- **资源文件夹树** — 新操作 `assets.children`：照 Content Browser 按文件夹一层层列（`/` 给 Content、项目插件、Engine 这些挂载根），只读资源注册表、不加载资源；Agent 面板资源页的文件夹树点开哪层才读哪层，大项目也不卡

### 注意
- 装好后要在本地完整编译一次（Live Coding 不够）

## v2.2.0-beta.1

> 搬进 UEKit 插件库后的第一版。还没在本地用 UE 编译过，先发预发布（客户端查更新只认正式版）。

### 新功能
- **扩展插件接口** — UEKit 里的扩展插件（AssetForge、GameTest）把自己的操作挂到 UELink 的 `/rpc` 上（带前缀，如 `forge.import`、`gt.run`），共用令牌和只听本机；名片和 `/info` 多了 `extensions`，列出这个进程里加载了哪些扩展、什么版本
- **扩展推送事件** — 扩展可以经推送通道推实时状态（作业进度、录制到的一步），客户端订阅 `forge.job` 这类主题即可，不用轮询；`features` 里多了 `extEvents`
- **蓝图资源的组件树** — `components` 传蓝图资源时，除了 C++ 组件也列出蓝图里加的组件（含父蓝图的），按挂接关系给出父组件，Agent 面板资源页据此画组件树、点组件改它的默认值
- **世界大纲和 UObject 树** — 新操作 `outliner`（一个世界的全部 Actor：名字、类、文件夹、挂接）和 `objects.children`（按外层一层层列非 Actor 对象），只给名字和层级不读属性；Actor 增删、挂接变化时推 `uelink.outliner` 事件，客户端不用轮询
- **版本差异宏公开** — `UELinkCompat.h` 挪到 UELinkCore 的 Public，扩展插件直接用（宏改名为 `UELINK_JSON_KEY` 等）

### 注意
- **仓库搬到 fixcode-h/UEKit** — 源码在 `plugins/UELink/`，发布标签改成 `UELink-v<版本>`；客户端改读 UEKit 的 `catalog.json` 查更新
- 装好后要在本地完整编译一次（Live Coding 不够）

## v2.1.0

> 源码和 2.1.0-beta.1 相同，按用户要求转正式版。

### 改进
- **推送通道一律要令牌** — 每个请求都要带名片里的令牌，没带或不对回 `bad token`；本机别的进程读不到项目的 Saved 目录就连不上
- **没有 HTTP 服务时也写名片** — 带 `-NoAgentPanel` 启动、或 HTTP 端口没拿到时，名片照样写（只是没有 `port` / `http`），UEToVS 照样能找到推送通道
- **令牌整个进程只生成一次** — HTTP 服务和推送通道谁先起谁生成，之后不再换，客户端读到的令牌一直有效

### 注意
- **去掉兼容层** — 不再写 `Saved/AgentPanel/instances/` 和 `Saved/UEToVSLink/port.txt`。需要 Agent 面板 UE 插件 0.11.0、UEToVS 0.6.0 及以上（它们读 `Saved/UELink/instances/` 并带令牌）；更旧的客户端连不上，先升级客户端
- 装好后要在本地完整编译一次（Live Coding 不够）

## v2.1.0-beta.1

> 还没在本地用 UE 编译过，先发预发布（客户端查更新只认正式版，不会推给用户）。本地完整编译通过后去掉 `-beta` 发 2.1.0。

### 改进
- **推送通道一律要令牌** — 每个请求都要带名片里的令牌，没带或不对回 `bad token`；本机别的进程读不到项目的 Saved 目录就连不上
- **没有 HTTP 服务时也写名片** — 带 `-NoAgentPanel` 启动、或 HTTP 端口没拿到时，名片照样写（只是没有 `port` / `http`），UEToVS 照样能找到推送通道
- **令牌整个进程只生成一次** — HTTP 服务和推送通道谁先起谁生成，之后不再换，客户端读到的令牌一直有效

### 注意
- **去掉兼容层** — 不再写 `Saved/AgentPanel/instances/` 和 `Saved/UEToVSLink/port.txt`。需要 Agent 面板 UE 插件 0.11.0、UEToVS 0.6.0 及以上（它们读 `Saved/UELink/instances/` 并带令牌）；更旧的客户端连不上，先升级客户端
- 装好后要在本地完整编译一次（Live Coding 不够）

## v2.0.0

> 和 2.0.0-beta.1 的源码相同，已在本地用 UE 5.4 完整编译通过，转正式版。

### 新功能
- **一个插件同时给 Agent 面板和 UEToVS 用** — 合并了 Agent 面板的 UE 桥接（AgentPanelBridge 1.9.0）和 UEToVS 的编辑器插件（UEToVSLink 0.2.0）：控制台、GM、对象属性、DataTable、资源和蓝图读写、Trace、Lua，加上打开资源、Live Coding、自动化测试、日志推送，项目里只装这一个、只编译一次
- **Live Coding 结果两边都能看到** — 不管是在 VS 里、面板里还是编辑器里触发的编译，都会跟踪到结束：结果、耗时和编译器输出推给订阅了 `liveCoding` 的客户端，HTTP 用 `livecoding.status` 也能查
- **Agent 能跑自动化测试** — 新增 HTTP 操作 `tests.run`、`tests.status`，结果和推送通道的 `tests` 事件是同一份
- **一张名片** — `Saved/UELink/instances/<PID>.json` 里同时有 HTTP 端口、推送通道端口（`stream`）、令牌、协议号和能力列表（`features`）

### 改进
- **日志只挂一次** — 原来两个插件各在 GLog 上挂一个输出设备、每行各拷一份；现在推送通道和 `log.tail` 共用一个环形缓冲
- **推送通道认令牌** — 请求里带 `token` 时必须和名片一致；不带令牌的旧 UEToVS 暂时照常能连（兼容层）

### 注意
- **模块改名** — `AgentPanelBridge` → `UELinkCore`，`AgentPanelBridgeEditor` → `UELinkEditor`。项目代码的 Build.cs 里依赖了 `AgentPanelBridge` 的（继承 `UAgentPanelGMLibrary`、调 `FAgentPanelLua::SetStateProvider`），把依赖改成 `UELinkCore`；类名和头文件名没变。资源里引用旧脚本包的由插件自带的 CoreRedirects 重定向
- **旧插件要删掉** — 项目 `Plugins/` 下还有 `AgentPanelBridge` 或 `UEToVSLink` 时编译直接报错并说明要删哪个目录（两个同时在会重复挂日志、抢 Live Coding，类名也会冲突）
- **兼容层** — 仍写旧位置的名片 `Saved/AgentPanel/instances/` 和 `Saved/UEToVSLink/port.txt`，现有版本的 Agent 面板 UE 插件和 UEToVS 不用升级就能连上；之后的版本会去掉
- **只发源码** — Lua 支持要在编译时按项目里的 UnLua 选接口，没法发预编译的 DLL，装好或更新后要在本地完整编译一次（Live Coding 不够）
