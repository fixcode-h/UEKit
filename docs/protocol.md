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
| 资源 | `assets`、`assets.children`、`asset.classes`、`asset.describe`、`asset.refs`、`asset.create`、`asset.duplicate`、`asset.rename`、`asset.delete`、`asset.save`、`asset.revert`、`asset.dirty`、`asset.undo`、`asset.validate`、`asset.export`、`asset.textify`、`asset.snapshot`、`asset.open`、`asset.redirectors`、`asset.import`、`asset.reimport`、`asset.preview`、`editor.functions`、`editor.call` |
| 控件蓝图（编辑器，2.17） | `umg.design`、`umg.anims`、`umg.render` |
| 蓝图 | `bp.outline`、`bp.graph`、`bp.edit`、`bp.compile`、`bp.find`、`bp.node_search` |
| 日志和性能 | `log.tail`、`log.clear`、`perf.live`、`trace.query` |
| 编辑器 | `pie`、`livecoding`、`livecoding.status`、`tests.run`、`tests.status`、`editor.selection`、`editor.select`、`object.reveal`、`editor.spawn`、`editor.destroy`、`editor.map`（2.22） |
| Lua 声明（编辑器，2.5） | `luadecl.status`、`luadecl.enable`、`luadecl.run` |
| 作业（2.13） | `job.start`、`job.get`、`job.wait`、`job.stop`；`job.batch`（2.14，见「批量」） |
| Lua（装了 UnLua 时） | `lua.status`、`lua.objects`、`lua.gc`、`lua.get`、`lua.set`、`lua.exec`、`lua.check`、`lua.reload`、`lua.modules`、`lua.errors`、`lua.inspect`、`lua.call`、`lua.snap`、`lua.profile`、`lua.leaks`、`lua.instances`、`lua.config`（2.20） |

`asset.snapshot {"out", "package", "file"?}`（2.14）：资源的结构化快照，给面板资源页的「版本差异」逐项比较。`file` 是某一版资源的临时文件（和 `asset.textify` 一样挂到临时挂载点加载、用完卸载）；不给 `file` 就读编辑器里已加载的那份（含没保存的改动）。快照写进 `out`（面板给的绝对路径，UTF-8 紧凑 JSON），回包只有 `{out, bytes, class}`。快照里的临时包名都换回 `package`。形状：

- `class`、`props`（属性列表，每项和 `props` 操作一样带 `name`/`displayName`/`category`/`type`/`value`，跳过 Transient，值不截断）、`note?`；
- 蓝图另有 `blueprint`：`parent`、`interfaces[]`、`variables[{name, type, category, flags[]}]`、`components[{name, class, parent, source: native|scs|inherited, props}]`、`graphs[{name, kind, nodes[{id, guid, title, class, comment?, pins{引脚: 默认值}, links["出引脚->节点id.入引脚"]}], pseudo}]`，`props` 是类默认值；
- 其他资源另有 `subobjects[{name(相对资源的路径), class, props}]`，数据表 / 曲线表另有 `rows`（`GetTableAsJSON` 的原文）、`rowStruct`。
- 控件蓝图（2.21）的 `blueprint` 还有 `widgets[{name, class, path, parent, props, slotClass?, slot?}]`（设计时控件树，按 `path` 排序，`path` 和 `umg.design` 一致、`parent` 是父控件的路径；`slot` 是插槽对象的存盘属性）和 `anims[{name, length, channels[{target, channel, keys[[秒, 值]]}]}]`（通道和 `umg.anims` 一致，每条通道最多 200 个关键帧）。
- 2.21 起带 `file` 时按引擎「和版本库比较」的方式加载（`LOAD_ForDiff | LOAD_DisableCompileOnLoad`），不在加载时编译这一版的蓝图（`asset.textify` 同样）：编辑器里同一个蓝图开着时，以前编译副本会很慢、报编译错或卸不掉。

`trace.query {"file", "what", "live"?, ...}`：用 TraceServices 分析 .utrace（what 的各项见面板 `ue/server/trace-query.ts`）。每个回包都带 `analyzing`、`duration`（秒，从进程启动算）；2.8.1 起还带 `readBytes` / `fileBytes`（已交给分析器的字节数 / 文件大小）、`origin`（第一帧的开始时间，客户端减掉它就是「从录制开始算」的秒数，还没有帧时是 0）、`caughtUp`（读到了文件当前的末尾）、`live`（正在跟着读）。`live: true` 表示这份文件还在录：读到末尾不结束，等新数据接着分析，没分析完也能查已经分析到的部分；客户端看着时每隔几秒再带 `live: true` 查一次续上，30 秒没人续就把现有数据读完结束。2.8.1 之前文件一变就从头重新分析，录制中的文件永远停在 `analyzing`。

2.9.0 起 `trace.query` 认得 Lua 计时器（录制时开了 `lua` 通道，见下面「Lua 函数进 trace」）。Lua 计时器按注册时的文件认：文件以 `.lua` 结尾的是 Lua 函数，文件是 `<lua:C>` 的是 C 函数。`butterfly`、`stack`、`instances` 不变（客户端用 `luainfo` 的表认 Lua 计时器）：

- `what=info` 多一个 `lua`：这份 trace 里有没有 Lua 计时器（抓取列表打标签用）。
- `what=luainfo` → `{"lua": bool, "timers": {"<计时器 id>": {"file", "line", "c"?: true}}}`：全部 Lua 计时器的文件和行号（Lua 函数是定义行，C 函数是 0），客户端打开 trace 时取一次缓存。没有时 `{"lua": false, "timers": {}}`。
- `what=timers` 加参数 `lua?: true`（只要 Lua 计时器，含 C 函数）、`mergeC?: true`（C 函数不单列：它的独占时间算进调用它的那一块的独占时间；C 函数里又回调进 Lua 的部分还是算那些 Lua 函数自己的）。Lua 计时器的行多 `file`、`line`、`c?: true`。
- `what=tree {"t0", "t1", "lanes"?, "root"?, "luaOnly"?: true, "mergeC"?: true}` → `{"root": 节点}`：自上而下的调用树（火焰图用），范围内的块按「从起点到自己」的计时器路径合并，多条泳道合进一棵树。节点格式和 `butterfly` 的树一样：`{"id", "name", "count", "incl", "excl", "children", "more"?}`（毫秒；每层按 `incl` 倒序最多 30 个、最多 16 层，`more` 是没列出的子节点数）。根节点是虚的：`id` 为 -1、`name` 为「全部」、`incl` 是下面各起点的和、`excl` 为 0。起点：
  - 给了 `root`（计时器 id）：它最外层的那次（和 `butterfly` 的被调用者一致），这时根节点下只有它一个子节点；`luaOnly` 不起作用。
  - `luaOnly: true`：最外层的 Lua 计时器，跳过上面的 UE 作用域，不同 UE 回调里进来的同一个 Lua 函数合在一起；没有 Lua 祖先的非 Lua 块不算。
  - 都没给：泳道的根。
  - `mergeC: true` 时 C 函数节点不出现，独占时间并进父节点，它下面回调进来的 Lua 函数挂到父节点下。

2.11.0 起 `trace.query` 是**异步操作**：编辑器在游戏线程上只找会话（必要时开始分析），计算交给线程池，算完再回 HTTP 回复（客户端照常等回复，不用改）。同时最多算 4 个查询、多的排队；单次查询最多 20 秒，超了照样 `ok: true`，回已经算出的部分并带 `truncated: true`、`truncatedNote`（中文，说明怎么缩小范围）。编辑器退出时排队的查询回「编辑器正在退出，查询取消了」。`GET /info` 的 `ops` 照旧列出 `trace.query`。

2.11.0 新增和扩充的 what（时间都是 trace 时间、秒；耗时毫秒；`t0` / `t1` 是范围，`lanes` 是泳道 id 数组，GPU 是 -1）。录制时没开对应通道、或者 UE 启动时没加需要的参数时返回 `ok: false` 并说明要开什么；`UELinkCompat.h` 里某类的 `UELINK_TRACE_*` 关掉时返回「编译时关掉了」。2.11.1 起各类按引擎里有没有对应的 TraceServices 头文件自动开关；UE 5.4 上 `files`（没有文件活动的头文件）和 `regions`、`memtags`、`allocs`（接口写法不同）是关着的，`session` 的 `has` 里对应项为 false：

- `frames {"type"?: "render"}`：`type=render` 取渲染帧，默认游戏帧。
- `counters {"stats": true}`：不给 `names` 时，列表每条多 `n`、`min`、`max`、`avg`、`first`、`last`、`firstT`、`lastT`（范围内原始值，不降采样）。
- `framestats {"type"?, "f0"?, "f1"?, "lanes"?}` → `{"type", "f0", "count", "total", "ms": [帧时长], "lanes": [{"id", "name", "busy": [每帧忙的毫秒]}]}`：每帧每条线程深度 0 的块落在这一帧里的时间（两位小数）。不给 `lanes` 时是 GPU 和名字带 GameThread / RenderThread / RHIThread 的线程。GPU 的帧和游戏帧不完全对齐（GPU 晚一两帧），客户端看分布不逐帧对。
- `framediff {"frames": [帧号], "base"?: [帧号], "lanes"?, "top"?, "type"?}` → `{"base": {"frames", "ms"}, "frames": [{"frame", "start", "ms", "items": [{"id", "name", "excl", "base", "gpu"?, "file"?, "line"?, "c"?}]}]}`：每个目标帧里独占时间比基线（基线帧的平均）涨得最多的计时器（只列涨了的，最多 `top` 个，默认 8）。不给 `base` 时取帧时长排在 P40–P60 的帧（最多 120 帧）当基线；`frames` 最多 200 帧。开始时间落在这一帧里的块记它的独占时间。
- `session` → `{"name", "platform"?, "app"?, "commandLine"?, "branch"?, "build"?, "changelist"?, "config"?, "target"?, "gameFrames", "renderFrames", "threads", "counters", "has": {"cpu", "gpu", "lua", "regions", "memtags", "allocs", "loads", "files", "tasks", "switches", "net"}}`。
- `regions {"t0"?, "t1"?, "filter"?, "top"?}` → `{"items": [{"text", "start", "ms", "depth"}], "total"}`（`region` 通道）。
- `memtags {"t0"?, "t1"?, "name"?, "tracker"?: "Default", "px"?}` → `{"tracker", "items": [{"name", "avgKB", "peakKB", "firstKB", "lastKB", "t"?, "v"?}]}`（`memtag` 通道，UE 要 `-llm`）。`name` 按名字筛；和某个标签同名时那一项带降采样走势 `t` / `v`（KB，每段取最大）。
- `allocs {"at"? | "t0"/"t1", "groupBy"?: "tag|callstack|size", "filter"?, "top"?, "depth"?}` → `{"mode": "live|growth", "groupBy", "totalKB", "totalCount", "groups", "items": [{"key", "kb", "count", "maxKB", "stack"?: [帧]}], "partial"?}`（`memalloc` 通道，UE 要用 `-trace=memory` 启动）。给 `at`：那一刻还活着的分配；不给：`t0` 到 `t1` 之间分配、到 `t1` 还没释放的。`stack` 是这一组一个调用栈样本最上面 `depth` 层（跳过分配器自己的帧；符号没解析出来时是地址）。超时时 `partial: true`。
- `loads {"t0"?, "t1"?, "sort"?: "time|size", "filter"?, "top"?}` → `{"packages", "mainMs", "asyncMs", "items": [{"package", "mainMs", "asyncMs", "kb", "exports"}], "types": [{"type", "totalMs", "count", "maxMs"}]}`（`loadtime`、`assetloadtime` 通道）。
- `files {"t0"?, "t1"?, "sort"?: "time|size", "filter"?, "top"?}` → `{"ops", "files", "items": [{"path", "op", "start", "ms", "kb", "failed"?}], "byFile": [{"path", "ms", "kb", "ops"}]}`（`file` 通道）。
- `tasks {"t0"?, "t1"?, "sort"?: "exec|wait", "filter"?, "top"?}` → `{"tasks", "names", "items": [{"name", "count", "execMs", "execMaxMs", "waitMs", "waitMaxMs"}], "slowest": [{"name", "start", "execMs", "waitMs", "lane"}]}`（`task` 通道）。`wait` 是从发起到开始执行的排队时间。
- `switches {"t0"?, "t1"?, "lanes"?}` → `{"items": [{"lane", "name", "runMs", "runShare", "switches"}]}`（`contextswitch` 通道，Windows 要管理员权限）：每条线程真正在 CPU 上跑的时间。
- `net {"t0"?, "t1"?}` → `{"instances": [{"instance", "server", "connections": [{"name", "address", "out"?: {"packets", "kb", "maxBytes", "kbPerSec"}, "in"?: {…}}]}]}`（`net` 通道）。

2.17.0 起按 UE 控件蓝图编辑器的样子在网页上看 UMG（只在编辑器进程里有，`umg.render` 还要有 Slate 渲染器，后台编辑器里不行；`features` 里有 `umg.design`、`umg.render`、`umg.anims`，旧桥接没有时网页降级成近似预览）。`path` 都认控件蓝图资源路径（`/Game/A/WBP_X`、`/Game/A/WBP_X.WBP_X`）、生成类（`.WBP_X_C`）、类默认对象和蓝图里某个控件模板的路径：

- `umg.design {"path"}` → `{"asset", "parent"?, "root": 节点 | null, "animations": 动画个数}`：控件蓝图设计时的控件树（`UWidgetBlueprint::WidgetTree`，不是运行中的实例，不用开 PIE；`umg.tree` 只认运行中的实例）。`parent` 是父类路径（父类就是 `UserWidget` 时不写）；没有根控件时 `root` 为 `null`。
  - 节点 `{"name", "class", "path", "var"?: true, "visibility"?, "slot"?, "active"?, "user"?: true, "asset"?, "children": [节点], "truncated"?: true}`：`path` 是控件模板的完整对象路径（`/Game/A/WBP_X.WBP_X:WidgetTree.Btn_OK`），拿它调 `props` / `get` 读细节；`var` 是「是变量」；`visibility` 不是 `Visible` 时才写（`Collapsed`、`Hidden`、`HitTestInvisible`、`SelfHitTestInvisible`）；`slot` 是插槽类短名（`CanvasPanelSlot`）；`active` 是 WidgetSwitcher 的当前页（第 0 页也写）；嵌套的控件蓝图标 `user`，`asset` 是它的资源路径，不展开它的内部（点进去再对它调 `umg.design`）。子控件按面板的子控件和命名插槽（Named Slot）的内容走。
  - 上限和 `umg.tree` 一样：深度 64、节点 5000，超了的那一层标 `truncated`。
- `umg.anims {"path"}` → `{"animations": [{"name", "length", "tracks": [{"target", "channel", "keys": [[秒, 值, "cubic|linear|constant"]]}], "skipped"?: [{"target", "channel"?, "reason"}]}], "truncated"?: true}`：只读，形状和 AssetForge 配置的 `animations` 一致（没装 AssetForge 也能看）。
  - `length` 是播放范围的秒数；`target` 是控件路径（`Root/Shell/Btn_OK`，第一段是根控件名，和 AssetForge 一致）；`channel` 沿用 AssetForge 的白名单：`RenderOpacity`、`RenderTransform.Translation.X|Y`、`RenderTransform.Scale.X|Y`、`RenderTransform.Shear.X|Y`、`RenderTransform.Angle`、`slot.Padding.Left|Top|Right|Bottom`。UE 编辑器建的 2D 变换轨道和整条 Padding 轨道拆成这些分量。
  - 读不懂的轨道（颜色、可见性、事件、绑在控件蓝图自身上的、别的属性）列在这个动画的 `skipped` 里，不悄悄丢；一条轨道多个片段时只读第一段，也记一条。
  - 关键帧总数最多 1500，超了截断并回 `truncated: true`。
- `umg.render {"path", "width"?: 1920, "height"?: 1080, "dpi"?: 1, "anim"?, "time"?: 0}` → `{"file", "width", "height", "widgets": [{"name", "x", "y", "w", "h"}], "ms", "note"?}`：离屏渲染一帧预览图。
  - 做法照 UE 设计器：在编辑器世界里建一个打了设计器标志的实例（不跑 Construct，跑 PreConstruct），用 UMG 的 `FWidgetRenderer` 画进 RenderTarget、读回像素存 PNG（带透明通道）。`width` / `height` 是图的像素（16–7680 × 16–4320），`dpi` 是 UE 设计器的 DPI 缩放（0.25–4，布局区域 = 像素 / dpi）。同步做，一次在百毫秒级。
  - 图写进 `<项目>/Saved/UELink/preview/<资产名>-<路径 CRC>.png`，同一个资产覆盖旧图（网页按 `ms` 或时间戳防缓存）。
  - `widgets` 是控件蓝图自己这棵树里每个画出来的控件在图里的矩形（像素，按绘制几何，不含渲染变换的旋转），名字和 `umg.design` 的 `name` 对应；折叠、隐藏、零尺寸的不列，最多 2000 个；取不到时为空数组并在 `note` 里说明。
  - 给了 `anim`（动画名，见 `umg.anims`）时按 `time`（秒）求该动画白名单通道的值写到实例上再画（曲线首尾之外按首尾值）；找不到动画、有不支持的轨道时照常画，并在 `note` 里写明。
  - 蓝图有没编译的改动时画的是上次编译的结果（`note` 里写明）；编译出错、没有生成类、抽象类时返回 `ok: false` 和原因。

`asset.preview {"path", "width"?: 512, "height"?: 512, "view"?: {"yaw"?, "pitch"?, "distance"?}, "select"?}` → `{"file", "width", "height", "kind", "mode": "thumbnail", "ms", "note"?}`（2.18，只在编辑器进程里有，要有渲染器；`features` 里有 `asset.preview`）：把资源离屏画成一张 PNG，给面板资源查看页预览。
  - 支持的类型（`kind`）：`staticMesh`、`skeletalMesh`、`material`（材质和材质实例，画在资源缩略图设置的图元上，默认是球）、`texture`、`blueprint`（只认 Actor 蓝图：缩略图场景里放它的组件）。`path` 也认蓝图的生成类（`_C`）和类默认对象。控件蓝图回 `ok: false` 让改用 `umg.render`；其他类型、没有生成类、没有可见组件的蓝图、网格或贴图还在异步编译时回 `ok: false` 和原因。
  - 这一版用编辑器的缩略图渲染（`ThumbnailTools::RenderThumbnail`，和内容浏览器缩略图同一套画法），回包固定 `mode: "thumbnail"`，图是不透明的。以后做了真正的预览场景会换成别的 `mode`，客户端按 `mode` 判断。缩略图模式没有每个组件在图里的位置，所以不回 `components`（以后的模式对蓝图会给 `components: [{"name", "rect": {"x", "y", "w", "h"}}]`，算不出时省略）；给了 `select` 时只在 `note` 里说明没生效。
  - `width` / `height` 是图的像素（16–2048）。`view` 照 UE 缩略图设置的轨道相机：`yaw`、`pitch` 是角度（`pitch` 负数是从上往下看，-90–90），`distance` 是在自动取景距离上加的偏移（0 是刚好装下包围球，正数更远）；没给的分量用资源自己的缩略图设置（没有就是 UE 默认的 yaw -157.5、pitch -11.25、distance 0）。视角是画的时候临时改资源的缩略图设置、画完改回，不标脏、不进撤销。贴图没有视角，给了 `view` 只写 `note`。
  - 图和 `umg.render` 写在同一个目录：`<项目>/Saved/UELink/preview/<资产名>-<路径 CRC>.png`，同一个资产覆盖旧图（网页按 `ms` 或时间戳防缓存）。同步做，一次几十到几百毫秒。
  - 蓝图有没编译的改动时画的是上次编译的结果（`note` 里写明）。

2.18.0 起蓝图读操作补了给网页画图和大纲用的字段（`features` 里有 `bp.layout`；都是默认值时省略）：

- `bp.graph {"path", "graph", "format": "json", "layout"?: true, ...}`：节点列表（整张图分页或 `around` 周围几跳）每个节点多 `x`、`y`（`NodePosX` / `NodePosY`，图表坐标）；注释节点另有 `w`、`h`（注释框的尺寸）和 `color`（`#rrggbb`，sRGB）。不给 `layout` 时和以前一样不带坐标。单个节点的详情（给 `node`）本来就有 `x`、`y`，注释节点现在也带 `w`、`h`、`color`。
- `bp.outline`：
  - `dispatchers[]` 每项多 `params: [{"name", "type"}]`（分发器签名的参数，没有参数时省略）。
  - `graphs[]` 里 `kind: "function"` 的多 `category`（函数的分类，`Default` 时省略）、`access`（`protected` / `private`，public 时省略）、`const: true`。
  - `variables[]`（和 `dispatchers[]`）多列出父蓝图链上定义的变量，标 `inherited: true`，排在自己定义的后面；默认值按当前蓝图类默认对象上的值（子类改过就是改过的值）。C++ 父类的属性不列。

`components {"path"}` → `{"items": [{"path", "name", "class", "parent", "root", "scene", "created": "native|blueprint|script|instance", "inherited"?}], "scs"?}`。
2.2.0 起 `path` 是蓝图资源（或它的类默认对象）时，除了 C++ 组件还带上蓝图组件列表（自己和父蓝图的 SCS）里的组件，`path` 是组件模板，用 `props` / `set` 读改它就是改蓝图里这个组件的默认值；`inherited` 标出来自父蓝图的，回包带 `"scs": true`。旧版本没有 `scs`，客户端自己用 `bp.outline` 的 `components` 补。

`set {"path", "prop", "value"?, "array"?, "index"?, "key"?, "class"?}` → `{"prev", "value", "asset", "replicated", "client", "created"?, "index"?}`。三种写法：

- 只给 `value`：整体写（JSON，或 UE 文本格式）。
- 给 `array`（细节面板容器的那几个按钮，直接改容器，不整体替换，实例化子对象里的数据不会丢）：
  - 数组：`add`（末尾加）、`insert`（在 `index` 前插）、`duplicate`（复制第 `index` 个，实例化子对象会复制成新对象）、`remove`（删第 `index` 个）、`clear`。2.13.0 起 `add` / `insert` 可以带 `value`（新元素的值）或 `class`（元素是对象时新建这个类的内嵌子对象放进去），回包 `index` 是新元素的下标。
  - 集合、映射（2.13.0 起）：`add` / `remove` 按 `key`（元素或键，JSON 或 UE 文本格式，和属性路径 `Tags["Fire"]` 里的写法一样），`clear` 清空。映射 `add` 可以带 `value` 或 `class`（值是对象时新建子对象）；键已经在时拒绝，改已有键的值用 `prop=名字[键]` + `value`。旧版本集合和映射只认 `clear`。
- 只给 `class`（2.13.0 起）：给对象属性新建内嵌子对象（细节面板实例化属性的「选类」下拉），`prop` 可以是 `Triggers[2]` 这样的数组元素。子对象的 Outer 是属性所在的对象，标志随它传下去（类默认值里的是原型）。属性要是 Instanced，或者类是 EditInlineNew；抽象类、Actor、不是属性要的类都拒绝。回包 `created` 是新子对象的路径。`class` 写原生类名或蓝图类路径（`/Game/BP_Foo`，没加载时会加载）。

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
- `object.reveal {"path", "action"?: "select"|"open", "focus"?: true}` → `{"did": "actor"|"asset"|"blueprint", "path", "name", "pie"?: true}`（2.10.0 起，只在编辑器进程里有）：给任意对象路径（`GetPathName`），换算成编辑器里能选中、能打开的东西。EmmyLua 调试器变量树右键「在 UE 中选中 / 打开资产」用它，路径取自第 7 节宿主值提供者给的 `hint`。
  - `select`（默认）：对象是 Actor 或在 Actor 里（组件、子对象）→ 在关卡里选中那个 Actor（`focus` 为 true 时视口对焦，PIE 里的 Actor 回 `"pie": true`）；是资源或资源里的子对象 → 在内容浏览器定位资源；都不是但类是蓝图生成的（UMG 控件、普通 UObject 实例、类默认对象、蓝图类本身）→ 定位它的蓝图。
  - `open`：资源（含资源里的子对象）打开资源编辑器；Actor、控件这类实例打开它（或外层第一个蓝图类对象）的蓝图。原生类的对象、关卡本身不打开，回错误。
  - 路径带 `:`（关卡里、Actor 里的子对象）只找内存里的，不按需加载（避免把整张地图加载进来）；资源路径找不到时加载。找不到时回错误说明原因（对象已销毁、PIE 已结束、在另一个进程里）。
  - 注意：Lua 停在编辑器进程（PIE）的断点上时游戏线程卡在调试器里，`/rpc` 要等继续运行后才执行；客户端自己决定是等还是先提示（EmmyLua 是继续运行后再发）。
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

2.19.0 起 Lua 对象列表带状态、类链和归属（代码在 `AgentPanelLuaObjects.cpp`）：

- `lua.objects {"class"?, "prefix"?, "module"?, "limit"?: 200, "world"?}` → `{"items": [{"path", "name", "class", "module", "state"?, "world"?, "map"?, "classChain": [父类名...], "owner"?, "ownerName"?}], "more", "bound", "stale", "memoryKB"}`：
  - `class` 按 `IsA` 过滤（含子类），蓝图类可省 `_C`；`prefix` 是对象名前缀；`module` 是模块名包含的字。
  - `state` 只在不是正常活着时有：`garbage`（已标记销毁，下次 UE 垃圾回收收掉）、`noworld`（Actor、组件、UMG 控件所在的世界已经关了）。2.18 及以前已标记销毁的对象不列。
  - `world` 是所在世界的 id（同 `outliner` 的世界 id，世界已关时没有），`map` 是世界对象名。
  - `classChain` 是父类名，从直接父类一直到 `Object`；面板据此做「按类名搜（含子类）」和 UE 类继承树。
  - `owner` / `ownerName`：外层链上第一个 Actor 或 UMG 控件（组件 → Actor，子控件 → 父界面）；Actor 没有这样的外层时是它的 Owner。
  - 对象表里的地址先在活对象集合里查（一次遍历全部 UObject），查不到的不读、只计进 `stale`：UnLua 分支在对象销毁时漏删对象表项时，这些是残留的已释放地址。
- `lua.gc {"action": "collect" | "count"}` → `{"beforeKB", "afterKB", "ms"?}`：`collect` 跑一次完整的 Lua 垃圾回收（`collectgarbage("collect")`），`count` 只看内存。
- `lua.inspect` 的 `chain` 每层加 `kind`：`lua`（`package.loaded` 里的模块）、`ue`（不是模块，但在注册表里有字符串键，即 UnLua 给 UE 类建的元表，`name` 是这个键）；都认不出时没有 `kind`、`name` 为空。2.19 起最多走 16 层（以前 6 层）。

2.19.1 起：

- `lua.objects` 每项是类（UClass）时带 `"isClass": true`。没开始玩时 UnLua 只给类建了表，对象表里全是类；类不能当实例传给 UE 函数。类的 `module` 问它的默认对象。
- `module`（`lua.objects`）和 `chain[].name`（`lua.inspect`）在 `package.loaded` 里对不上时，按表里第一个 Lua 函数所在的文件推：路径里 `/Script/`（或 `/Scripts/`）之后、去掉 `.lua`、`/` 换成 `.`。
- `lua.inspect` 的 `chain` 走过的表不再走（UnLua 给 UE 类建的元表常是自己的元表，2.19.0 会把同一层重复到 16 次）。`kind: "ue"` 的层能按反射找到类时，`name` 换成短类名（C++ 类带前缀，如 `AActor`；蓝图类是生成类名），完整注册表键放 `path`；最后一层 UE 类之后按反射补上父类直到 `UObject`，这些层带 `"reflected": true`、没有函数。
- 实例表自己身上的 C 函数（UnLua 第一次调 UE 函数时缓存的包装）`from` 记成第一层 UE 类，不算「自己的」。

2.4.0 起的 Lua 深度调试（「装了 UnLua 时」才有，代码在 `AgentPanelLuaDebugOps.cpp`）。三者都靠 Lua 的调试钩子或只读遍历，不改游戏状态；不做断点（桥接在游戏线程上处理请求，游戏线程一停桥接也停了）：

- 钩子：快照点用行钩子、采样用调用 / 返回钩子，共用一个钩子函数。挂上前记下原来的钩子（IDE 的 Lua 调试器），事件按它原来的掩码转过去，都撤了再放回去。**挂上之后 IDE 的 Lua 调试器又换了钩子时不抢回来**（2.5.3 起；以前每帧抢回，会让 IDE 断点失灵）：这期间快照点和采样收不到事件，`lua.snap list`、`lua.profile start` 带 `warning`；调试器断开把钩子清掉后自动接上。`lua.exec` / `lua.call` / `lua.reload` 执行期间临时换成超时钩子，那段代码里的快照点和采样看不到；执行完只在钩子还是超时钩子时才放回原来的（期间调试器换过就不动）。钩子按协程各一份，挂之前就在跑的协程看不到（回包 `note` 说明）。
- `lua.snap {"action": "add", "file", "line", "hits"?: 1, "ttlSec"?: 600}` → `{"snap": {"id", "file", "line", "hits", "max", "active", "created", "expiresIn"}, "note"}`：执行到这一行时记下函数、局部变量（表顺带前 20 项简单值）、upvalue 和调用栈，游戏不暂停。`file` 可以是磁盘路径、相对脚本根的路径或模块名（`UI.BagPanel`），和 chunkname 按路径段从后往前比。命中 `hits` 次（最多 20）或过了 `ttlSec`（10–3600 秒）自动撤掉；同时最多 20 个。
  - `{"action": "list", "id"?}` → `{"items": [...], "warning"?}`：不给 `id` 只列概要；给 `id` 时带 `captures: [{"at", "fn", "src", "file"?, "line", "locals": [{"name", "t", "v"?, "fields"?}], "upvalues"?, "stack": [行]}]`。钩子被别的调试器换掉时带 `warning`。
  - `{"action": "remove", "id"?}` → `{"removed"}`：不给 `id` 全部撤掉并清记录。
  - 推送事件 `lua.snap`（命中一次推一次）：`{"id", "hits", "max", "active", "capture"}`。客户端订阅 `topics` 里写 `lua.snap`。
- `lua.profile {"action": "start", "seconds"?: 5}` → `{"started", "seconds", "note", "warning"?}`：采几秒（1–60），按函数（闭包）累计调用次数、总用时、自身用时，C 函数（UnLua 导出的 UE 接口）也算。到时间自己停；`{"action": "stop"}` 提前停。
  - `{"action": "result", "sort"?: "self|total|calls", "limit"?: 50}` → 还在采时 `{"running": true, "remaining"}`；采完 `{"running": false, "startedAt", "seconds", "events", "total", "items": [{"fn", "src", "file"?, "line", "c", "calls", "totalMs", "selfMs"}], "report"}`（默认按 `selfMs` 排；`fn` 2.9.0 起和 Lua trace 的计时器同名，见下面「Lua 函数进 trace」），`file` 是完整路径（chunkname 以 @ 开头时才有，`src` 太长会被截断），`c` 表示 C 函数；全部函数写进 `Saved/UELink/lua-profile/profile-<时间>.json`。递归函数的总用时会重复计入；协程挂起的时间算在协程里的函数上。
- `lua.leaks {"limit"?: 100, "maxVisit"?: 200000, "objectKey"?: "Object"}` → `{"items": [{"kind", "module"?, "path"?, "object"?, "class"?}], "total", "visited", "truncated", "ms", "report", "note"}`：找 Lua 还拿着、UObject 却已经没了的（专查「关了界面控件释放不掉」）。`kind`：
  - `destroyed`：从 `_G` 和注册表出发走 Lua 堆（表、函数的 upvalue、元表；弱表不算引用），找到像 UnLua 实例表（有元表、`objectKey` 字段是 userdata）却不在对象表里的表——UnLua 在对象销毁时会把它从对象表删掉。是推测，`path` 是 Lua 里拿着它的第一条路径（如 `_G.UIManager.Panels[3]`、`<upvalue Panels>`）。
  - `stale`（对象表里还有、UObject 已经不在）、`garbage`（已标记销毁）、`noworld`（Actor、组件、UMG 控件所在的世界已经关了）：对象表本身的问题，`path` 是 Lua 里另外拿着它的路径（只有对象表拿着时没有）。
  - 协程栈上的局部变量走不到。全部结果写进 `Saved/UELink/lua-leaks/leaks-<时间>.json`；大项目走一遍几百毫秒，会卡一下游戏线程。
- `lua.instances {"module"?, "classFields"?, "roots"?, "nameFields"?, "objectKey"?, "limit"?: 200, "maxVisit"?, "world"?}`（2.20）→ 给了 `module`：`{"module", "items": [{"ref", "path", "module", "name"?, "byField"?}], "total", "more", "visited", "truncated", "ms", "rootsMissing"?}`；没给：`items` 是 `[{"module", "count"}]`（每个类有几个实例，多的在前）。找**纯 Lua 实例**：Lua 框架自己 `setmetatable({}, Class)` 出来、不绑 UObject 的表（UnLua 对象表里没有，`lua.objects` 列不出）。
  - 从 `roots`（`_G.GameApp.Modules` 这种点路径，`_G.` 可省）走 Lua 堆；不给 `roots` 从 `_G` 和注册表走整个堆（和 `lua.leaks` 同一种走法：表的键值、元表、函数的 upvalue，弱表不算），最多走 `maxVisit` 个表 / 函数。
  - 一张表算 `module` 的实例：元表链（元表、元表的 `__index`，往上最多 12 层）上有 `package.loaded[module]` 那张表（子类的实例也算）；或者 `classFields` 里某个字段的值写的就是这个模块（`SubModulePath = "/Module/A/B"`）。模块名比较时 `.`、`/`、`\` 一样，不分大小写，首尾的 `/` 和 `.lua` 不算。模块表自己（类）、`objectKey` 字段是 userdata 的（UnLua 绑了 UObject 的实例表，归 `lua.objects`）不算。
  - `ref`：`luaref:<编号>`，同一张表总是同一个编号（弱引用，不挡回收）。`lua.get` / `lua.set` / `lua.call` / `lua.inspect` / `lua.exec` 的 `path` 填它就以这张表为起点（`lua.exec` 里是 `self`）；表被回收、模块重载后报错让客户端重新找。`path` 只是给人看的（第一次走到它的那条路），不能拿来取值。
  - `name`：`nameFields` 里第一个有值（字符串或数字）的字段。`byField`：按类名字段认出来的。
  - `classFields`、`roots`、`nameFields`、`objectKey` 不给时用 `lua.config` 存的。没加载的模块：配了 `classFields` 时照样按字段找，没配报错。
- `lua.config {"set"?: {"scriptRoot", "objectKey", "classFields", "roots", "nameFields", "maxVisit"}}` → `{"config", "file"}`（2.20）：面板「权限与设置 → Lua」下发的设置，`set` 整份替换并写进 `Saved/UELink/lua-config.json`（编辑器重启后照样生效，没连面板跑的 GameTest 也用它）；不给 `set` 只读。各项留空 = 自动：`scriptRoot`（`lua.modules` 等没给 `root` 时的脚本根，空 = `Content/Script`）、`objectKey`（`lua.leaks`、`lua.instances` 认 UnLua 实例表的字段，空 = `Object`）、其余见 `lua.instances`。

2.9.0 起 Lua 函数进 Unreal Insights trace（「装了 UnLua 时」才有，代码在 `AgentPanelLuaTrace.cpp`）。不是一个操作：客户端录 trace 时在通道串里加上下面的通道（`Trace.File`、`Trace.Start` 或命令行 `-trace=default,lua`），录下的 .utrace 用 `trace.query` 或 UE 自带的 Unreal Insights 都能看到 Lua 函数：

- 通道 `lua`：每个 Lua 函数是游戏线程上一个 CPU 计时器作用域，和 UE 自己的作用域嵌在一起（`Tick` → … → `Player.BP_PlayerController_C:ReceiveTick` → `UE.GetVelocity`）。通道 `luac`：C 函数（UnLua 导出的 UE 接口、Lua 标准库）也单独成块，只在 `lua` 也开着时生效；不开时 C 函数不出块，时间算进调用它的 Lua 函数，trace 也小一些。面板默认录 `lua,luac`。还要开着 `cpu` 通道（`default` 里有）。
- 计时器的名字、文件、行号（`OutputEventType` 注册，每个函数只注册一次，之后每次调用只发 id）：
  - Lua 函数：名字按顺序取第一个能用的：有调用方名字时 `模块:函数`；没有（UnLua 从 C++ 调进来的 `ReceiveTick` 这类）时在 `package.loaded[模块]` 表里找值是这个函数的字段名，`模块:字段`；再找不到 `模块:定义行`；文件的主 chunk 是 `模块:(main)`。模块名是文件相对 `Content/Script/` 的路径去掉 `.lua`、`/` 换成 `.`（`Player.BP_PlayerController_C`）。文件 = 完整路径（chunkname 去掉 `@`；不是文件的 chunk 用 `short_src`），行号 = 定义行。文件按字节原样写进 trace（引擎的接口是 ANSI），路径里有中文时 Insights 里显示会乱。
  - C 函数：Lua 标准库是 `lua.<名>` / `lua.<库>.<名>`（`lua.ipairs`、`lua.string.format`），其他（UnLua 导出的 UE 接口）是 `UE.<调用处的名字>`；文件固定 `<lua:C>`，行号 0。
- 每帧计数器（只在 `lua` 通道开着时发；引擎只在 `counters` 通道也开着时才写进 trace，客户端录 Lua 时一并打开）：`Lua/FrameMs`（浮点，这一帧所有顶层 Lua 调用的总时长）、`Lua/Calls`（整数，这一帧的调用次数，含 C 函数）、`Lua/MemoryKB`（整数，`lua_gc(LUA_GCCOUNT)`）。
- 开销：没在录制、或录制里没开 `lua` 通道时**零开销**——不挂钩子、不发事件和计数器、不注册每帧回调，只挂着「开始 / 停止录制」两个委托；录制开始后才每帧看一次通道开关，开了才挂调用 / 返回钩子（和快照点、采样共用一个钩子函数），停止录制时摘掉。录制期间 Lua 会变慢一些（每次调用 / 返回多一次钩子和一个 trace 事件），看占比不看绝对值。
- 时间轴保证严格嵌套：协程切换时先结束上一个协程还开着的函数、切回来再按它的栈重新开始（同一个协程函数在每次恢复时各出现一段）；调 `coroutine.yield` 时就先结束这个协程的帧；Lua 里的 `pcall` 返回时把更深的帧一起结束；尾调用按层数对齐，被替换的帧在同一时刻结束。从 C++ 调进来又报错的，靠项目侧报错钩子 `FAgentPanelLua::CaptureError` 收尾（2.9.0 起它会先把这次调用进来的 Lua 帧结束）；没接这个钩子、或 C 里直接 `lua_yield` 的，那一帧结束时按影子栈全部收尾，只影响那一帧，录制结束时日志里打一条「有 N 帧 Lua 没正常收尾」。
- IDE 的 Lua 调试器接管着钩子时收不到 Lua 函数（和快照点、采样一样不抢回来），日志里打一条警告；钩子按协程各一份，录制开始前就在跑的协程看不到。
- `lua.profile` 2.9.0 起用同一套名字解析，`fn` 不再是一堆 `?`。

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
- `pie {"action": "start", "map"?}`（2.22.0 起多 `map`）：只在 PIE 里玩这张图（`FRequestPlaySessionParams::GlobalMapOverride`，和 GameTest 的 `pie.start` 一样），编辑器里打开的图不变、没保存的关卡改动不受影响；`map` 的写法和 `editor.map` 一样，回包多 `map`（包名）。
- `editor.map {"action": "status"}` → `{"package", "map", "pie", "dirty"?: [包名], "dirtyCount"?, "loading"?: true, "last"?: {"package", "previous", "ok"?, "ms"?, "error"?}}`（2.22.0 起，只在编辑器进程里有）：编辑器当前打开的地图（包名和短名）、PIE 在不在跑、改了没保存的关卡包（地图包、子关卡、World Partition 的外部 Actor 包，最多列 50 个），以及正在打开 / 上一次打开的结果（`last` 里有 `ok` 表示打开完了）。
- `editor.map {"action": "open", "map", "unsaved"?: "error"|"discard", "reload"?: false}` → `{"started", "package", "previous", "already"?: true, "discarded"?: [包名]}`：在编辑器里打开地图，和关卡编辑器「打开关卡」同一个入口（`FEditorFileUtils::LoadMap`），加载时压住模态对话框（`GIsRunningUnattendedScript`）。
  - `map` 可以是包名、对象路径、`.umap` 文件路径，也可以只写地图名或路径的结尾（`QuestMain_P_Free`、`QuestMain/QuestMain_P_Free`）：按资源注册表里的 World 找，同名的有几张时报出候选（最多 10 个）。
  - 已经是这张图时回 `already: true`、不重开（`reload: true` 强制重开）。PIE 运行中、正在打开别的地图时拒绝。
  - 有没保存的关卡包时：`unsaved` 默认 `error`，拒绝并列出包名（要留着由客户端先 `asset.save`）；`discard` 清掉脏标记后打开，回包 `discarded` 列出丢掉的包。
  - 回包只表示「下一帧开始打开」：加载会卡住编辑器主线程，期间的 HTTP 请求要等加载完才回。客户端接着调 `status`，`last.ok` 为 true 才算打开好；为 false 时 `last.error` 写了现在是哪张图。
  - 为什么不用控制台 `open`：编辑器世界里 `open` 立刻回成功，换没换图、有没有弹保存对话框都不知道。
- `screenshot`（编辑器世界，2.23.0 起）：`editor.map` 正在打开地图时直接报「编辑器正在加载地图」；没有活动视口（焦点在内容浏览器、细节面板）时用第一个关卡视口。
- `lua.exec {"code", "path"?, "timeoutMs"?, "depth"?: 2}`（`depth` 2.23.0 起）：返回值是表时 `results[i].fields` 列出前 50 项；`depth` ≥ 2 时不超过 20 项的子表在自己那一项里再带 `fields`（同样的结构），最多展开到第 `depth` 层（1–3，默认 2）。更大的子表只给项数 `n`，要看时用 `lua.inspect` 分页。

> 2.4.0 这一批的操作由几个并行的工作包分别加：每个包的新操作放各自的 `*Ops.cpp`（例如 `UELinkSelectionOps.cpp`、`AgentPanelLuaDevOps.cpp`），在这份文档里各写一段，操作表里各加一格，版本统一记在 UELink 的 CHANGELOG `v2.4.0-beta.1` 一节里。

2.0.0 新加的三个：

- `livecoding` → `{"started": true}`；已经在编译时 `{"started": false, "message": "already compiling"}`。结果用下面的 `livecoding.status` 查。
- `livecoding.status` → `{"available", "phase": "idle|compiling|finished", "result": "success|failed|noChanges|cancelled|unknown", "message", "durationMs", "output": [最近的编译输出]}`。不管是谁触发的编译都会记录。
- `tests.run {"filter": "Project.Foo", "id"?}` → `{"id"}`；`tests.status` → `{"running", "id", "filter", "passed", "failed", "current"?, "durationMs", "message"?, "tests": [{"name", "result", "durationMs", "errors"}]}`。同一时间只能跑一组。

### 作业（2.13.0 起）

要等很久的操作（编译蓝图、改名、删除、批量校验……）可以作为作业跑：马上拿到作业号，HTTP 不用一直挂着，进度走推送。客户端按 `GET /info` 的 `ops` 里有没有 `job.start` 判断支持（推送通道的 `features` 里也有 `jobs`）。

- `job.start {"op", "args"}`（一步）或 `{"steps": [{"op", "args"}], "title"?, "stopOnError"?: true}`（最多 500 步）→ `{"job": 作业号, "id", "title", "status": "queued", "total", "finished", "failed", "startedAt"}`。每一步就是一个已登记的同步操作，参数和返回字段和直接调时完全一样；异步操作（`trace.query`、`job.*`）不能放进作业。`stopOnError` 为 true 时某一步失败后面的不做（算 `skipped`）。
- 作业按先后排队，同一时间只跑一个；每帧做到 30 毫秒为止（一步本身超过时至少做一步）。
- `job.get {"id"}` → 摘要 + 结果：`status`（`queued` / `running` / `done` / `failed` / `cancelled`）、`total`、`finished`、`failed`、`current`?（正在做的操作）、`skipped`?、`startedAt`、`endedAt`?、`ms`?、`report`?。只有一步的作业把这一步的 `result`（操作的回包）/ `error` 提到顶层；多步的给 `steps: [{"op", "state": "ok|failed|skipped", "error"?, "ms"?, "result"?}]`，各步结果合计超过 12000 字时后面的只给状态并标 `truncated: true`。做完后全部结果（含每步参数）写进 `Saved/UELink/jobs/<作业号>.json`（`report`，留最近 50 份）。不给 `id` → `{"items": [摘要]}`，最近的在前，内存里留最近 20 个结束的作业。
- `job.wait {"id", "timeoutMs"?: 20000}`：异步操作，作业做完或等满 `timeoutMs`（最多 60000）才回，回包同 `job.get`；没做完时 `status` 还是 `running` / `queued`，客户端再调一次接着等，不用轮询 `job.get`。
- `job.stop {"id"}` → 摘要 + `stopped`：取消还没做的步骤（正在做的那一步做完才停），已经结束的回 `stopped: false`。
- 推送事件 `uelink.job`（订阅时 `topics` 里写它）：摘要字段 + `phase`：`queued`、`started`、`progress`（每帧做完一批还没完时）、`finished`。后台编辑器（commandlet）没有推送通道，客户端用 `job.wait` 等。

### 批量（2.14.0 起）

登记了能批量的操作，参数里给 `paths` 或 `items` 时不直接执行，而是展开成一个作业（上一节），同一组参数逐个作用到每个资源，马上回作业号。客户端按 `GET /info` 的 `batchOps`（能批量的操作名）判断。

- 能批量的操作：读 `bp.outline`、`bp.graph`、`bp.compile`、`asset.describe`、`asset.refs`、`asset.export`、`get`、`props`；写 `bp.edit`、`set`、`datatable.set`、`asset.duplicate`、`asset.rename`、`asset.delete`、`asset.undo`（撤回一整批：`items: [{path, tx}]`）。新操作要支持批量，在登记处加一行 `AddBatchOp(名字, FAgentPanelBatchInfo)`（编辑器的在 `AgentPanelBridgeEditor.cpp` 的 `RegisterEditorBatchOps`）。
- `<操作> {"paths": [路径...] | "items": [{"path", "vars"?, ...这一项自己的参数}], ...其余参数照常, "dryRun"?, "dryRunFirst"?: true, "onError"?: "rollback", "compile"?: "end"}`，或 `job.batch {"op", 同上}`。一次至少 1 个资源（2.14 最多 2000 个，2.15 起不限），同一路径不能出现两次。
  - 公共参数和这一项的参数合并（同名的以这一项为准），`path` 换成这一项的路径。
  - 字符串参数里的占位符逐个替换：`{name}`（资源名，`/Game/A/BP_X.BP_X` → `BP_X`）、`{path}`、`{dir}`（`/Game/A`）、`{vars.键}`（这一项 `vars` 里的值；整个字符串就是 `{vars.键}` 时原样换成那个值，数字、对象都行）。认不出的原样留着。
- 写操作（`bp.edit`、`asset.duplicate / rename / delete`）认 `dryRun` 的先整批演练（`dryRunFirst: false` 跳过）：有一个失败（含蓝图编译出错）整批不改，`batch.stopped = "dryRun"`。`dryRun: true` 只演练。
- 真改时有失败（含编译出错）：`onError: "rollback"`（默认）后面的不做，把这批已改成功的按倒序撤回：`bp.edit`、`datatable.set` 用结果里的 `tx` 调 `asset.undo`，`set` 用结果里的 `prev` 把值设回去；复制、改名、删除没有事务，撤不回，汇总里写明。`"keep"` 保留改成功的，接着做后面的。
- `bp.edit` 的 `compile: "end"`：每个蓝图改完不编译，整批改完再逐个 `bp.compile`（有父子关系的一批蓝图不会被反复编译）。
- 每步在作业里的阶段：`dryRun`、`apply`（写）/ `run`（读）、`compile`、`undo`。
- `job.get` / `job.wait` 对批量作业不列每一步，而是给 `batch`：`{op, items, ok?, failed?, skipped?, rolledBack?, dryRunOk?, pending?, stopped?, listed?, list: [{path, item, state, error?}]}`（`item` 是这个资源在 paths / items 里的下标）。各状态的个数只写不为 0 的；`list` 失败和回滚的排前面，放不下时只列前 `listed` 个。每步的完整参数和结果在 `report` 文件（`Saved/UELink/jobs/<作业号>.json`，`steps` 每项带 `item`、`phase`，另有不截断的 `batch`）。
- 作业整体的 `status`：有资源失败、演练没过或回滚过时是 `failed`，否则 `done`。`job.stop` 取消批量作业时不回滚已经做完的。
- 作业的 `steps`（`job.start`）上限 2.14 起是 2000 步（原来 500）。

`bp.edit` 一条操作作用到多个成员（2.14.0）：`set_variable`、`remove_variable`、`remove_function`、`set_component`、`remove_component` 的 `name` 可以给数组，每项或单个名字都可以写通配符（`*`、`?`，不分大小写）；两个组件操作另认 `componentClass`（组件类，含子类），`name` 不写时就是这个类的全部组件；`remove_node` 的 `node` 可以给 id 数组。展开后逐个执行，同在一个事务里，任一个失败整批取消（报错带出是哪个名字）；结果里这一步带 `count` 和 `each`（每个名字的结果）。一个都没匹配上算失败。`set_variable` 一次多个时不能带 `rename`。

`bp.edit` 的 `set_component`、`set_default` 2.14.0 起：结果带原值 `prev` 和新值 `value`（演练时也有，可以看改前改后）；`props: {属性路径: 值}` 一条改多个属性（展开成一个属性一条）；`where` 是这个属性期望的当前值（用 `props` 时写成 `{属性路径: 期望值}`），对不上的跳过（结果带 `skipped: true`、`why`），比较时字符串不分大小写、数字按数值。`set` 操作同样认 `where`。

`datatable.set` 2.14.0 起一次改多行：`row` 给数组或通配符（不写是全部行），`where: {列: 值}` 只改这些列现在的值都对得上的行；同一列改成同一个 `value`，整批一个事务（`tx`），认 `dryRun`；回 `count` 和 `rows: [{row, prev, value}]`（最多列 200 行）。一行都没挑中算失败。批量时同一张表可以在 `items` 里出现多次（每项改不同的行列）。

`actors` 2.14.0 起认 `class`（含子类）和 `tag`（Actor 标签）筛选，给了其中一个时 `prefix` 可以空；`limit` 最多 2000。

`bp.edit` 的增加类操作（`add_variable`、`add_function`、`add_component`（给了 `name` 时）、`add_interface`）2.14 起认 `ifMissing: true`：已经有同名的就跳过，这一步结果带 `skipped: true`，不算失败。同一组操作可以放心重跑。

`assets` 2.14 起多三个筛选，都只读资源注册表、不加载资源：`parent`（蓝图父类名，含间接继承，原生父类按 `NativeParentClass` 标签认，带不带 `_C` 都行）、`tags`（`{"键": "值"}`，值写 `"*"` 表示有这个标签就行）、`recursive`（默认 true，false 只看这一层目录）。

`assets` 2.15 起：`paths`（目录数组，给了就不看 `path`）；`name`（名字通配符，字符串或数组，`*`、`?`，不分大小写，对上任一个就留）、`exclude`（同样写法，对上任一个就去掉），模式里带 `/` 时比对包路径（如 `/Game/Enemies/Old/*`），否则比对资源名；`parent` 给 C++ 类时认整条继承链（蓝图最近的 C++ 父类是它的子类就算，以前只认最近那一层）。`limit` 写 0 时不截断（其他值照旧 1–2000）。批量操作（`paths` / `items`）2.15 起不再限 2000 个。旧版本不认这几个字段会忽略，客户端按版本判断或自己再筛一遍。

导入和编辑器函数库（2.16.0，只在编辑器进程里有，代码在 `UELinkImportCallOps.cpp`）：

- `asset.import {"files": [磁盘路径] | "file", "dest": "/Game/目录", "name"?, "replace"?: false, "dryRun"?}` → `{"items": [{"file", "objects": [资源路径], "error"?}], "imported", "dirtied"?: [连带弄脏的包]}`：用 `AssetTools.ImportAssetTasks` 导入（`bAutomated`，不弹选项对话框，不保存）。相对路径按项目目录算；资源名默认取文件名，`name` 只在导入一个文件时能用。先列计划：文件不存在、目标已有又没写 `replace` 时整批不导入，报错列出全部问题；`dryRun` 只回计划 `items: [{"file", "path", "exists"?}]`。不进撤销栈，没保存前用 `asset.revert` 丢掉。
- `asset.reimport {"path", "dryRun"?}` → `{"path", "reimported": true}`：`FReimportManager` 按资源记下的源文件重新导入（不问新文件、自动化）；不能重新导入的资源报错。能批量（`paths` / `items`），认演练，没有事务、回滚不了。
- `editor.functions {"class"? , "query"?, "limit"?: 60}` → `{"items": [{"class", "name", "params": [{"name", "type", "out"?}], "ret"?, "tooltip"?}], "total", "more"?}`：函数库（`UBlueprintFunctionLibrary` 子类，给了 `class` 时就是这个类）里蓝图能调的静态函数；`query` 按类名、函数名里的文字搜。
- `editor.call {"class", "function", "args"?, "path"?}` → 返回值和输出参数同 `call`，外加 `tx`：只调静态、蓝图能调（或标了 `AgentPanel`）的函数，在类默认对象上执行，包进编辑器事务（函数里 `Modify` 过的对象能用 `asset.undo` 撤回）。能批量：`paths` / `items` 每项一次，`args` 里的 `{path}` `{name}` 按资源替换，失败整批按 `tx` 撤回；没有演练。

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
- **读 Lua 字段**（2.8 起）：扩展要看对象 Lua 实例表上的数据时用 C++ 接口 `FAgentPanelLua::ReadFields(对象, 层数, 字段上限, 错误)`，返回 JSON 对象（标量原样、子表展开，函数和 userdata 跳过，超过上限带 `"__more": true`）。只用 `lua_next` 原样读，不触发元方法、不调 Lua 函数；对象没绑 Lua、没有 UnLua 时返回空并写原因。GameTest 1.4 录制列表条目时用它。
- **进程内调操作**（2.12 起）：扩展用 `FAgentPanelBridge::Get().CallOp(操作名, 参数, 返回, 错误)` 在游戏线程直接调一个已登记的同步操作，参数和返回和经 `/rpc` 调时一样；`HasOp(名字)` 看这个进程有没有这个操作（`lua.*` 只在编进了 Lua 支持时有）。GameTest 1.9 读 Lua 模块、调 Lua 函数、取 Lua 报错走它（`lua.get` / `lua.call` / `lua.errors`），不另写 Lua 读写。GameTest 2.1 的引用 `in: "lua"`、`class: "lua:模块"` 和 `gt.classes lua=true` 也走它（`lua.objects` / `lua.inspect` / `lua.modules`）。
- 扩展各自的操作列表见 `docs/asset-forge.md`、`docs/game-test.md`。
- **项目文档根**（AssetForge 1.6.1、GameTest 1.11.4 起）：AssetForge 配置根、GameTest 场景根没在项目设置里指定时，放在项目文档根下的 `AssetForge/`、`GameTest/`。文档根由 Agent 面板算（和面板「文档」角色同一个，P4 / SVN 项目是各分支共用的 `ProjectDoc/docs`，可能在别的分支目录下），面板 UE 插件写进 `Saved/AgentPanel/docs-root.txt`：一行绝对路径，不进版本管理（每台机器的路径可能不同）。扩展每次取根目录时读它；文件没有、不是绝对路径、目录不在时按旧规则（项目外层同级有 `ProjectDoc/docs` 就用它，否则项目里的 `docs`）。新文档根下还没有对应子目录、旧位置有时先用旧位置，面板会把旧内容复制过去（旧的不删）。

## 7. 给 EmmyLua 调试器提供 UE 对象属性

2.4.0-beta.4 起（装了 UnLua 时）。不走 `/rpc`，是进程内的 C 接口：EmmyLua 调试器（IntelliJ-EmmyLua 的原生 runtime）在断点暂停时展开 UObject，问 UELink 要属性，UELink 用 UE 反射回答。

- **怎么接上**：UELink 不链接 emmy，项目也不用改代码。每 2 秒看一眼进程里有没有 emmy runtime 的模块（`emmy_core` / `emmy_debugger_core`，或静态链进了主程序），有并且导出了 `Emmy_RegisterHostValueProviderV2` 就注册；模块卸载或重新注入后重新注册；新版 runtime 还导出 `Emmy_HasHostValueProviderV2`，核心卸载后在同一地址重新加载（模块句柄不变）时靠它发现注册丢了再注册。旧版 runtime 没有这个导出，UELink 什么也不做。
- **接口**：权威定义在 IntelliJ-EmmyLua 的 `docs/design/variable-view.md` 13.6，UELink 里的副本是 `UELinkCore/Private/UELinkEmmyHostValueAbi.h`，两边要同步改。`describe` 回一行（`valid`、`typeName`、`summary`、`hint`、`kind`、`expandable`），`listChildren` 分页回子项（`total`、`children[]`，每项 `name`、`type`、`value`、`typeName`、`kind`、`attrs`、`hint`、`length`、`expandable`、`childRef`），`releasePause` 在暂停结束时释放这次暂停发出的句柄。
- **显示什么**：UObject 下面按声明类分组（最派生的在前，组内按声明顺序，和细节面板一致），Actor 多一个「组件」分组，最后是「方法 (UFunction)」（带参数和返回类型）。结构体、`TArray` / `TSet` / `TMap` 分页展开（数组下标从 1 开始，和 UnLua 一致）。软引用只显示路径，不加载。
- **`hint` 是对象路径**：UObject 的 `describe` 和对象行的 `hint` 是 `GetPathName()` 原文（不截断），IDE 拿它调 `object.reveal`，改格式要和 IntelliJ-EmmyLua 一起改。
- **安全**：认 Lua 值是哪个 UObject 用 UnLua 的对象表反查，不读 userdata 的内存布局；拿到指针先在活着的对象里确认（`GUObjectArray` + `IsValid`），已销毁的对象只回 `<已失效>`，不读任何属性。只在游戏线程上回答（不在时回 `NEEDS_GAME_THREAD`），GC 进行中不回答。属性内存的句柄只在发出它的那次暂停内有效，每次用之前再确认所属对象还活着。
