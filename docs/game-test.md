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

放在场景根目录（默认跟着项目的文档根：项目外层同级有 `ProjectDoc/docs` 就是 `ProjectDoc/docs/GameTest`，否则是项目里的 `docs/GameTest`；项目设置 → 插件 → Game Test 里可以指定），子目录随意，录制生成的在 `recorded/`。

```json
{
  "kind": "gametest-scenario",
  "schemaVersion": 2,
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
- `schemaVersion`：当前是 2（1.1.0 起）。2 里 `click` / `type` 默认走真实输入（真实指针、真实按键）；1 的文件照常能跑，见下面「从旧场景迁移」。写比插件支持的更高的版本会被明确拒绝。
- 每一步的公共字段：`action`（必填）、`target`、`note`、`timeoutMs`、`onFail`（`stop` 默认 / `continue`）、`hint`。动作不认识的字段直接报错，不静默忽略。
- `hint`：录制写的线索（`asset` WBP 资源路径、`class`、`text`、`rect` 视口归一化 `[x, y, w, h]`；录的时候核对没过还会多一个 `ambiguous` 写原因）。只给人看、解析失败时找候选用，**执行时不读，回放不会拿它换目标**。手写场景不用写。

### 选择器（`target`）

| 写法 | 意思 |
| --- | --- |
| `屏名:路径` | 先按屏名挑屏，再走路径。屏名可以写 WBP 类名（去掉 `_C`）、控件名，或资源路径（`/Game/UI/Menu/WBP_MainMenu`），忽略大小写；`*` 表示任意屏 |
| `路径` | 不写屏名：在所有屏里找，多个命中算歧义 |
| `@句柄` | 之前 `find` 捕获的控件，不再重新解析 |
| `A>B` | `A` 落在一个嵌套的 WBP 实例（或列表条目）上，`B` 从**这个实例自己的**控件树根开始走。同一个通用按钮 WBP 用了十次也不会歧义，因为前面的父链已经唯一 |
| `A>>Slate段` | 在 `A` 的 Slate 子树（包括它弹出的菜单窗口）里找纯 Slate 控件，如下拉框的选项、菜单行 |
| `>>Slate段` | 不挂在 UMG 上：在游戏窗口和它弹出的子窗口里找 |

路径段（用 `/` 分隔）：

| 段 | 意思 |
| --- | --- |
| `名字` | 设计器里起的控件名 |
| `#类名`、`#类名#序号` | 按类找；有多个同类时用序号 |
| `text=子串` | 按显示的文字找（受当前语言影响，能用名字就别用它） |
| `*` / `..` / `.` | 任一子节点 / 任一后代 / 就是这个屏本身（`WBP_Hud:.` 指整个屏） |
| `列表名[序号]` | ListView 族的第几个条目（从 0 数，只认已经生成出来的条目控件） |
| `列表名[属性=值]` | 数据项属性等于这个值的条目（比序号稳，列表顺序变了也不会点错） |

Slate 段写成 `类型[text=子串][序号]`（后两个可选），多段用 `/` 分隔，每段在上一段的任意深度里找。只认 Slate 类型名和文字，不认指针，跨次运行稳定。

例子：

| 场景 | 选择器 |
| --- | --- |
| 主菜单里的开始按钮 | `WBP_MainMenu:Root/Menu/BtnStart` |
| 开始按钮本身是嵌套的 WBP，点它里面真正接住点击的控件 | `WBP_MainMenu:Root/Menu/BtnStart>Root/HitArea` |
| 背包第 3 格的「使用」 | `WBP_Bag:Root/ItemList[2]>BtnUse` |
| 背包里 ItemId=1001 那格的「使用」 | `WBP_Bag:Root/ItemList[ItemId=1001]>BtnUse` |
| 设置页画质下拉里的「高」 | `WBP_Settings:Root/QualityCombo>>STableRow[text=高]` |
| 按资源路径指定屏 | `/Game/UI/Menu/WBP_MainMenu:Root/Menu/BtnStart` |

解析失败一律报错并给出候选，不做「取第一个匹配」。写不准时用 `gt.inspect`（见「操作」）看光标下的控件该怎么写。

## 动作

`gt.actions` 返回实际可用的清单（含每个动作接受的字段），下表是全部动作：

| 动作 | 字段 | 模块 | 说明 |
| --- | --- | --- | --- |
| `ping` | — | 主模块 | 通道连通性检查：回显世界、PIE 与编辑器状态，不改变任何东西 |
| `mark` | `text` | 主模块 | 只把一句说明写进报告，不做任何操作（给 AI 写自己的判断） |
| `editor.ensure` | `assetsLoaded`, `noModal` | 主模块（仅编辑器） | 编辑器就绪门禁：等资产加载完、没有模态框，之后才允许动编辑器 |
| `pie.start` | `map`, `width`, `height`, `netMode`, `players` | 主模块（仅编辑器） | 启动 PIE（默认新窗口 + 固定分辨率）；netMode=listenServer 可开监听服务器会话（players 为玩家窗口总数，至少 1），并等到全部世界跑起来；PIE 已经在跑同一张地图（单人）时沿用当前会话，不报错 |
| `pie.stop` | — | 主模块（仅编辑器） | 退出 PIE。请求只是入队，用 wait until=pieStopped 等它真的结束 |
| `level.load` | `map` | 主模块（仅编辑器） | 在编辑器里加载关卡（PIE 之前切换起始地图用它） |
| `widget.add` | `class`, `zOrder` | 主模块 | 按类路径创建 UUserWidget 并加进视口——测试靶子不必依赖真实游戏流程 |
| `widget.remove` | — | 主模块（要 target） | 把目标控件从视口移除并丢弃 |
| `find` | `into` | 主模块（要 target） | 把选择器解析成句柄（捕获此刻的控件），之后用 @句柄 引用它；into 缺省用路径末段当句柄名 |
| `snapshot` | `geometry`, `props` | 主模块 | 控件树快照：AI 的主要观察手段；给 target 就只取那棵子树，props=true 带出类自己的属性 |
| `wait` | `until`, `seconds`, `target`, `actor`, `tag`, `point`, `radius` | 主模块 | 跨帧等待谓词成立：pieReady/pieStopped/widget/widgetGone/seconds/actorNear（actorNear 用 actor 或 tag + point + radius） |
| `click` | `via`, `at`, `button` | 主模块（要 target） | 点击控件：默认真实指针——先等控件可操作（见下面「UI 动作怎么执行」），再把光标移过去按下松开，不认控件类（按钮、自定义 WBP、Lua 绑的事件都一样）；点完核对接住按下的是不是目标，不是就失败。`at=[x,y]` 控件内位置（0–1，默认中心），`button` 默认 `LeftMouseButton`（也可 `RightMouseButton` / `MiddleMouseButton`）；`via` 取 `pointer`（默认）或 `code`，`code` 走旧的代码级点击（按钮 OnClicked、复选状态回调、菜单锚开合，不受遮挡影响） |
| `drag` | `at`, `toTarget`, `toAt`, `by`, `button` | 主模块（要 target） | 拖动：从控件内 `at` 处按下，分几帧移到 `toTarget` 控件的 `toAt` 处（或按视口归一化位移 `by=[dx,dy]`，可以是负数）再松开；`toTarget` 和 `by` 二选一。起点和 click 一样先等可操作 |
| `wheel` | `delta`, `at` | 主模块 | 滚轮：给 target 时先等控件可操作、把光标移到控件上再滚；不给时在光标当前位置滚。`delta` 必填，正数向上、负数向下 |
| `type` | `text`, `commit`, `via`, `append` | 主模块（要 target） | 在输入框里打字：默认真实按键——把键盘焦点给控件，先全选（`append=true` 时不全选、接着原文字打），逐字发字符，`commit=true` 时再按回车；`via` 取 `keys`（默认）或 `code`，`code` 直接写文本并广播 OnTextChanged / OnTextCommitted（commit=true 才广播提交） |
| `select` | `index`, `property`, `equals`, `add` | 主模块（要 target） | 列表族（ListView / TileView / TreeView）选中条目：按索引、或按数据项属性匹配（属性名大小写不敏感）；TreeView 的匹配范围是根条目 |
| `scroll` | `mode`, `value`, `index`, `widget` | 主模块（要 target） | 滚动：ScrollBox 用 offset/end/start/widget，列表族用 index |
| `set` | `value`, `checked`, `option`, `index`, `expanded`, `property`, `equals`, `open`, `key` | 主模块（要 target） | 通用赋值：滑条数值/复选/下拉选项/切换器页号/展开态（含 TreeView 条目展开）/菜单开合/按键；给了该控件不认的字段即硬错误 |
| `screenshot` | `name`, `annotate` | 主模块 | 截游戏窗口（含 UMG）落盘，返回相对路径与像素尺寸；annotate 画编号框并给出图例 |
| `console` | `command`, `expectContains` | 主模块 | 执行一条控制台命令（等价于在控制台里敲它）；给 expectContains 时输出必须包含该子串，否则本步失败（用来让世界状态可被场景判定） |
| `input.cursor` | `x`, `y` | 主模块 | 把光标移到视口内归一化坐标（0–1），用于驱动直读鼠标位置的游戏逻辑（如瞄准）；移动的是真实光标 |
| `key` | `key`, `phase` | 主模块 | 按键：键盘键或鼠标键（UE 的键名，如 W、SpaceBar、LeftMouseButton），走 Slate 输入入口，和真实按键同一条路；phase=tap（默认，按下隔一帧松开）/ press / release；鼠标键在光标当前位置按（先 input.cursor）；给 `target` 时键盘事件发给这个控件（输入框里的回车、退格、方向键），否则发给游戏视口 |
| `actor.get` | `class`, `tag`, `client`, `locallyControlled` | 主模块 | 在游戏世界里按 class 或 tag 找 actor（唯一命中），回报对象路径、位置、朝向与速度；多客户端会话可用 client（\"server\" 或 0 起序号）寻址世界、locallyControlled 区分本端与远端 |
| `net.compare` | `class`, `tag`, `from`, `to`, `tolerance` | 主模块 | 跨端一致性断言：同一玩家对应的 actor 在两端世界的位姿偏差不超过 tolerance（厘米）；from/to 各可给 client 与 locallyControlled（缺省 from=server、to=client0）；给 timeoutMs 则跨帧等到一致 |
| `assert` | `exists`, `visible`, `enabled`, `text`, `textContains`, `count`, `selected`, `selectedProperty`, `stateKey`, `equals` | 主模块（要 target） | 断言目标状态（stateKey+equals 可断言快照 state 里的任意字段）；失败信息里带实际值，不做静默通过 |
| `probe` | `pointX`, `pointY`, `expect` | 主模块（要 target） | 只读命中测试：这个点上最终会命中谁（返回命中路径与最顶层控件；被遮挡时失败并给出遮挡者） |
| `stack.push` | `class` | GameTestCommonUI（要 target） | 把可激活控件类推进 CommonUI 层级容器，并等它成为当前界面（Queue 类容器只等进入列表） |
| `stack.pop` | — | GameTestCommonUI（要 target） | 弹出 CommonUI 层级容器的当前界面，并等它回落到上一个（空栈弹出是失败） |
| `ability.input` | `tag`, `phase`, `class` | GameTestGAS | 对游戏世界里的目标发出一次技能输入标签：默认本地玩家 Pawn，可用 class 指定（沿继承链匹配、必须唯一）；缺省按下，phase=release 发松开 |
| `input.start` | `asset`, `value`, `client` | GameTestEnhancedInput | 按输入资产开始持续注入（每 tick 重发、走完整 modifier/trigger 链，与真实按键同一条路）；重复注入同一资产以最后一次的值为准，run 结束或取消时自动清空。字段名是 asset 不是 action——action 是公共字段里的动作名；多客户端会话用 client 选注入到哪一端（缺省 server） |
| `input.stop` | `asset`, `client` | GameTestEnhancedInput | 停止某个输入资产的持续注入；对没在注入的资产会失败并说明（字段名是 asset，理由同 input.start；client 须与注入时一致） |

`select` / `scroll` / `set` 和 `via: "code"` 的 `click` / `type` 是「直接改值」的工具，给手写场景和 Agent 做准备工作用（例如点屏幕外或被挡住的控件）；录制器不写它们。

### UI 动作怎么执行（自动等待和真实指针）

`click`、`drag`、`wheel`（给了 target）、`type`、`key`（给了 target）动手之前，在超时内每帧检查目标，全部满足才动手：

| 检查 | 不满足时报什么 |
| --- | --- |
| 解析到：选择器唯一命中 | 「没找到」+ 候选 |
| 可见：自己和所有祖先都不是 Collapsed / Hidden | 「找到了…，但不可见（某某是 Collapsed）」 |
| 启用：自己和所有祖先都可用 | 「找到了…，但禁用中」 |
| 尺寸 > 0：已经布局 | 「尺寸为 0（还没布局）」 |
| 稳定：连续 2 帧位置不变（入场动画、滑入结束） | 「还在动（位置没稳定）」 |
| 没被挡：在点击点做命中测试，命中的是目标 | 「点击点被某某挡住」 |

超时取步骤的 `timeoutMs`，没写时用项目设置的「UI 等待超时」（`UiWaitTimeoutMs`，默认 10000ms；比单步默认超时 `DefaultStepTimeoutMs` 短，界面一般几秒内就出来）。所以场景里不用在每个 UI 操作前插 `wait`；显式的 `wait until=widget / widgetGone` 照样保留，给「等某个界面出现但不操作它」用。

动手：真实指针 `click` 把光标移到控件内 `at` 的位置，按下、隔一帧松开，和玩家点的效果一样，控件自己处理。按下后核对这次按下的处理者是目标本身或它里面的控件（点在按钮里的文字上，接住的是按钮也算），不是就报「点了 X，但接住按下的是 Y」，不算通过；没人接住按下时（有的控件只在松开时响应）不算失败，结果里 `handled: false`。这个进程拿不到处理者时（见限制）跳过核对，结果里写明「核对不了」。

## 录制回放

1. 面板上点「录制」（`gt.record.start`，可给 `name`、`keepPace`）。编辑器里 PIE 正在跑时，场景开头自动加一条 `pie.start`（同一张地图），回放能自己拉起游戏。
2. 在游戏窗口里正常操作。只录发给游戏窗口（和它弹出的子窗口，如下拉菜单）的输入，编辑器其他面板上的操作不录。
3. 需要检查的地方：按住 **Ctrl+Alt 点一个控件**，录一条断言（见下表）；或插检查点（`gt.record.mark`，可带 `screenshot: true`），之后在面板里改成 `assert`。
4. 停止（`gt.record.stop`），写成 `recorded/<名字>.json`（`schemaVersion: 2`），重名加序号。

**怎么认出点的是哪个控件**（1.1.0 起）：不看控件是什么类，看**谁接住了这次输入**。Slate 自己会告诉我们哪个控件处理了这次按下（引擎的 Slate 调试事件，Widget Reflector 看事件用的同一条通道），再把它反查成 UMG 控件、写成选择器。按钮、复选框、在蓝图或 Lua 里重写了 OnMouseButtonDown 的 WBP、UImage 绑的事件都是同一条规则，所以以前「项目的按钮不是 UButton 就只能录成坐标」的问题没有了。

录成什么：

| 操作 | 录成 |
| --- | --- |
| 按下和松开都在同一个 UI 控件上 | `click target=…`，附 `hint`；点在控件中心以外时带 `at`，不是左键时带 `button` |
| 在 UI 控件上按住挪开了（超过 8 像素） | `drag`：起点控件 + `at`，终点落在控件上写 `toTarget` + `toAt`，否则写 `by` |
| 接住点击的是纯 Slate 控件（下拉项、菜单行） | `click target=最近的 UMG 控件>>Slate段`，没有 UMG 祖先时 `>>Slate段` |
| 滚轮被 UI 控件接住 | `wheel target=… delta=…`，对同一个控件连着滚合成一条 |
| 键盘焦点在输入框里打字 | `type target=… text=… append: true`，连续字符合成一条 |
| 被 UI 控件接住的功能键（回车、退格、方向键） | `key key=… target=…`（先把打到一半的字落成一条 `type`） |
| 点在游戏画面上（游戏视口接住、或没人接） | 照旧 `input.cursor`（视口归一化坐标）+ `key LeftMouseButton` |
| 发给游戏的键盘、手柄、鼠标键 | `key`：短按（< 0.25 秒）录成 `tap`，长按录成 `press` … `release` |
| 按住 Ctrl+Alt 点一个控件 | 这次点击被录制器吃掉、不传给游戏，录成 `assert target=… visible: true`；文字类控件顺带断言此刻的 `text` |
| 两次游戏操作隔了 0.15 秒以上 | `wait until=seconds` |

- **UI 操作前不录 `wait`**：回放的 `click` / `type` 等自己会等控件可操作，录下来的秒数只会让慢机器失败、快机器白等。游戏画面上的操作前照旧按间隔补 `wait`（游戏逻辑没有「可操作」的信号可等）。想原样复现手速时开始录制给 `keepPace: true`，所有间隔都录。
- **选择器怎么写出来**：路径用设计器里起的控件名；运行时生成的名字（`WBP_Item_C_12` 这类）换成 `#类名#同类序号`；嵌套的 WBP 用 `>` 进去；列表条目写成 `列表[属性=值]`（能找到唯一的 Id 类属性时）或 `列表[序号]`。写完用回放同一套解析当场核对，必须唯一、指回同一个控件；核对没过时 `hint` 里多一个 `ambiguous` 写原因，回放前要人看一眼。
- 录出来的步骤不对（点的不是想要的控件、选择器有歧义）时，把光标停在那个控件上调 `gt.inspect`，看光标下的命中路径、谁接住了上一次按下、选择器该怎么写。

实现：在 Slate 上挂一个 `IInputProcessor`（引擎公开的输入预处理扩展点）拿原始事件，只看不吃（拾取断言那一下除外）；处理者来自 `FSlateDebugging` 的输入事件，反查 UMG 用 Widget Reflector 同一份元数据。回放时 `key` 走 `FSlateApplication::ProcessKeyDownEvent` 等入口，和真实按键同一条路（Slate → 视口 → PlayerController → EnhancedInput），所以不需要知道项目的输入资产。回放发出的合成输入不会被录进去。run 收尾时按下没松开的键全部松开。不改引擎、不改项目代码。

一次最多录 2000 步。录完的场景是普通场景文件，可以在面板里改：把 `mark` 换成 `assert`、删掉多余的 `wait`。

## 操作（UELink `/rpc`）

全部在游戏线程执行。返回默认不超过 4000 字，读结构的（快照、报告、动作清单）不超过 12000 字；超了整份写进 `Saved/GameTest/responses/`，只回 `file`（相对项目根）和几项摘要。

| 操作 | 参数 | 返回 |
| --- | --- | --- |
| `gt.health` | — | `version`、`process`（editor / game）、`busy`、`runId`、`editor`（PIE 状态）、`worldPresent`、`playable`、`map`、`scenarioRoot`、`outputRoot`、`record`；装了 EnhancedInput 模块时有 `activeInjections` |
| `gt.actions` | — | `common`、`actions: [{name, params, targetRequired, editorOnly, summary, module}]`、`unavailable: [{name, module, plugin}]` |
| `gt.scenarios` | `query`、`offset`、`limit`（默认 200） | `scenarios: [{name, relativePath, _note, steps, revision, tags, parseError?}]`、`total` |
| `gt.run` | `scenario`（相对场景根）或 `steps` + `name`、`stepDelayMs`；批量：`tag`（场景的 `tags` 里有它的全部场景）或 `scenarios`（路径数组），可带 `name` | 单个：`runId`、`steps`、`scenario`、`finished`；批量：`batchId`、`scenarios`（个数）。都在后台跑 |
| `gt.step` | `step: {action, ...}` | 这一步的结果：`ok`、`status`、`message`、`payload` |
| `gt.job` | `steps`（只回最后几步，默认 50） | 当前 / 最近一次 run：`status`、`busy`、`total`（已完成）、`planned`、`failed`、`steps`；批量运行时多 `batch`（见下） |
| `gt.runs` | — | 本次会话最近的 run 摘要（含 `outputDir`） |
| `gt.report` | `runId` | 完整报告（每步的状态、消息、payload、耗时；跑的是场景文件时有 `scenario`；失败的步骤 `payload.evidence` 是失败证据的路径） |
| `gt.stop` | — | 取消当前 run；批量运行中时整批取消（没轮到的标 `skipped`） |
| `gt.history` | `scenario`（可省） | 按场景文件的运行历史（跨编辑器会话，每个场景最近 10 次）。给 `scenario`：`runs: [{runId, succeeded, startedAtUtc, durationMs, total, failed, failedStep?, message?, outputDir, revision?}]`，最新在前；不给：`scenarios: {路径: "pfpp…"}`（最新在前，`p` 通过、`f` 失败），列表上画历史条用 |
| `gt.resolve` | `targets`（选择器数组）和 / 或 `scenario`（核对它每一步的 `target`、`toTarget`） | 只解析不执行：`results: [{target, step?, field?, found, path?, class?, text?, visible?, enabled?, error?}]`（`error` 里是候选）、`checked`、`found`、`missing`、`map`。没有游戏世界时报错（这时什么都核对不了）。只看这一刻的界面：后面步骤才打开的界面现在找不到是正常的 |
| `gt.inspect` | `x`、`y`（视口归一化坐标 0–1，可省，省略时用当前光标位置） | `supported`（这个进程能不能拿到处理者）、`handlerSource`、`path`（光标下的命中路径，从外到内，每项含 Slate 类型和对应的 UMG 控件名 / 类，最多 30 项）、`target`（最深的 UMG 控件写出的选择器）、`verified`（这个选择器回解析是否唯一命中它）、`hint`、`lastHandler`（最近一次鼠标按下被谁接住，描述字符串）。只读，不动光标、不发输入 |
| `gt.record.start` | `name`、`startPie`（默认 true）、`keepPace`（默认 false：UI 操作之间的间隔不录） | 录制状态 |
| `gt.record.mark` | `text`、`screenshot` | 录制状态 |
| `gt.record.stop` | `save`（默认 true） | `path`、`absolutePath`、`steps`、`seconds`、`preview`（前 20 步） |
| `gt.record.status` | `last`（默认 10） | `recording`、`handlerSource`（`slateDebugging`：处理者来自 Slate 调试事件，准；`hitPath`：这个进程没编 Slate 调试，按命中路径推断，录不到打字）；在录时还有 `name`、`steps`、`seconds`、`keepPace`、`last`，超过步数上限时 `truncated: true` |

同一时间只跑一个 run（批量运行也是一个接一个）。每次 run 的报告和截图在 `Saved/GameTest/<名字>-<时间>/`（`report.json`、`*.png`）。

### 失败证据（1.2.0 起）

一步失败（取消不算）时自动留下三样东西，写进这次 run 产物目录的 `failures/step-<第几步>/`，报告里那一步的 `payload.evidence` 只记路径（相对项目根）：

| 字段 | 内容 |
| --- | --- |
| `dir` | 这一份证据的目录 |
| `screenshot` | `screen.png`，失败那一刻的游戏窗口（截不了时是 `screenshotError`） |
| `widgets` | `widgets.json`，那一刻屏上的控件树（结构和几何，不含属性，最多 3000 个节点）；`screens` 是屏数 |
| `target` / `selectorError` | 步骤有 `target` 时再解析一次：找到了是 `target`（路径、类、文字、`visibility`、`enabled`，说明「控件在，但没点成」）；找不到是 `selectorError`，带候选 |
| `log` | `log.txt`，失败前 50 行和之后 50 行日志（之后的最多等 3 秒，Stop 模式下失败后马上收尾，可能不满 50 行） |

一次 run 最多留 5 份。设置「失败时留证据」（`bCaptureFailureEvidence`，默认开）可以关掉。

### 批量运行（1.2.0 起）

`gt.run tag=smoke` 跑场景根里 `tags` 带 `smoke` 的全部场景（不分大小写，按路径排序），`gt.run scenarios=[…]` 跑指定的几份。前一个结束后下一帧起下一个，某个失败、解析不过都不挡后面的。进度在 `gt.job` 和 `gt.job` 事件的 `batch` 里：

```json
{ "id": "batch-7", "label": "tag:smoke", "active": true, "total": 3, "current": 1, "passed": 1, "failed": 0, "skipped": 0,
  "items": [{ "scenario": "smoke/login.json", "name": "登录", "status": "passed", "runId": "登录-5", "durationMs": 8400 },
            { "scenario": "smoke/shop.json", "status": "running" },
            { "scenario": "smoke/bag.json", "status": "pending" }] }
```

`status`：`pending`、`running`、`passed`、`failed`（带 `failedStep`、`message`）、`error`（没能开始，例如解析失败）、`skipped`（被取消）。批量运行时作业的 `busy` 一直为真（两个场景之间也是），`gt.step` 和单独的 `gt.run` 会被拒绝。

### 运行历史（1.2.0 起）

跑场景文件（不是临时步骤）的每次 run 结束都记进 `Saved/GameTest/history.json`，按场景文件分组，每个场景留最近 10 次。用 `gt.history` 读。和 `gt.runs`（本次会话最近若干次的完整报告，只在内存）不同，这份跨编辑器会话保留。

`gt.inspect` 的用途：录出来的步骤不对，或者手写的选择器解析不到时，把光标停在那个控件上调一次，看光标下到底是哪个控件、它在哪个 WBP 里、选择器该怎么写（`verified: false` 说明写出来的选择器有歧义或指不回它）；`lastHandler` 用来确认刚才那一下点击被谁接住了。

### 推送事件（UELink 推送通道，订阅主题）

| 主题 | 什么时候 | 字段 |
| --- | --- | --- |
| `gt.job` | run 开始、每完成一步、结束 | 和 `gt.job` 一样但不带 `steps`，多 `lastStep` 和 `phase`（`progress` / `finished`） |
| `gt.record` | 录制开始、每录一步、停止 | 录制状态 + `phase`（`started` / `step` / `stopped`），`last` 只有最新一步 |

## 其他入口

- **控制台命令**：`GameTest.Step <动作> [目标] [参数JSON]`、`GameTest.Health`、`GameTest.Actions`、`GameTest.Inspect`（把 `gt.inspect` 同样的内容打印到日志，取当前光标位置）。编辑器控制台、PIE 控制台、`-ExecCmds=` 都能用。
- **视口高亮**：执行器碰过的控件在游戏窗口里框出来（成功绿、失败红），停留 2.5 秒。

## 从旧场景迁移

1.1.0 把场景格式升到 `schemaVersion: 2`，旧文件不用改：

| 内容 | 处理 |
| --- | --- |
| `schemaVersion: 1` 的文件 | 照常能跑。读进来时没写 `via` 的 `click` / `type` 自动补上 `via: "code"`（只改内存，不改文件），行为和以前一样；旧选择器全部有效（新语法是超集） |
| 想让旧场景改用真实点击、真实打字 | 整份换：把 `schemaVersion` 改成 2，并去掉步骤里的 `via: "code"`。只换几步：保持 1，在那几步上写 `via: "pointer"`（click）或 `via: "keys"`（type），写了 `via` 的步骤不会被补。真实点击会检查遮挡和禁用，以前「被挡住也点成功」的步骤可能会失败，这暴露的正是真实玩家会遇到的问题 |
| 新录的、新写的 | `schemaVersion: 2`，`click` / `type` 默认真实输入 + 自动等待；确实要绕过遮挡做准备工作时单步写 `via: "code"` |
| 以前录成坐标的场景（`input.cursor` + `key`） | 照常能回放；想换成控件步骤就用 1.1.0 重新录一遍 |
| 以前录的 UI 操作之间的 `wait seconds` | 照常执行；v2 的 UI 动作自己会等，可以删掉 |

旧版插件遇到 `schemaVersion: 2` 的文件会报「schemaVersion 高于本插件支持的版本」并拒绝运行，不会跑错。

## 限制

- 云端编不了 C++：每一版都要在本地 UE 5.8 完整编译验证，没验证过的版本号带 `-beta.N`。
- `input.cursor` 和录制回放里点在游戏画面上的操作会移动真实光标，回放时别动鼠标。
- 真实指针的 `click` / `drag` / `wheel` 也会移动真实光标：回放时别动鼠标，游戏窗口要在前台。
- Shipping 不可用：GameTest 不进 Shipping，录制和 `gt.inspect` 用的 Slate 调试事件、控件元数据在 Shipping 里也没有。
- 没编 Slate 调试的进程（`gt.record.status` / `gt.inspect` 的 `handlerSource` 是 `hitPath`）：拿不到处理者，点击按光标下最深的控件推断（可能不如 `slateDebugging` 准），**录不到打字**；回放的 `click` 也核对不了接住按下的是不是目标。
- 纯 Slate 控件只认游戏窗口和它弹出的子窗口里的；编辑器工具窗口上的操作不录。
- 列表条目只认已经生成的：虚拟化列表里没滚到的条目没有控件，选择器指向它会报「没找到」，先 `scroll index=…`。`列表[序号]` 在列表顺序变化时会点错，能用 `列表[属性=值]` 就用它。
- `text=` 和 `hint.text` 受当前语言影响；录制优先用控件名。
- 手柄和 CommonUI 的焦点导航：按键照旧录成 `key`，不按焦点控件录。
