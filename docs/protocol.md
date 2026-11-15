# UELink 协议与安装约定

给客户端（Agent 面板的 UE 插件、UEToVS）看：怎么找到 UE 进程、怎么调用、怎么订阅、怎么查更新和安装。**改协议先改这份文档。**

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
| 对象和属性 | `actors`、`objects`、`classes`、`components`、`props`、`get`、`set`、`call`、`functions`、`watch`、`object.refs`、`umg.tree`、`screenshot` |
| DataTable | `datatable`、`datatable.rows`、`datatable.row`、`datatable.set` |
| 资源 | `assets`、`asset.classes`、`asset.describe`、`asset.refs`、`asset.create`、`asset.duplicate`、`asset.rename`、`asset.delete`、`asset.save`、`asset.revert`、`asset.dirty`、`asset.undo`、`asset.validate`、`asset.export`、`asset.textify`、`asset.open` |
| 蓝图 | `bp.outline`、`bp.graph`、`bp.edit`、`bp.compile`、`bp.find`、`bp.node_search` |
| 日志和性能 | `log.tail`、`log.clear`、`perf.live`、`trace.query` |
| 编辑器 | `pie`、`livecoding`、`livecoding.status`、`tests.run`、`tests.status` |
| Lua（装了 UnLua 时） | `lua.status`、`lua.objects`、`lua.get`、`lua.set`、`lua.exec` |

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
   `files` = 只复制文件、版本管理留给用户（UEToVS 不问版本管理，自己装时写这个）。另一个客户端升级时沿用同样的 `mode`。这个文件不进发布的源码包。
8. 装完提示**需要完整编译一次**（Live Coding 不够）。

## 6. 扩展插件

UEKit 里的其他插件（AssetForge、GameTest）是 UELink 的扩展：不自己开端口，操作挂在 UELink 的 `/rpc` 上，共用令牌和名片。

- 扩展模块启动时 `FAgentPanelBridge::Get().RegisterExtension(名字, 版本)`，名片和 `GET /info` 里多一个字段：
  ```json
  "extensions": [{ "name": "AssetForge", "version": "1.0.0" }]
  ```
  这是**这个进程里实际加载了**的扩展和版本；装了新版还没重编时，和项目 `Plugins/` 里磁盘上的版本不同。
- 操作用 `AddExtensionOp("前缀.名字", 处理函数)` 登记：名字必须带前缀（AssetForge 用 `forge.`，GameTest 用 `gt.`），和已有操作重名时拒绝。客户端调用方式和 UELink 自己的操作完全一样。
- 长任务（批量导入、跑测试场景）马上返回作业号，客户端用 `forge.job` / `gt.job` 查进度。
- 扩展各自的操作列表见 `docs/asset-forge.md`、`docs/game-test.md`。
