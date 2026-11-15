# GameTest

用 JSON 场景跑游戏测试：按选择器找到控件并操作、注入输入、跨帧等待、断言、截图，也能把真人操作录成场景再回放。只支持 UE 5.8，依赖 UELink 2.2 起的扩展接口。界面在 Agent 面板 UE 页的「GameTest」页，Agent 通过面板的 `ue_gametest` 工具用同一套操作。

编辑器进程（PIE）和独立的游戏进程都能用：GameTest 是 Runtime 模块（Shipping 不加载），游戏进程里只要也装了 UELink，面板就能连上。

## 模块

| 模块 | 类型 | 内容 |
| --- | --- | --- |
| `GameTest` | Runtime（不进 Shipping） | 执行器、选择器、快照、通用动作、录制、`gt.*` 操作 |
| `GameTestCommonUI` | Runtime，可选 | `stack.push` / `stack.pop`，快照里层级容器和可激活控件的状态 |
| `GameTestGAS` | Runtime，可选 | `ability.input` |
| `GameTestEnhancedInput` | Runtime，可选 | `input.start` / `input.stop`，run 收尾时清掉注入 |
| `GameTestTests` | UncookedOnly，只在编辑器 | 自动化测试（`GameTest.*`）和测试探针控件 |

主模块不依赖 CommonUI、GameplayAbilities、EnhancedInput。可选模块启动时往动作登记表里加自己的动作；没加载时 `gt.actions` 的 `unavailable` 列出缺的动作和要开的插件，写了这些动作的场景解析时报「要启用哪个插件」。这三个插件在 `.uplugin` 里是可选依赖，它们是引擎自带的，启用 GameTest 时会一起打开。

## 场景文件

放在场景根目录（默认 `Docs/GameTest`，项目设置 → 插件 → Game Test），子目录随意，录制生成的在 `recorded/`。

```json
{
  "kind": "gametest-scenario",
  "schemaVersion": 1,
  "name": "主菜单冒烟",
  "_note": "可选说明",
  "revision": 3,
  "tags": ["smoke"],
  "stepDelayMs": 0,
  "steps": [
    { "action": "pie.start", "map": "/Game/Maps/MainMenu" },
    { "action": "wait", "until": "widget", "target": "WBP_MainMenu:Root/BtnStart" },
    { "action": "click", "target": "WBP_MainMenu:Root/BtnStart", "note": "点开始" },
    { "action": "assert", "target": "WBP_HUD:Root", "visible": true }
  ]
}
```

- `stepDelayMs`（可选）覆盖项目设置里的步间隔（默认 1000ms，方便看清每一步；录制的场景写 0）。
- 每一步的公共字段：`action`（必填）、`target`、`note`、`timeoutMs`、`onFail`（`stop` 默认 / `continue`）。动作不认识的字段直接报错，不静默忽略。
- `target` 写法：`屏名:路径`（屏名是 UUserWidget 的类名或控件名）、`路径`（所有屏里找，多个命中算歧义）、`@句柄`（之前 `find` 捕获的）；路径段可以是 `名字`、`#类名`、`#类名#序号`、`text=子串`、`*`、`..`。

## 动作

`gt.actions` 返回实际可用的清单（含每个动作接受的字段），下表是全部动作：

| 动作 | 字段 | 模块 | 说明 |
| --- | --- | --- | --- |
| `ping` | — | 主模块 | 通道连通性检查：回显世界、PIE 与编辑器状态，不改变任何东西 |
| `mark` | `text` | 主模块 | 只把一句说明写进报告，不做任何操作（给 AI 写自己的判断） |
| `editor.ensure` | `assetsLoaded`, `noModal` | 主模块（仅编辑器） | 编辑器就绪门禁：等资产加载完、没有模态框，之后才允许动编辑器 |
| `pie.start` | `map`, `width`, `height`, `netMode`, `players` | 主模块（仅编辑器） | 启动 PIE（默认新窗口 + 固定分辨率）；netMode=listenServer 可开监听服务器会话（players 为玩家窗口总数，至少 1），并等到全部世界跑起来 |
| `pie.stop` | — | 主模块（仅编辑器） | 退出 PIE。请求只是入队，用 wait until=pieStopped 等它真的结束 |
| `level.load` | `map` | 主模块（仅编辑器） | 在编辑器里加载关卡（PIE 之前切换起始地图用它） |
| `widget.add` | `class`, `zOrder` | 主模块 | 按类路径创建 UUserWidget 并加进视口——测试靶子不必依赖真实游戏流程 |
| `widget.remove` | — | 主模块（要 target） | 把目标控件从视口移除并丢弃 |
| `find` | `into` | 主模块（要 target） | 把选择器解析成句柄（捕获此刻的控件），之后用 @句柄 引用它；into 缺省用路径末段当句柄名 |
| `snapshot` | `geometry`, `props` | 主模块 | 控件树快照：AI 的主要观察手段；给 target 就只取那棵子树，props=true 带出类自己的属性 |
| `wait` | `until`, `seconds`, `target`, `actor`, `tag`, `point`, `radius` | 主模块 | 跨帧等待谓词成立：pieReady/pieStopped/widget/widgetGone/seconds/actorNear（actorNear 用 actor 或 tag + point + radius） |
| `click` | — | 主模块（要 target） | 代码级点击：按钮走 Slate ExecuteOnClick，复选走 Slate 状态回调，菜单锚走开合 |
| `type` | `text`, `commit` | 主模块（要 target） | 输入框写文本；commit=true 才广播 OnTextCommitted（游戏逻辑多半绑在提交上） |
| `select` | `index`, `property`, `equals`, `add` | 主模块（要 target） | 列表族（ListView / TileView / TreeView）选中条目：按索引、或按数据项属性匹配（属性名大小写不敏感）；TreeView 的匹配范围是根条目 |
| `scroll` | `mode`, `value`, `index`, `widget` | 主模块（要 target） | 滚动：ScrollBox 用 offset/end/start/widget，列表族用 index |
| `set` | `value`, `checked`, `option`, `index`, `expanded`, `property`, `equals`, `open`, `key` | 主模块（要 target） | 通用赋值：滑条数值/复选/下拉选项/切换器页号/展开态（含 TreeView 条目展开）/菜单开合/按键；给了该控件不认的字段即硬错误 |
| `screenshot` | `name`, `annotate` | 主模块 | 截游戏窗口（含 UMG）落盘，返回相对路径与像素尺寸；annotate 画编号框并给出图例 |
| `console` | `command`, `expectContains` | 主模块 | 执行一条控制台命令（等价于在控制台里敲它）；给 expectContains 时输出必须包含该子串，否则本步失败（用来让世界状态可被场景判定） |
| `input.cursor` | `x`, `y` | 主模块 | 把光标移到视口内归一化坐标（0–1），用于驱动直读鼠标位置的游戏逻辑（如瞄准）；移动的是真实光标 |
| `key` | `key`, `phase` | 主模块 | 按键：键盘键或鼠标键（UE 的键名，如 W、SpaceBar、LeftMouseButton），走 Slate 输入入口，和真实按键同一条路；phase=tap（默认，按下隔一帧松开）/ press / release；鼠标键在光标当前位置按（先 input.cursor）。录制生成的场景用它回放 |
| `actor.get` | `class`, `tag`, `client`, `locallyControlled` | 主模块 | 在游戏世界里按 class 或 tag 找 actor（唯一命中），回报对象路径、位置、朝向与速度；多客户端会话可用 client（\"server\" 或 0 起序号）寻址世界、locallyControlled 区分本端与远端 |
| `net.compare` | `class`, `tag`, `from`, `to`, `tolerance` | 主模块 | 跨端一致性断言：同一玩家对应的 actor 在两端世界的位姿偏差不超过 tolerance（厘米）；from/to 各可给 client 与 locallyControlled（缺省 from=server、to=client0）；给 timeoutMs 则跨帧等到一致 |
| `assert` | `exists`, `visible`, `enabled`, `text`, `textContains`, `count`, `selected`, `selectedProperty`, `stateKey`, `equals` | 主模块（要 target） | 断言目标状态（stateKey+equals 可断言快照 state 里的任意字段）；失败信息里带实际值，不做静默通过 |
| `probe` | `pointX`, `pointY`, `expect` | 主模块（要 target） | 只读命中测试：这个点上最终会命中谁（返回命中路径与最顶层控件；被遮挡时失败并给出遮挡者） |
| `stack.push` | `class` | GameTestCommonUI（要 target） | 把可激活控件类推进 CommonUI 层级容器，并等它成为当前界面（Queue 类容器只等进入列表） |
| `stack.pop` | — | GameTestCommonUI（要 target） | 弹出 CommonUI 层级容器的当前界面，并等它回落到上一个（空栈弹出是失败） |
| `ability.input` | `tag`, `phase`, `class` | GameTestGAS | 对游戏世界里的目标发出一次技能输入标签：默认本地玩家 Pawn，可用 class 指定（沿继承链匹配、必须唯一）；缺省按下，phase=release 发松开 |
| `input.start` | `asset`, `value`, `client` | GameTestEnhancedInput | 按输入资产开始持续注入（每 tick 重发、走完整 modifier/trigger 链，与真实按键同一条路）；重复注入同一资产以最后一次的值为准，run 结束或取消时自动清空。字段名是 asset 不是 action——action 是公共字段里的动作名；多客户端会话用 client 选注入到哪一端（缺省 server） |
| `input.stop` | `asset`, `client` | GameTestEnhancedInput | 停止某个输入资产的持续注入；对没在注入的资产会失败并说明（字段名是 asset，理由同 input.start；client 须与注入时一致） |

## 录制回放

1. 面板上点「录制」（`gt.record.start`，可给 `name`）。编辑器里 PIE 正在跑时，场景开头自动加一条 `pie.start`（同一张地图），回放能自己拉起游戏。
2. 在游戏窗口里正常操作。只录发给游戏窗口的输入，编辑器其他面板上的操作不录。
3. 需要检查的地方插检查点（`gt.record.mark`，可带 `screenshot: true`）。
4. 停止（`gt.record.stop`），写成 `recorded/<名字>.json`，重名加序号。

录成什么：

| 操作 | 录成 |
| --- | --- |
| 点按钮、复选框、菜单锚 | `click`，目标是选择器（录的时候用同一套解析核对过，指回的是同一个控件） |
| 改输入框、下拉框、滑条、微调框 | 离开它时录一条 `type`（`commit: true`）或 `set`，只录最终值 |
| 点在游戏画面上 | `input.cursor`（视口归一化坐标）+ `key LeftMouseButton` |
| 键盘、手柄、鼠标键 | `key`：短按（< 0.25 秒）录成 `tap`，长按录成 `press` … `release` |
| 两次操作隔了 0.15 秒以上 | `wait until=seconds` |

实现：在 Slate 上挂一个 `IInputProcessor`（引擎公开的输入预处理扩展点），只看不吃事件；回放时 `key` 走 `FSlateApplication::ProcessKeyDownEvent` 等入口，和真实按键同一条路（Slate → 视口 → PlayerController → EnhancedInput），所以不需要知道项目的输入资产。回放发出的合成输入不会被录进去。run 收尾时按下没松开的键全部松开。不改引擎、不改项目代码。

一次最多录 2000 步。录完的场景是普通场景文件，可以在面板里改：把 `mark` 换成 `assert`、删掉多余的 `wait`。

## 操作（UELink `/rpc`）

全部在游戏线程执行。返回默认不超过 4000 字，读结构的（快照、报告、动作清单）不超过 12000 字；超了整份写进 `Saved/GameTest/responses/`，只回 `file`（相对项目根）和几项摘要。

| 操作 | 参数 | 返回 |
| --- | --- | --- |
| `gt.health` | — | `version`、`process`（editor / game）、`busy`、`runId`、`editor`（PIE 状态）、`worldPresent`、`playable`、`map`、`scenarioRoot`、`outputRoot`、`record`；装了 EnhancedInput 模块时有 `activeInjections` |
| `gt.actions` | — | `common`、`actions: [{name, params, targetRequired, editorOnly, summary, module}]`、`unavailable: [{name, module, plugin}]` |
| `gt.scenarios` | `query`、`offset`、`limit`（默认 200） | `scenarios: [{name, relativePath, _note, steps, revision, tags, parseError?}]`、`total` |
| `gt.run` | `scenario`（相对场景根）或 `steps` + `name`、`stepDelayMs` | `runId`、`steps`、`finished`；run 在后台跑 |
| `gt.step` | `step: {action, ...}` | 这一步的结果：`ok`、`status`、`message`、`payload` |
| `gt.job` | `steps`（只回最后几步，默认 50） | 当前 / 最近一次 run：`status`、`busy`、`total`（已完成）、`planned`、`failed`、`steps` |
| `gt.runs` | — | 本次会话最近的 run 摘要（含 `outputDir`） |
| `gt.report` | `runId` | 完整报告（每步的状态、消息、payload、耗时） |
| `gt.stop` | — | 取消当前 run |
| `gt.record.start` | `name`、`startPie`（默认 true） | 录制状态 |
| `gt.record.mark` | `text`、`screenshot` | 录制状态 |
| `gt.record.stop` | `save`（默认 true） | `path`、`absolutePath`、`steps`、`seconds`、`preview`（前 20 步） |
| `gt.record.status` | `last`（默认 10） | `recording`、`name`、`steps`、`seconds`、`last` |

同一时间只跑一个 run。每次 run 的报告和截图在 `Saved/GameTest/<名字>-<时间>/`（`report.json`、`*.png`）。

### 推送事件（UELink 推送通道，订阅主题）

| 主题 | 什么时候 | 字段 |
| --- | --- | --- |
| `gt.job` | run 开始、每完成一步、结束 | 和 `gt.job` 一样但不带 `steps`，多 `lastStep` 和 `phase`（`progress` / `finished`） |
| `gt.record` | 录制开始、每录一步、停止 | 录制状态 + `phase`（`started` / `step` / `stopped`），`last` 只有最新一步 |

## 其他入口

- **控制台命令**：`GameTest.Step <动作> [目标] [参数JSON]`、`GameTest.Health`、`GameTest.Actions`。编辑器控制台、PIE 控制台、`-ExecCmds=` 都能用。
- **视口高亮**：执行器碰过的控件在游戏窗口里框出来（成功绿、失败红），停留 2.5 秒。

## 限制

- 云端编不了 C++：每一版都要在本地 UE 5.8 完整编译验证，没验证过的版本号带 `-beta.N`。
- `input.cursor` 和录制回放里点在游戏画面上的操作会移动真实光标，回放时别动鼠标。
- 录制只认 UMG：Slate 原生控件（编辑器工具窗口之类）上的操作录成坐标点击。
