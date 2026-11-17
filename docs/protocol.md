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
  "startedAt": "2026-11-14T02:00:00.000Z",
  "ready": true,               // 2.7 起只有编辑器进程有：编辑器可用了没有，见下
  "readyAt": "2026-11-14T02:01:30.000Z"
}
```

- **编辑器就绪**（2.7 起，`features` 里有 `editorReady`）：名片出现只说明引擎初始化完、HTTP 服务起来了，这时编辑器主窗口可能还没出来、资源注册表还在扫盘。编辑器进程的名片一开始就带 `"ready": false`，主窗口建好、资源注册表扫完、没有正在异步加载的包（连续两次检查，每 0.5 秒一次）后改成 `true` 并写 `readyAt`，之后不再回退。同时推送事件 `uelink.ready {"ready": true, "readyAt", "seconds"}`（`seconds` 是从进程启动算起的秒数），日志打一行 `UELink：编辑器已就绪（启动 N 秒）`。`GET /info` 也带 `ready` / `readyAt`。
  - 着色器、资源编译不算在就绪里（编辑器边编边能用），`editor.ready` 的回包里报剩多少，客户端自己决定等不等。
  - 客户端：名片**没有** `ready` 字段的是 2.7 以前的 UELink、后台编辑器或 `-game` 进程，名片出现就当可用；有 `ready: false` 时等它变 `true`。
  - 编辑器在同步加载、编着色器时游戏线程卡住，HTTP 和推送通道都暂时不回应（连接可能超时，也可能被直接拒绝），只要 `pid` 还活着就当「忙」，不要判成启动失败。
  - `editor.ready` → `{"ready", "readyAt"?, "readySeconds"?, "waiting"?: "mainFrame|assetRegistry|asyncLoading", "seconds", "mainFrame", "assetsLoading", "asyncLoading", "shadersLeft", "assetsCompiling"}`；只在编辑器进程里有。

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
| 控制台和变量 | `console`、`cvars`、`cvar.set`、`gm.list`、`gm.run`、`config.files`、`config.get`、`tags.list`、`tags.refs` |
| 对象和属性 | `actors`、`outliner`、`objects`、`objects.children`、`classes`、`components`、`props`、`get`、`set`、`call`、`functions`、`watch`、`object.refs`、`umg.tree`、`screenshot` |
| DataTable | `datatable`、`datatable.rows`、`datatable.row`、`datatable.set` |
| 资源 | `assets`、`assets.children`、`asset.classes`、`asset.describe`、`asset.refs`、`asset.create`、`asset.duplicate`、`asset.rename`、`asset.delete`、`asset.save`、`asset.revert`、`asset.dirty`、`asset.undo`、`asset.validate`、`asset.export`、`asset.textify`、`asset.open`、`asset.redirectors` |
| 蓝图 | `bp.outline`、`bp.graph`、`bp.edit`、`bp.compile`、`bp.find`、`bp.node_search` |
| 日志和性能 | `log.tail`、`log.clear`、`perf.live`、`trace.query` |
| 编辑器 | `pie`、`livecoding`、`livecoding.status`、`tests.run`、`tests.status`、`editor.selection`、`editor.select`、`editor.spawn`、`editor.destroy` |
| Lua 声明（编辑器，2.5） | `luadecl.status`、`luadecl.enable`、`luadecl.run` |
| Lua（装了 UnLua 时） | `lua.status`、`lua.objects`、`lua.get`、`lua.set`、`lua.exec`、`lua.check`、`lua.reload`、`lua.modules`、`lua.errors`、`lua.inspect`、`lua.call`、`lua.snap`、`lua.profile`、`lua.leaks` |

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

2.5.0 起的 Lua 声明（只在编辑器进程里有，代码在 `UELinkLuaDecl.cpp` / `UELinkLuaDeclGen.cpp`）：后台把 UE 反射（原生类、结构体、枚举、蓝图、用户定义结构体 / 枚举）生成 EmmyLua 注解的 Lua 声明文件，给 IDE（IntelliJ-EmmyLua 的 emmylua_ls）做跳转、补全、悬停。**默认关闭**，不需要 UnLua 也能开。

- 输出目录 `<项目>/Intermediate/UELink/LuaDecl/`：`Native/<模块>/<类型>.lua`（原生类型）、`Blueprint/<包路径>.lua`（如 `Blueprint/Game/UI/BP_Bag.lua`）、`UE.lua`（全局 `UE` 表）、`UnLua/`（装了 UnLua 时拷它 `Content/IntelliSense` 里的静态声明：`UnLua` 表、`TArray` / `TMap` 等泛型），清单 `.uelink-luadecl.json`（`files`：相对路径 → 内容哈希；`assets`：包名 → `{file, stamp, full}`）。IDE 认这个目录：有清单就用它，不再加 UnLua 自己生成的 IntelliSense 目录（两份会重复定义）。
- 格式和 UnLua 的 Generate IntelliSense 一致：类型名 `AActor`、`BP_Hero_C`、`FVector`、`TArray<integer>`，`---@class 名 : 父类`、`---@field`、`function 类:函数(参数) end`（静态函数用 `.`）。比 UnLua 多的：输出参数和带默认值的参数标成可省（`---@param X? T`），输出参数按 UnLua 的调用约定跟在返回值后面多写 `---@return`；字节属性带枚举时类型是枚举名；名字不是合法标识符的成员写成 `["名字"]`。
- 增量：内容哈希和清单一样就不写盘，所以 emmylua_ls 的文件监听只收到真正变了的文件。打开后（或每次启动编辑器）先整遍比对一次：原生类型、资源注册表里的全部蓝图类资源；之后只处理变化——蓝图编译完、资源加载 / 保存 / 新增 / 改名 / 删除（删除和改名的旧名删掉对应文件）、Live Coding / 热重载和新模块加载（原生类型再比对一遍）。全部在游戏线程上分帧做：编辑器在前台每帧约 4 毫秒，在后台约 20 毫秒，PIE 里 1 毫秒；写盘在后台线程按顺序做（先写临时文件再换名）。
- **默认只生成 C++ 反射类型**（2.6.0 起）：蓝图、用户定义结构体 / 枚举要另开 `blueprints` 开关（`[UELink.LuaDecl] Blueprints`，`luadecl.enable {"blueprints": true}` 或控制台 `UELink.LuaDecl.Blueprints 1`），默认关；关着时以前生成的蓝图声明在下一遍整遍比对时删掉。
- 没加载过的蓝图不为了声明去加载：先写只有继承关系的占位（`ParentClass` 标签，`full` 为 false），打开、编译或运行时加载过一次就补全；以前补全过的留着。要一次补全就 `luadecl.run {"loadBlueprints": true}`：在后台每帧加载一个（PIE 里暂停）。
- `luadecl.status {}` → `{"enabled", "blueprints", "active", "dir", "phase": "idle|native|assets|loading", "done"?, "total"?, "loadDone"?, "loadTotal"?, "files", "stubs", "written", "deleted", "pending", "lastPassAt"?, "lastPassMs"?}`：`enabled` 是开关，`active` 是这个进程里在不在跑；`written` / `deleted` 是这次打开以来写、删的文件数。
- `luadecl.enable {"enabled": true|false, "blueprints"?: true|false}` → 同 `luadecl.status`：开关存在本机的编辑器个人配置（`EditorPerProjectUserSettings.ini` 的 `[UELink.LuaDecl] Enabled`），下次启动编辑器照这个来；项目想统一打开可以在 `DefaultEditor.ini` 写同一节，个人配置优先。关掉时已生成的文件留着（IDE 照样能用，删目录就回到 UnLua 的声明）。
- `luadecl.run {"force"?: false, "loadBlueprints"?: false}` → `{"started": true, "toLoad"?, ...状态}`：马上再整遍比对一次；`force` 不看清单全部重写；没打开时报错让先 `luadecl.enable`；`loadBlueprints` 要先打开蓝图开关。
- 推送事件 `uelink.luadecl` `{"phase": "done", "written", "deleted", "ms"}`（`features` 里有 `luaDecl`）：整遍比对做完时推。
- 编辑器控制台：`UELink.LuaDecl.Enable 1|0`、`UELink.LuaDecl.Run [force] [load]`。

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

2.4.0 起的 Lua 深度调试（「装了 UnLua 时」才有，代码在 `AgentPanelLuaDebugOps.cpp`）。三者都靠 Lua 的调试钩子或只读遍历，不改游戏状态；不做断点（桥接在游戏线程上处理请求，游戏线程一停桥接也停了）：

- 钩子：快照点用行钩子、采样用调用 / 返回钩子，共用一个钩子函数。挂上前记下原来的钩子（IDE 的 Lua 调试器），事件按它原来的掩码转过去，都撤了再放回去。**挂上之后 IDE 的 Lua 调试器又换了钩子时不抢回来**（2.5.3 起；以前每帧抢回，会让 IDE 断点失灵）：这期间快照点和采样收不到事件，`lua.snap list`、`lua.profile start` 带 `warning`；调试器断开把钩子清掉后自动接上。`lua.exec` / `lua.call` / `lua.reload` 执行期间临时换成超时钩子，那段代码里的快照点和采样看不到；执行完只在钩子还是超时钩子时才放回原来的（期间调试器换过就不动）。钩子按协程各一份，挂之前就在跑的协程看不到（回包 `note` 说明）。
- `lua.snap {"action": "add", "file", "line", "hits"?: 1, "ttlSec"?: 600}` → `{"snap": {"id", "file", "line", "hits", "max", "active", "created", "expiresIn"}, "note"}`：执行到这一行时记下函数、局部变量（表顺带前 20 项简单值）、upvalue 和调用栈，游戏不暂停。`file` 可以是磁盘路径、相对脚本根的路径或模块名（`UI.BagPanel`），和 chunkname 按路径段从后往前比。命中 `hits` 次（最多 20）或过了 `ttlSec`（10–3600 秒）自动撤掉；同时最多 20 个。
  - `{"action": "list", "id"?}` → `{"items": [...], "warning"?}`：不给 `id` 只列概要；给 `id` 时带 `captures: [{"at", "fn", "src", "file"?, "line", "locals": [{"name", "t", "v"?, "fields"?}], "upvalues"?, "stack": [行]}]`。钩子被别的调试器换掉时带 `warning`。
  - `{"action": "remove", "id"?}` → `{"removed"}`：不给 `id` 全部撤掉并清记录。
  - 推送事件 `lua.snap`（命中一次推一次）：`{"id", "hits", "max", "active", "capture"}`。客户端订阅 `topics` 里写 `lua.snap`。
- `lua.profile {"action": "start", "seconds"?: 5}` → `{"started", "seconds", "note", "warning"?}`：采几秒（1–60），按函数（闭包）累计调用次数、总用时、自身用时，C 函数（UnLua 导出的 UE 接口）也算。到时间自己停；`{"action": "stop"}` 提前停。
  - `{"action": "result", "sort"?: "self|total|calls", "limit"?: 50}` → 还在采时 `{"running": true, "remaining"}`；采完 `{"running": false, "startedAt", "seconds", "events", "total", "items": [{"fn", "src", "file"?, "line", "c", "calls", "totalMs", "selfMs"}], "report"}`，`file` 是完整路径（chunkname 以 @ 开头时才有，`src` 太长会被截断），`c` 表示 C 函数；全部函数写进 `Saved/UELink/lua-profile/profile-<时间>.json`。递归函数的总用时会重复计入；协程挂起的时间算在协程里的函数上。
- `lua.leaks {"limit"?: 100, "maxVisit"?: 200000, "objectKey"?: "Object"}` → `{"items": [{"kind", "module"?, "path"?, "object"?, "class"?}], "total", "visited", "truncated", "ms", "report", "note"}`：找 Lua 还拿着、UObject 却已经没了的（专查「关了界面控件释放不掉」）。`kind`：
  - `destroyed`：从 `_G` 和注册表出发走 Lua 堆（表、函数的 upvalue、元表；弱表不算引用），找到像 UnLua 实例表（有元表、`objectKey` 字段是 userdata）却不在对象表里的表——UnLua 在对象销毁时会把它从对象表删掉。是推测，`path` 是 Lua 里拿着它的第一条路径（如 `_G.UIManager.Panels[3]`、`<upvalue Panels>`）。
  - `stale`（对象表里还有、UObject 已经不在）、`garbage`（已标记销毁）、`noworld`（Actor、组件、UMG 控件所在的世界已经关了）：对象表本身的问题，`path` 是 Lua 里另外拿着它的路径（只有对象表拿着时没有）。
  - 协程栈上的局部变量走不到。全部结果写进 `Saved/UELink/lua-leaks/leaks-<时间>.json`；大项目走一遍几百毫秒，会卡一下游戏线程。

2.4.0 起 `asset.validate` 能一次查多个：`{"paths": [...]}`（最多 200 个）→ `{"items": [{"path", "result", "errors", "warnings", "issues": [前 5 条]}], "errors", "warnings", "report"}`，完整结果写进 `Saved/UELink/validate/<时间>.json`，`report` 是它的绝对路径。只给 `path` 时照旧回单个资源的结果。

2.4.0 起的编辑器小工具（重定向器、放删 Actor 只在编辑器进程里有；配置和 GameplayTag 哪种进程都有。客户端按 `/info` 的 `ops` 判断）：

- `config.files {}` → `{"items": [{"name", "file"}]}`：进程里加载了哪些配置（`name` 是 `Engine`、`Game`、`Input` 这类短名，`file` 是合并后的文件路径）。
- `config.get {"file"?: "Engine", "section"?, "key"?, "filter"?, "limit"?}`：只读 `GConfig` 里合并后的最终值。
  - 不给 `section` → `{"file", "path", "sections": [名字], "total"}`，`filter` 按名字包含筛，`limit` 默认 500、最多 5000。
  - 给了 `section` → `{"file", "path", "section", "values": [{"key", "value"}], "total", "missing"?}`：同一个键的多个值（数组项）按顺序各占一行；`key` 只要这一个键；没有这个 section 时 `missing: true`。
  - 每一层 ini（Base、Default、平台、Saved）各写了什么不在这里给：客户端自己读磁盘上的那几层文件，和这里的最终值对照（插件、命令行改的值只有这里有）。
- `tags.list {"filter"?, "limit"?}` → `{"items": [{"tag", "source"?}], "total"}`：进程里登记的全部 GameplayTag（ini、DataTable、C++ 原生的，含只因为有子 Tag 才存在的隐式父级），按名字排序。`source` 是来源（ini 文件名、DataTable 包名、`Native`），只在编辑器里有，隐式父级没有。`limit` 默认 20000。
- `tags.refs {"tag", "limit"?}` → `{"tag", "items": [包名], "total", "loading"?}`：哪些资源引用了这个 Tag，查资源注册表的「可搜索名」引用（和引用查看器一样，编辑器保存资源时写的）。`loading: true` 表示注册表还在扫盘，结果可能不全。
- `asset.redirectors {"path": 文件夹, "fix"?: false, "limit"?: 200}` → `{"path", "items": [{"path", "package", "file", "readOnly", "target"? | "broken": true, "referencers": [{"package", "file", "readOnly"}]}], "total", "more"?}`：文件夹（含子文件夹）下的 ObjectRedirector，`referencers` 是还在引用它的包（修复时会被改写并保存）。
  - `fix: true` 时调 `AssetTools.FixupReferencers`（和内容浏览器右键「修复重定向器」同一个入口，不弹签出框）：改写引用方并保存，删掉修好的重定向器。回包多 `fixed` / `left`（`[{"package", "file"}]`，按修完后包还在不在算）、`dirtied`、`note`?。超过 `limit` 个时拒绝，让客户端分批。
  - 修复会保存引用方、删除重定向器文件：客户端按「结构性操作」请用户确认，P4 项目先把 `readOnly` 的文件签出，修完再把删掉的重定向器标记删除。
- `editor.spawn {"class", "location"?: [x,y,z], "rotation"?: [pitch,yaw,roll], "scale"?: [x,y,z], "level"?, "label"?, "folder"?, "select"?: true}` → `{"actor": {"path", "name", "label", "class", "level", "location"}, "tx", "dirtied"?}`：在编辑器关卡里放一个 Actor（`GEditor->AddActor`，和从内容浏览器拖进视口同一套）。`class` 写原生类名或蓝图路径；`level` 是已加载关卡的包名或短名，空为当前关卡。包在编辑器事务里，`tx` 交给 `asset.undo` 撤回。PIE 运行中、后台编辑器里拒绝。
- `editor.destroy {"actors": [Actor 路径]}` → `{"destroyed": [同上的 actor], "missing"?: [没找到的路径], "tx"}`：删掉编辑器关卡里的 Actor（`UEditorActorSubsystem::DestroyActors`），一次最多 100 个，进撤销栈。PIE 里的 Actor 不能用它删。

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

## 7. 给 EmmyLua 调试器提供 UE 对象属性

2.4.0-beta.4 起（装了 UnLua 时）。不走 `/rpc`，是进程内的 C 接口：EmmyLua 调试器（IntelliJ-EmmyLua 的原生 runtime）在断点暂停时展开 UObject，问 UELink 要属性，UELink 用 UE 反射回答。

- **怎么接上**：UELink 不链接 emmy，项目也不用改代码。每 2 秒看一眼进程里有没有 emmy runtime 的模块（`emmy_core` / `emmy_debugger_core`，或静态链进了主程序），有并且导出了 `Emmy_RegisterHostValueProviderV2` 就注册；模块卸载或重新注入后重新注册；新版 runtime 还导出 `Emmy_HasHostValueProviderV2`，核心卸载后在同一地址重新加载（模块句柄不变）时靠它发现注册丢了再注册。旧版 runtime 没有这个导出，UELink 什么也不做。
- **接口**：权威定义在 IntelliJ-EmmyLua 的 `docs/design/variable-view.md` 13.6，UELink 里的副本是 `UELinkCore/Private/UELinkEmmyHostValueAbi.h`，两边要同步改。`describe` 回一行（`valid`、`typeName`、`summary`、`hint`、`kind`、`expandable`），`listChildren` 分页回子项（`total`、`children[]`，每项 `name`、`type`、`value`、`typeName`、`kind`、`attrs`、`hint`、`length`、`expandable`、`childRef`），`releasePause` 在暂停结束时释放这次暂停发出的句柄。
- **显示什么**：UObject 下面按声明类分组（最派生的在前，组内按声明顺序，和细节面板一致），Actor 多一个「组件」分组，最后是「方法 (UFunction)」（带参数和返回类型）。结构体、`TArray` / `TSet` / `TMap` 分页展开（数组下标从 1 开始，和 UnLua 一致）。软引用只显示路径，不加载。
- **安全**：认 Lua 值是哪个 UObject 用 UnLua 的对象表反查，不读 userdata 的内存布局；拿到指针先在活着的对象里确认（`GUObjectArray` + `IsValid`），已销毁的对象只回 `<已失效>`，不读任何属性。只在游戏线程上回答（不在时回 `NEEDS_GAME_THREAD`），GC 进行中不回答。属性内存的句柄只在发出它的那次暂停内有效，每次用之前再确认所属对象还活着。
