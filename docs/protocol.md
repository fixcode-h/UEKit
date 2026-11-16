# UELink 协议与安装约定

给客户端（Agent 面板的 UE 插件）看：怎么找到 UE 进程、怎么调用、怎么订阅、怎么查更新和安装。**改协议先改这份文档。**

> UEToVS 0.7.0 起不再连 UE（日志、Live Coding、测试结果不经过 VS）。下面推送通道里的一次性命令和 `tests` / `liveCoding` 主题是给旧版 UEToVS 留的，面板用 HTTP 的 `tests.*`、`livecoding.*` 和扩展事件。

## 1. 名片：找到 UE 进程

每个加载了 UELink 的进程（编辑器、PIE 所在的编辑器、`-game`、专用服务器、后台编辑器）启动后写一张名片：

```
<项目>/Saved/UELink/instances/<PID>.json
```

```json
{
  "pid": 12345,
  "kind": "editor",            // editor / game / server / background
  "project": "D:/Game/Game.uproject",
  "engine": "5.4.4",
  "plugin": "UELink",
  "version": "2.0.0",          // UELink 版本（等于 bridge）
  "bridge": "2.0.0",           // 旧字段，Agent 面板按它比版本
  "port": 30110,               // HTTP 端口（旧字段）；HTTP 服务没起来时（-NoAgentPanel 等）没有这个字段
  "http": 30110,               // HTTP 端口；同上
  "stream": 51234,             // 推送通道端口；只有编辑器进程有
  "protocol": 2,               // 推送通道协议号
  "features": ["log", "tests", "liveCoding"],
  "token": "…",                // 本次启动随机生成，HTTP 和推送通道都用它
  "startedAt": "2026-11-14T02:00:00.000Z"
}
```

- 进程退出时删名片；客户端读到名片后还要确认 `pid` 还活着（崩溃时名片会留下）。
- 名片只写在 `Saved/UELink/instances/`。2.0 的兼容层（同一张名片再写一份到 `Saved/AgentPanel/instances/`、推送通道端口写到 `Saved/UEToVSLink/port.txt`）在 2.1 去掉了：客户端要 Agent 面板 UE 插件 0.11.0、UEToVS 0.6.0 及以上。
- 同一项目开着几个编辑器时，推送通道的客户端连 `startedAt` 最新的那个。

## 2. HTTP：请求和应答

- `POST http://127.0.0.1:<http>/rpc`，请求头 `X-AgentPanel-Token: <token>`，正文 `{"op": "...", "args": {...}}`。
- 回 `{"ok": true, ...}` 或 `{"ok": false, "error": "..."}`。`error` 是给人看的中文，原样显示给用户和 Agent。
- `GET /info`（同样要令牌）回进程和世界列表。
- 请求在游戏线程上执行。端口：命令行 `-AgentPanelPort=` 指定的优先，否则从 30110 往后找空闲的。带 `-NoAgentPanel` 启动时不起 HTTP 服务。

操作（`op`）：

| 分组 | 操作 |
| --- | --- |
| 控制台和变量 | `console`、`cvars`、`cvar.set`、`gm.list`、`gm.run` |
| 对象和属性 | `actors`、`outliner`、`objects`、`objects.children`、`classes`、`components`、`props`、`get`、`set`、`call`、`functions`、`watch`、`object.refs`、`umg.tree`、`screenshot` |
| DataTable | `datatable`、`datatable.rows`、`datatable.row`、`datatable.set` |
| 资源 | `assets`、`assets.children`、`asset.classes`、`asset.describe`、`asset.refs`、`asset.create`、`asset.duplicate`、`asset.rename`、`asset.delete`、`asset.save`、`asset.revert`、`asset.dirty`、`asset.undo`、`asset.validate`、`asset.export`、`asset.textify`、`asset.open` |
| 蓝图 | `bp.outline`、`bp.graph`、`bp.edit`、`bp.compile`、`bp.find`、`bp.node_search` |
| 日志和性能 | `log.tail`、`log.clear`、`perf.live`、`trace.query` |
| 编辑器 | `pie`、`livecoding`、`livecoding.status`、`tests.run`、`tests.status`、`editor.selection`、`editor.select` |
| Lua（装了 UnLua 时） | `lua.status`、`lua.objects`、`lua.get`、`lua.set`、`lua.exec`、`lua.check`、`lua.reload`、`lua.modules`、`lua.errors`、`lua.inspect`、`lua.call` |

`components {"path"}` → `{"items": [{"path", "name", "class", "parent", "root", "scene", "created": "native|blueprint|script|instance", "inherited"?}], "scs"?}`。
2.2.0 起 `path` 是蓝图资源（或它的类默认对象）时，除了 C++ 组件还带上蓝图组件列表（自己和父蓝图的 SCS）里的组件，`path` 是组件模板，用 `props` / `set` 读改它就是改蓝图里这个组件的默认值；`inherited` 标出来自父蓝图的，回包带 `"scs": true`。旧版本没有 `scs`，客户端自己用 `bp.outline` 的 `components` 补。

2.2.0 起的世界大纲和 UObject 树（只给名字和层级，属性点了再用 `props` 读）：

- `outliner {"world"?, "limit"?}` → `{"world", "total", "items": [{"path", "name", "class", "label"?, "folder"?, "parent"?, "player"?}]}`：世界里全部 Actor，`parent` 是挂接的父 Actor，`folder` 是大纲文件夹（编辑器里才有）。
- `objects.children {"path"?, "world"?, "limit"?}` → `{"items": [{"path", "name", "class", "more"}], "total"?}`：`path` 为空时给根（GameInstance、世界、引擎），否则给它下一层（外层是它）的对象；不含 Actor、组件、关卡、反射对象和类默认对象。`more` 表示还能往下展开。
- 推送事件 `uelink.outliner` `{"worlds": [世界 id], "all": bool}`：编辑器里 Actor 增删、挂接、换文件夹、关卡列表变了时推，最多每 0.5 秒一次，不带数据，客户端收到后重读 `outliner`。

2.3.0 起的资源文件夹树（照 Content Browser，点开一个文件夹才读下一层；只读资源注册表、不加载资源，编辑器、后台编辑器、`-game` 里都能用）：

- `assets.children {"path", "class"?, "limit"?}` → `{"path", "folders": [{"name", "path", "count"?}], "items": [{"name", "path", "class"}], "more"?, "loading"?}`。
  - `path` 为 `/`（或空）时给挂载根：`/Game`（`name` 为 `Content`）、内容目录在项目里的挂载点（项目插件）、`/Engine`，`items` 为空。
  - 其他路径给这一层：`folders` 是子文件夹（不含世界分区的 `__External*` 目录），`items` 是这一层的资源（不含重定向器），`path` 是对象路径（`/Game/A/B.B`，和 `assets` 一致），`class` 是类名短名。都按名字排序（不分大小写），文件夹在前。`count` 目前不给（每个子文件夹单独数要扫全树），客户端按可选字段处理。
  - `class` 按类筛这一层的资源（含子类，类名写法和 `assets` 一样）；文件夹不受筛选影响。`limit` 默认 500、最多 2000，文件夹加资源超过时截断并回 `"more": true`。
  - `loading: true` 表示资源注册表还在扫盘（编辑器刚启动），结果可能不全，稍后重读。
  - 什么时候用：网页资源页的文件夹树逐层展开；要按名字搜全项目还是用 `assets`。
  - 旧版本（2.2 及以前）没有这个操作：`GET /info` 的 `ops` 里没有 `assets.children`（直接调会回「这个进程不支持 assets.children」），客户端改走磁盘兜底（自己列项目 `Content/` 和插件 `Content/` 目录下的 `.uasset` / `.umap`）。

2.4.0 起的编辑器选中同步（只在编辑器进程里有；客户端按 `GET /info` 的 `ops` 里有没有 `editor.selection` 判断）：

- `editor.selection {}` → `{"actors": [{"path", "name", "label", "class", "world"}], "actorCount", "assets": [{"path", "name", "class"}], "folders": [内容浏览器选中的文件夹], "blueprint"?: {"path", "graph"?, "nodes": [{"id", "title"}]}}`。
  - `actors` 是关卡编辑器里选中的 Actor（最多 50 个，`actorCount` 是实际个数）；`assets`、`folders` 是主内容浏览器里选中的（各最多 50 个）；`blueprint` 是最近激活的蓝图编辑器里打开的蓝图和它当前图表里选中的节点（节点 id 和 `bp.graph` 一致），没开蓝图编辑器时没有这个字段。
  - 只读编辑器的选中集合（`USelection`、`IContentBrowserSingleton::GetSelectedAssets`、蓝图编辑器的 `GetSelectedNodes`），不加载资源。
- `editor.select {"actors"?: [Actor 路径], "assets"?: [资源路径], "focus"?: true}` → `{"actors": 选中了几个, "assets": 几个, "missing": [没找到的路径]}`：在编辑器里选中这些 Actor（`focus` 为 true 时视口对焦过去）、在内容浏览器里定位这些资源。打开资源还是用 `asset.open`。
- 推送事件 `uelink.selection`（`features` 里有 `selection`）：关卡里选中的 Actor、内容浏览器选中的资源或文件夹变了时推，最多每 0.3 秒一次，事件字段和 `editor.selection` 的回包一样（不含 `blueprint`：蓝图编辑器的节点选中没有公开的变化通知，要时再调 `editor.selection`）。客户端订阅 `topics` 里写 `uelink.selection`。

2.4.0 起的 Lua 改完能验、对象检视（都在「装了 UnLua 时」才有，`/info` 的 `ops` 里能看到）：

- `lua.check {"code"? | "file"? | "files"?: [...], "name"?}` → `{"passed", "checked", "errors": [{"file", "line", "message"}]}`：只编译不执行（`luaL_loadbuffer`），在一个**单独新建**的 `lua_State` 里做，不碰游戏里的 Lua 环境，所以不开游戏也能查。`file` / `files` 是磁盘路径，相对路径按项目目录算；一次最多 500 个文件。回包里不用 `ok` 字段（和外层的 `ok` 冲突），用 `passed`。
- `lua.reload {"modules": ["UI.BagPanel", ...]}` 或 `{"changed": true}` → `{"items": [{"module", "ok", "mode": "reloader|require", "error"?}], "note"?}`：
  - 项目调了 `FAgentPanelLua::SetReloader` 时，每个模块交给它重载（`mode: "reloader"`）。UnLua 二次开发版的热重载入口各家不同，桥接不猜。
  - 没接时用 `require` 兜底（`mode: "require"`）：清掉 `package.loaded[模块]` 再 `require`，然后把新模块表里的**函数**拷回旧表、旧表放回 `package.loaded`。UnLua 的实例表通过元表指向模块表，这样已经绑定的对象也会用上新函数；旧表上的数据字段不动，新加的字段补上。模块顶部 `local` 存的状态会重新初始化（新函数用新的 upvalue），回包 `note` 里说明。
  - `changed: true` 重载 `lua.modules` 里 `stale` 的那些。
- `lua.modules {"filter"?, "limit"?}` → `{"items": [{"name", "file"?, "objects", "loadedAt"?, "modifiedAt"?, "stale"}], "total", "root"}`：`package.loaded` 里键是字符串、值是表的模块。`file` 先取模块里第一个 Lua 函数的定义文件，取不到时按模块名在脚本根目录（`root`，默认 `Content/Script`）下找 `A/B/C.lua`。`objects` 是绑定这个模块的对象数。`loadedAt` 是桥接重载它的时间，没重载过时是桥接第一次看到它的时间（偏晚，回包里 `loadedAtGuess: true`）；`stale` = 文件修改时间晚于 `loadedAt`。时间都是 ISO 8601。
- `lua.errors {"since"?, "limit"?, "clear"?}` → `{"items": [{"id", "message", "where"?, "count", "first", "last", "stack": [行], "locals"?: [{"name", "t", "v"?}], "source": "capture|log", "seq"}], "seq"}`：最近的 Lua 报错，按「消息 + 出错位置」归并（同一处反复报只占一条，`count` 加一）。`since` 是上次回包的 `seq`，只给这之后变过的。`clear: true` 清空。
  - 来源一 `capture`：项目在自己的 Lua 报错处理里调 `FAgentPanelLua::CaptureError(L, 消息)`（错误处理函数里栈还在，能拿到出错那一帧的局部变量）。
  - 来源二 `log`：没接时从日志里认（分类名带 `Lua` 的 Error 行，或文字里有 `LuaError` / `stack traceback`），没有局部变量。
- `lua.inspect {"path"?, "keys"?, "offset"?, "limit"?, "world"?}` → `{"value", "chain": [{"name", "src"?}], "src"?, "fields": [...], "functions": [{"name", "params": [...], "vararg", "src"?, "from"}], "upvalues": [{"fn", "name", "t", "v"?}], "total", "more"}`：
  - 起点和 `lua.get` 一样（`path` 是对象路径，空为 `_G`；`keys` 往下走）。
  - `fields` 是这张表自己的项（不含函数，格式同 `lua.get`），分页同 `lua.get`。
  - `functions` 是自己的函数（`from: 0`）和元表链上继承来的（`from` 是第几层），参数名用 `debug.getlocal` 取，C 函数没有参数名。
  - `chain` 是沿元表 `__index` 往上的每一层，`name` 是它在 `package.loaded` 里的模块名（不是模块时为空）。
  - `upvalues` 是自己的函数用到的 upvalue（模块顶部的 `local`），同名只列一次，最多 100 个，只给简单值。
- `lua.call {"path"?, "keys": [函数路径], "self"?: false, "args"?: [JSON], "timeoutMs"?: 10000}` → `{"results", "output", "ms", "changes"?: [{"k", "before", "after"}]}`：沿 `keys` 找到函数，`self: true` 时把上一级的表当第一个参数（`obj:fn(...)`）。参数是 JSON，对象和数组转成 Lua 表。在 `lua_pcall` 里执行，超时中止，`print` 收进 `output`。`self: true` 时回包带上 self 表顶层简单字段调用前后的变化。
- `lua.set` 2.4.0 起 `value` 也可以是 JSON 对象或数组（转成 Lua 表）；原值是表时也能整个换掉，回包 `"undoable": false`、没有 `prev`（客户端不要提供撤回）。

2.4.0 起 `asset.validate` 能一次查多个：`{"paths": [...]}`（最多 200 个）→ `{"items": [{"path", "result", "errors", "warnings", "issues": [前 5 条]}], "errors", "warnings", "report"}`，完整结果写进 `Saved/UELink/validate/<时间>.json`，`report` 是它的绝对路径。只给 `path` 时照旧回单个资源的结果。

> 2.4.0 这一批的操作由几个并行的工作包分别加：每个包的新操作放各自的 `*Ops.cpp`（例如 `UELinkSelectionOps.cpp`、`AgentPanelLuaDevOps.cpp`），在这份文档里各写一段，操作表里各加一格，版本统一记在 UELink 的 CHANGELOG `v2.4.0-beta.1` 一节里。

2.0.0 新加的三个：

- `livecoding` → `{"started": true}`；已经在编译时 `{"started": false, "message": "already compiling"}`。结果用下面的 `livecoding.status` 查。
- `livecoding.status` → `{"available", "phase": "idle|compiling|finished", "result": "success|failed|noChanges|cancelled|unknown", "message", "durationMs", "output": [最近的编译输出]}`。不管是谁触发的编译都会记录。
- `tests.run {"filter": "Project.Foo", "id"?}` → `{"id"}`；`tests.status` → `{"running", "id", "filter", "passed", "failed", "current"?, "durationMs", "message"?, "tests": [{"name", "result", "durationMs", "errors"}]}`。同一时间只能跑一组。

## 3. 推送通道：TCP，一行一个 JSON

只在编辑器进程里有。连 `127.0.0.1:<stream>`，发一行 JSON，收一行或多行 JSON（UTF-8，`\n` 结尾）。

**令牌**：每个请求（包括 `subscribe` 和订阅连接上后续的 `subscribe` 行）都要带 `"token"`，和名片一致才处理；没带或不一致回 `{"ok":false,"message":"bad token"}`（订阅连接上的行直接忽略）。2.0 还放行不带令牌的旧 UEToVS，2.1 起一律要求。

一次性命令（回一行就关连接）：

| 命令 | 回复 |
| --- | --- |
| `{"cmd":"ping","protocol":2}` | `{"ok":true,"project","engine","protocol":2,"plugin":"2.0.0","name":"UELink","features":[...]}` |
| `{"cmd":"openAsset","path":"/Game/..."}`、`browseAsset` | `{"ok":true}`，没有编辑器的资源会附 `"message":"shown in Content Browser"` |
| `{"cmd":"liveCoding"}` | `{"ok":true}`，已在编译时附 `"message":"already compiling"` |
| `{"cmd":"runTests","id":"r1","filter":"Project.Foo"}` | `{"ok":true,"id":"r1"}`，结果按 `tests` 事件推 |
| 不认识的命令 | `{"ok":false,"message":"unknown command: ..."}`（旧客户端按这个开头判断插件太旧，不改） |

订阅：`{"cmd":"subscribe","protocol":2,"topics":["log","tests","liveCoding"]}` → 先回 `{"ok":true,"protocol":2,"plugin":...}`，然后连接一直开着，推事件行。同一个连接上再发 `subscribe` 可以换主题（回 `{"event":"subscribed","topics":[...]}`）。最多 8 个订阅者。

| 事件 | 字段 |
| --- | --- |
| `log` | `dropped`（被丢的行数）、`lines: [{"t": 相对秒数, "c": 分类, "v": ELogVerbosity 数值, "m": 文字}]` |
| `tests` | `phase`：`runStarted`（`id`、`filter`）、`testStarted`（`name`）、`testFinished`（`name`、`result`、`durationMs`、`errors`、`warnings`、`logs`）、`runFinished`（`passed`、`failed`、`durationMs`、`message`?） |
| `liveCoding` | `phase`：`started`、`output`（`lines`，`source:"console"` 表示来自 LiveCodingConsole 日志）、`finished`（`result`、`durationMs`、`message`?） |

客户端不读时：发送缓冲超过 4 MB 丢日志批次（计入下一批的 `dropped`），超过 16 MB 断开。

**协议号**：推送通道沿用 UEToVSLink 的协议 2。只有不兼容的格式变化才加协议号；加事件、加命令只往 `features` 里加名字。两边都忽略不认识的字段和事件。

## 4. 发布清单：查更新

每个版本的 Release（2.2 起在 fixcode-h/UEKit，标签 `UELink-v<版本>`；2.1 及以前在 fixcode-h/UELink，标签 `v<版本>`）带三个附件：

- `UELink-<版本>-src.zip`：只有源码，解压出 `UELink/` 目录。
- `manifest.json`：
  ```json
  {
    "name": "UELink",
    "version": "2.0.0",
    "versionCode": 1,
    "protocol": 2,
    "engineMin": "5.4",
    "asset": "UELink-2.0.0-src.zip",
    "url": "https://github.com/fixcode-h/UEKit/releases/download/UELink-v2.2.0/UELink-2.2.0-src.zip",
    "sha256": "…",
    "size": 137237,
    "replaces": ["AgentPanelBridge", "UEToVSLink"],
    "notes": "这一版的更新说明（Markdown）"
  }
  ```
- `SHA256SUMS`。

插件目录：`https://raw.githubusercontent.com/fixcode-h/UEKit/main/catalog.json`。每个插件一条，`latest` 是最新正式版的清单（就是上面这份 manifest），`prerelease` 是比它新的预发布版（没有为 null），客户端默认只装 `latest`。**更新源地址要能在客户端设置里改**（公司内网镜像、本地目录），镜像照原样放 `catalog.json` 和各插件的三个文件即可。

2.1 及以前的客户端读 `https://github.com/fixcode-h/UELink/releases/latest/download/manifest.json`。UELink 仓库删掉后它们读不到新版本，会用自带的那份，升级客户端即可。

## 5. 安装约定（两个客户端都照这个做）

1. **三份里取最高版本，只升不降**：项目里已装的（`Plugins/UELink/UELink.uplugin` 的 `VersionName`）、客户端自带的、发布清单里最新的。项目里的比另外两个都新（另一个客户端装的），就不动。
2. **下载要校验** `sha256`，不对就不装。
3. **编辑器开着时不装**（DLL 被占用）：提示用户先关编辑器。
4. **只换 `Source/`、`Config/` 和 `UELink.uplugin`**，留着 `Binaries/`、`Intermediate/`，让 UBT 按时间戳重编。
5. **清理旧插件**：`Plugins/` 下（含一层子目录）有 `replaces` 里的插件就在确认时说清楚并删掉目录，`.uproject` 的 `Plugins` 列表里有同名条目也删掉。不删的话 UELink 编译时会报错拦下。
6. **版本管理**：用户选「只在本机」时把 `Plugins/UELink/` 写进本机忽略（git 的 `info/exclude`、P4 的 `.p4ignore`）；选「加入版本管理」时 `git add` / `p4 add` 到默认变更列表，**不替用户提交**。
7. **安装记录** `Plugins/UELink/.install.json`：
   ```json
   { "version": "2.0.0", "by": "agent-panel|uetovs", "source": "github|bundled|mirror", "mode": "local|vcs|files", "at": "2026-11-14T02:00:00Z" }
   ```
   `files` = 只复制文件、版本管理留给用户（旧版 UEToVS 自己装时写这个）。另一个客户端升级时沿用同样的 `mode`。这个文件不进发布的源码包。
8. 装完提示**需要完整编译一次**（Live Coding 不够）。

## 6. 扩展插件

UEKit 里的其他插件（AssetForge、GameTest）是 UELink 的扩展：不自己开端口，操作挂在 UELink 的 `/rpc` 上，共用令牌和名片。

- 扩展模块启动时 `FAgentPanelBridge::Get().RegisterExtension(名字, 版本)`，名片和 `GET /info` 里多一个字段：
  ```json
  "extensions": [{ "name": "AssetForge", "version": "1.0.0" }]
  ```
  这是**这个进程里实际加载了**的扩展和版本；装了新版还没重编时，和项目 `Plugins/` 里磁盘上的版本不同。
- 操作用 `AddExtensionOp("前缀.名字", 处理函数)` 登记：名字必须带前缀（AssetForge 用 `forge.`，GameTest 用 `gt.`），和已有操作重名时拒绝。客户端调用方式和 UELink 自己的操作完全一样。
- 长任务（批量导入、跑测试场景）马上返回作业号，客户端用 `forge.job` / `gt.job` 查进度和最终结果。
- **推送事件**（2.2 起，`features` 里有 `extEvents`）：扩展用 `FAgentPanelBridge::Get().PublishEvent("前缀.主题", 事件)` 推实时状态（在游戏线程调用）。客户端在推送通道订阅时把主题写进 `topics`（如 `["log","forge.job","gt.record"]`），收到的事件行是 `{"event":"forge.job", ...事件字段}`。没人订阅时扩展推事件几乎没有开销。事件只是实时提示，客户端中途连上会漏掉之前的，最终结果以 `*.job` 为准。
- 扩展各自的操作列表见 `docs/asset-forge.md`、`docs/game-test.md`。
