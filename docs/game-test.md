# GameTest

> 1.9.0 起场景可以写成脚本（schemaVersion 3：如果、循环、调用、变量、守护、调函数、接口库），见 [game-test-script.md](game-test-script.md)；「阻挡界面」改成守护。

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
- `clickVia`（可选，1.2.0-beta.7 起）这个场景里 `click` 没写 `via` 时的点击方式：`code`（默认，不动鼠标）或 `pointer`（真实鼠标），覆盖运行配置。
- `skipIf`（可选，1.7.0 起）`false` 时这个场景忽略所有跳过条件（每一步都照做），写对象时覆盖运行配置的同名段，见「已达成就跳过」。
- `uiWaitTimeoutMs`（可选，1.2.0-beta.6 起）这个场景里 UI 动作等界面的上限（毫秒，0 = 不限），覆盖运行配置和项目设置，见「UI 动作怎么执行」。
- `schemaVersion`：当前是 2（1.1.0 起）。2 里 `click` / `type` 默认走真实输入（真实指针、真实按键）；1 的文件照常能跑，见下面「从旧场景迁移」。写比插件支持的更高的版本会被明确拒绝。
- 每一步的公共字段：`action`（必填）、`target`、`note`、`timeoutMs`、`onFail`（`stop` 默认 / `continue`）、`hint`、`skipIf`（已达成就跳过，1.7.0 起，见「已达成就跳过」）。动作不认识的字段直接报错，不静默忽略。
- `hint`：录制写的线索（`asset` WBP 资源路径、`class`、`text`、`rect` 视口归一化 `[x, y, w, h]`、`vp` 录制时视口像素 `[宽, 高]`（1.2.5 起）；`shot`、`screen`、`opened` 录制截图和点完打开的界面（1.3.2 起，见「录制截图」），`ocr` 面板从截图认出的字；`fields` 点在列表、格子条目上时这个条目全部字段的缓存文件（1.6.0 起，见「条目字段缓存」））。录的时候选择器核对不唯一、但按线索挑得回来时多一个 `pick`（说明），`byIndex: true` 表示按位置写的选择器只按序号点、不按文字找回（1.2.4 起），挑不回来时多一个 `ambiguous`（原因）。回放在两种情况下用它（见「UI 动作怎么执行」里的「按录制线索消歧」）：选择器命中**同一个 WBP 的几个实例**时按线索挑回录的那一个；按位置写的选择器（`#类名#序号`、`列表[序号]`）唯一命中、但文字和 `hint.text` 对不上时，在同一个容器里换成文字对得上的那个（1.2.4 起）。别的情况不拿它换目标。手写场景不用写。

### 选择器（`target`）

| 写法 | 意思 |
| --- | --- |
| `屏名:路径` | 先按屏名挑屏，再走路径。屏名可以写 WBP 类名（去掉 `_C`）、控件名，或资源路径（`/Game/UI/Menu/WBP_MainMenu`），忽略大小写；`*` 表示任意屏 |
| `路径` | 不写屏名：在所有屏里找，多个命中算歧义 |
| `@句柄` | 之前 `find` 捕获的控件，不再重新解析 |
| `A>B` | `A` 落在一个嵌套的 WBP 实例（或列表条目）上，`B` 从**这个实例自己的**控件树根开始走。同一个通用按钮 WBP 用了十次也不会歧义，因为前面的父链已经唯一 |
| `容器/#类名#序号>B` | 没有 UMG 父面板、挂在某个控件 Slate 上的子界面（DynamicEntryBox 的条目、不是 UListView 的自定义列表、Lua 或 C++ 直接塞进 Slate 的 WBP）。快照把它们排在容器的 UMG 子控件后面，按 Slate 里的顺序数（1.2.0-beta.4 起） |
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
| `容器[条件,条件]` | 任意容器（ListView 族、格子和其他 UMG 面板、DynamicEntryBox、挂在 Slate 上的条目）里按**唯一标识**认条目（1.4.0 起），见下面「条目标识」 |

**条目标识**（1.4.0 起）：方括号里写 `字段=值`，多个条件用逗号连（都要满足）。不写来源时 ListView 族看条目的**数据对象**，别的容器看**条目控件自己**；要看别处写 `@来源`：

| 来源 | 读哪里 |
| --- | --- |
| `@item.路径` | ListView 条目的数据对象（ListView 的默认来源） |
| `@itemLua.路径` | 数据对象绑的 UnLua 实例表 |
| `@self.路径` | 条目控件自己的字段（非 ListView 容器的默认来源） |
| `@lua.路径` | 条目控件绑的 UnLua 实例表 |
| `@text=文字` | 条目里显示的文字 |
| `@row=行`、`@col=列` | 格子（UniformGridPanel、GridPanel）里的行列 |

路径可以用点进结构体（`Data.ItemId`），大小写不敏感；值按数字、字符串（忽略大小写）比较。只读字段（反射属性、Lua 表里的值），不调函数。按条件一个都找不到时报错并列出现有的值，找到多个时报错让你再加一个字段，**不退回按序号**。ListView 里命中的条目没生成（没滚到）时先滚出来再操作。

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

解析失败一律报错并给出候选，不做「取第一个匹配」。唯一的例外是带录制线索的步骤，见下面「按录制线索消歧」。写不准时用 `gt.inspect`（见「操作」）看光标下的控件该怎么写。

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
| `click` | `via`, `at`, `button` | 主模块（要 target） | 点击控件：先等控件可操作（见下面「UI 动作怎么执行」）。`via` 取 `code`（默认，1.2.0-beta.7 起）或 `pointer`，没写时按场景 / 运行配置的 `clickVia`。`code` 不动鼠标：从目标往上（不出所在 WBP）找最近的按钮 / 复选框 / 菜单锚点，调它的委托（按钮依次广播 OnPressed → OnClicked → OnReleased，蓝图和 Lua 绑在哪个上都收得到）；目标是 WBP（列表条目、入口图标）时点它里面唯一可见的那个按钮；都找不到时在控件位置发一组虚拟鼠标事件（移动、按下、松开，同一帧发完，不移系统光标）。结果里 `via` 写实际用的（`code` / `virtualPointer`），`clicked` 写被调的控件。`pointer` 是真实鼠标：还要点击点没被挡住，把光标移过去按下松开，核对接住按下的是不是目标。`at=[x,y]` 控件内位置（0–1，默认中心），`button`（只对 `pointer`）默认 `LeftMouseButton` |
| `drag` | `at`, `toTarget`, `toAt`, `by`, `button` | 主模块（要 target） | 拖动：从控件内 `at` 处按下，分几帧移到 `toTarget` 控件的 `toAt` 处（或按视口归一化位移 `by=[dx,dy]`，可以是负数）再松开；`toTarget` 和 `by` 二选一。起点和 click 一样先等可操作 |
| `wheel` | `delta`, `at` | 主模块 | 滚轮：给 target 时先等控件可操作、把光标移到控件上再滚；不给时在光标当前位置滚。`delta` 必填，正数向上、负数向下 |
| `type` | `text`, `commit`, `via`, `append` | 主模块（要 target） | 在输入框里打字：默认真实按键——把键盘焦点给控件，先全选（`append=true` 时不全选、接着原文字打），逐字发字符，`commit=true` 时再按回车；`via` 取 `keys`（默认）或 `code`，`code` 直接写文本并广播 OnTextChanged / OnTextCommitted（commit=true 才广播提交） |
| `select` | `index`, `property`, `equals`, `add` | 主模块（要 target） | 列表族（ListView / TileView / TreeView）选中条目：按索引、或按数据项属性匹配（属性名大小写不敏感）；TreeView 的匹配范围是根条目 |
| `scroll` | `mode`, `value`, `index`, `widget` | 主模块（要 target） | 滚动：ScrollBox 用 offset/end/start/widget，列表族用 index |
| `set` | `value`, `checked`, `option`, `index`, `expanded`, `property`, `equals`, `open`, `key` | 主模块（要 target） | 通用赋值：滑条数值/复选/下拉选项/切换器页号/展开态（含 TreeView 条目展开）/菜单开合/按键；给了该控件不认的字段即硬错误 |
| `screenshot` | `name`, `annotate` | 主模块 | 截游戏窗口（含 UMG）落盘，返回相对路径与像素尺寸；annotate 画编号框并给出图例 |
| `console` | `command`, `expectContains` | 主模块 | 执行一条控制台命令（等价于在控制台里敲它）；给 expectContains 时输出必须包含该子串，否则本步失败（用来让世界状态可被场景判定） |
| `input.cursor` | `x`, `y` | 主模块 | 把光标移到视口内归一化坐标（0–1），用于驱动直读鼠标位置的游戏逻辑（如瞄准）；移动的是真实光标 |
| `key` | `key`, `phase`, `holdMs` | 主模块 | 按键：键盘键或鼠标键（UE 的键名，如 W、SpaceBar、LeftMouseButton），走 Slate 输入入口，和真实按键同一条路；phase=tap（默认，按下隔一帧松开；给 `holdMs` 时按住这么多毫秒再松开，即长按）/ press / release（按住期间还要做别的操作时用）；press 按住的 Alt / Ctrl / Shift 会带进之后的点击、滚轮、按键事件（1.2.0-beta.5 起）；鼠标键在光标当前位置按（先 input.cursor）；给 `target` 时键盘事件发给这个控件（输入框里的回车、退格、方向键），否则发给游戏视口 |
| `move` | `to`, `toActor`, `toTag`, `yaw`, `pitch`, `radius`, `via`, `fallback`, `teleportCommand`, `drive`, `forwardKey`, `path`, `jumps`, `fly`, `client` | 主模块 | 让本地玩家的角色走到一个点（1.3.0 起），见下面「移动步骤」 |
| `actor.get` | `class`, `tag`, `client`, `locallyControlled` | 主模块 | 在游戏世界里按 class 或 tag 找 actor（唯一命中），回报对象路径、位置、朝向与速度；多客户端会话可用 client（\"server\" 或 0 起序号）寻址世界、locallyControlled 区分本端与远端 |
| `net.compare` | `class`, `tag`, `from`, `to`, `tolerance` | 主模块 | 跨端一致性断言：同一玩家对应的 actor 在两端世界的位姿偏差不超过 tolerance（厘米）；from/to 各可给 client 与 locallyControlled（缺省 from=server、to=client0）；给 timeoutMs 则跨帧等到一致 |
| `assert` | `exists`, `visible`, `enabled`, `text`, `textContains`, `textLike`, `checked`, `count`, `selected`, `selectedProperty`, `stateKey`, `field`, `image`, `any`, `equals` | 主模块（要 target） | 断言目标状态（stateKey+equals 可断言快照 state 里的任意字段）；失败信息里带实际值，不做静默通过。`textLike`、`checked`、`field`、`image`、`any` 1.7.0 起，和跳过条件共用一套判断，见「已达成就跳过」里的条件表 |
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

**按录制线索消歧**（1.2.0-beta.4 起）：选择器命中同一个 WBP 的几个实例（「在 N 个屏里都命中」）、步骤又带着录制的 `hint` 时，「解析到」这一项按线索挑：先只留真在屏上的（在游戏视口里、整条父链可见、有尺寸，对象池里藏着的不算），剩一个就是它；还剩多个时按文字挑（三档，见下面「文字怎么比」）；再剩多个按 `hint.rect` 挑位置和大小最近的。最近的偏差超过 0.08（视口归一化）或最近两个差不到 0.02 时不挑；录制和回放的屏幕宽高比差超过 3%（`hint.vp` 记着录制时的视口像素，1.2.5 起）时只比中心、偏差放宽到 0.15，接着等，超时报「按录制线索也挑不出：…」。挑中时步骤结果里的 `picked` 写怎么挑的。没有 `hint` 的手写步骤照旧报歧义。

**容器里顺序变了按文字找回**（1.2.4 起）：录制器没有名字可写时按位置写选择器（`#类名#序号`、`列表[序号]`、`容器/#类名#序号>…`），容器里的条目顺序一变（历史记录按最近使用排、列表重新排序），同一个序号就是别的按钮了。所以带 `hint.text` 的 UI 动作在选择器唯一命中后再核对一次文字：三档里任一档对得上就照常；不一样时从最靠近目标的那个按位置写的段开始，把序号换成同容器里的其他序号（最多试 48 个，只认此刻在屏上、唯一命中的），按文字三档挑最高的一档，只有一个就点它，有几个按 `hint.rect` 挑最近的、挑不出就不换；这一层一个都对不上再往外一层找，都对不上照旧按序号（按钮文字本来就会变的情况）。换了的时候步骤结果的 `picked` 写原来的段、两边的文字和换成的路径。`assert`、`wait until=widget` 不换（断言要看的就是原位置上是什么）。就是要点「容器里第几个」、不管上面写什么的步骤，在 `hint` 里写 `"byIndex": true`（面板步骤详情里「回放怎么认」选「只按第几个」），回放照序号点。

**文字怎么比**（1.2.5 起，上面两种挑法共用）：按钮上的字常带会变的部分（数量、倒计时、红点、富文本标签），录制的 `hint.text` 和此刻的文字分三档比，先中的为准，同一档有几个才按位置挑：

| 档 | 怎么比 | 例子 |
| --- | --- | --- |
| 完全一样 | 原文相等 | 「结束所有引导」 |
| 忽略数字和标点后一样 | 去掉富文本标签 `<...>`、数字（含全角）、空白、中英文标点，英文转小写后相等 | 「领取(3)」和「领取(0)」、「剩余 02:15」和「剩余 01:58」 |
| 包含 | 归一后一方包含另一方，短的至少 2 个字且不短于长的 40% | 「商店」和「商店 NEW」 |

换了语言时三档都对不上，退回按序号或报歧义。`picked` 里写按哪一档对上的。

**自动滚动**（1.2.5 起）：等目标可操作时，(1) 选择器停在 `列表[序号]` / `列表[属性=值]` 指向、但还没生成的条目上（虚拟化列表没滚到），调列表的 `ScrollIndexIntoView` 把它滚出来再等，同一步里同一个条目只滚一次；(2) 目标在某个 ScrollBox 里、但不在它的可见范围，调 `ScrollWidgetIntoView`（不带动画）滚过去再等，嵌套的由里往外，一步最多 4 次。(3) **翻找**（1.7.1 起）：整个找不到、又认不出是第几个条目（按文字、路径认的条目没生成）时，找不到满 0.6 秒后把屏上所有显示着的 UserWidget（任何 `UUserWidget` 子类，不按名字认界面；1.7.2 起）里的列表、ScrollBox 从头一页一页往后翻（选择器写了屏、屏也找得到时它的先翻，最多 12 个容器，每页之间隔 0.2 秒等条目生成），翻到就停；**全都翻到底还没有就不再滚，原地等**到这一步超时。之后某个列表的条目数、滚动框的长度变了（数据晚到）就再翻一遍。步骤结果里 `scrolled` 写滚了什么。能写 `列表[属性=值]` 时还是写它：直接滚到那一条，不用一页页翻。

**真实鼠标被挡住改用代码点击**（1.2.5 起）：`pointer` 方式的 `click` 点击点被同一个控件连续挡住 1.5 秒（新手引导只留一个洞的遮罩之类），而 `pointer` 是从场景或运行配置继承来的（这一步自己没写 `via`），就改用代码点击，步骤结果的 `fallback` 写被谁挡住。这一步自己写了 `via: "pointer"` 时照旧等到超时报「被挡住」：那是在测遮挡本身。

等待上限（`wait until=widget / widgetGone` 也一样）按这个顺序取：步骤的 `timeoutMs` > 场景顶层的 `uiWaitTimeoutMs` > 运行配置（下一节）的 `uiWaitTimeoutMs` > 项目设置「UI 等待超时」（`UiWaitTimeoutMs`）。**默认 0 = 不限**（1.2.0-beta.6 起，以前默认 10 秒）：界面没出来就一直等，加载慢、网络慢时不误报；真卡住了在面板上点停止（`gt.stop`），等待中的这一步标失败，消息里带最后一次检查卡在哪。不限时报告里这一步的 `timeoutMs` 记 0。所以场景里不用在每个 UI 操作前插 `wait`；显式的 `wait until=widget / widgetGone` 照样保留，给「等某个界面出现但不操作它」用。

### 移动步骤（`move`，1.3.0 起）

让本地玩家（第一个玩家控制器）的角色走到一个点。录制时用「录移动」生成（见「录制回放」），手写或 Agent 写时只给 `to` 就行：

```json
{ "action": "move", "to": [1200, -340, 88], "yaw": 90, "pitch": -10,
  "path": [[0, 0, 88], [420, 10, 88], [800, -200, 96], [1200, -340, 88]], "jumps": [2],
  "hint": { "from": [0, 0, 88], "durationMs": 8200, "distance": 1460, "nav": true, "pawn": "BP_Hero_C", "map": "/Game/Maps/Town" } }
```

| 字段 | 说明 |
| --- | --- |
| `to` | 目标点，世界坐标 `[x, y, z]`（厘米）。和 `toActor` / `toTag` 二选一 |
| `toActor` / `toTag` | 走到某个 actor 旁边（按类名或标签，唯一命中，同 `actor.get`）；目标在动时每帧跟着它的位置 |
| `yaw` / `pitch` | 到了以后把控制器朝向（镜头）转到这个角度。录制时自动写：后面点游戏画面的步骤靠它对准 |
| `radius` | 到达半径（厘米）。默认：`to` 时取 `max(胶囊半径 × 2, 60)`，`toActor` 时取目标半径 + 角色半径 + 30 |
| `via` | `auto`（默认）/ `nav`（只用导航网格）/ `path`（只按录制轨迹）/ `probe`（只探路）/ `teleport`（直接瞬移） |
| `fallback` | `none`（默认）/ `teleport`：前面的走法都不行时瞬移 |
| `teleportCommand` | 瞬移用的控制台指令模板，`{x}` `{y}` `{z}` `{yaw}` 会被换成目标；不写时直接 `TeleportTo` |
| `drive` | `input`（默认，每帧 `AddMovementInput`，像推摇杆）/ `keys`（按住 `forwardKey`，默认 W，并把镜头转向下一个路点） |
| `path` / `jumps` / `fly` | 录制的轨迹（已简化）、在哪几个点之后起跳、哪几个点是游泳 / 飞行（按三维方向走） |
| `client` | 多端会话里用哪一端的世界，同 `actor.get` |

`via: "auto"` 的顺序：

1. 项目有导航网格、起点终点都投影得上、路径完整 → 寻路走（导航网格还在生成时先等最多 30 秒）；
2. 有 `path` → 按录制轨迹走：角色离轨迹 3 米内直接接上，再远就先「探路」走到轨迹上；
3. 没有 `path`，或上面都不行 → 整段探路；
4. 都不行：`fallback: "teleport"` 时瞬移，否则失败，`tried` 里写清每一种为什么不行。

**回放不核对起点**：角色从哪里出发都行，`hint.from` 只给人看。

**怎么走**：不管路线是哪来的，都交给同一个驱动——每帧对角色调 `AddMovementInput`，和玩家输入走同一个移动组件入口，碰撞、台阶、走速、动画、联机的客户端预测都和真人一样，不需要知道项目的按键绑定。走的时候镜头转向前进方向，到了再转到 `yaw` / `pitch`。`jumps` 标的点调 `ACharacter::Jump()`。**2 秒没往前走 20 厘米算卡住**：先试一次跳，还卡就换下一种走法。

**探路**（没有导航网格时）：在角色和目标之间做 A*，地面按 50 厘米一格，展开到一格才检查它——往下打射线找地面、看坡度，再用角色自己的胶囊做重叠检查；两格之间扫一次胶囊、看台阶高度。台阶高度、可行走坡度、胶囊尺寸、碰撞通道都从角色读。搜索范围是起终点连线外扩 30 米，最多查 2 万格，每帧最多 300 次查询，不卡游戏。找到后把路线拉直。**只认在地上走**：跳沟、爬梯子、游泳、要先开门的路算不出来，这些靠录制轨迹。不去现场生成引擎的导航网格：网格默认只在编辑器里烘焙，游戏里生成要改项目设置。

**瞬移**：单机和监听服务器直接 `TeleportTo`；客户端直接挪会被服务器拉回，在运行设置里配 `move.teleportCommand`（项目自己的 GM 瞬移指令）。结果里 `used: "teleport"` 表示这一步路上的问题没测到。

结果的 `payload`：`used`（实际用的走法：`nav` / `path` / `probe` / `teleport`，一开始就在终点附近是 `none`）、`reached`、`distance`（结束时离目标多远）、`ms`、`tried`（`[{via, why}]`，换过走法时才有）、`stuck`（卡住的位置）、`trail`（实际走的路线，最多 100 个点）、`probeCells`（探路查了多少格）。

没写 `timeoutMs` 时上限按录制用时 × 3 + 10 秒（没有录制用时按距离 ÷ 走速估），在 15 秒到 10 分钟之间。

### 已达成就跳过（`skipIf`，1.7.0 起）

录一步的目的常常是让界面变成某个样子：点开关让「重置点」勾上、点「领取」让按钮变成「已领取」。回放时界面已经是那个样子了，再点一次反而把勾取消、或者卡在一个点了没反应的按钮上。步骤写 `skipIf`，回放到这一步先看一眼，已经达成就不做：

```json
{ "action": "click", "target": "WBP_Settings:Root/RowResetPoint>BtnToggle",
  "skipIf": { "target": "WBP_Settings:Root/RowResetPoint>ChkBox", "checked": true } },
{ "action": "click", "target": "WBP_Reward:Root/BtnClaim",
  "skipIf": { "textLike": "已领取", "also": 1 } },
{ "action": "click", "target": "WBP_RewardPopup:Root/BtnOk" }
```

条件（和 `assert` 共用；写在同一个对象里都要成立）：

| 字段 | 说明 |
| --- | --- |
| `exists` / `visible` / `enabled` | 在不在、看不看得见、能不能点 |
| `text` / `textContains` | 文字完全一样 / 包含 |
| `textLike` | 文字「完全一样」或「忽略数字和标点后一样」。**不按包含比**：「已领取」包含「领取」，按包含比会把没领的当成领过了。按钮这类控件读它里面（和叠在它上面）的字 |
| `checked` | 勾选状态：UCheckBox，以及 CommonUI 可选中按钮的选中（GameTestCommonUI 模块，快照 state 的 `selected`）。自己拼的勾（一张图显隐）用 `visible` 看那张图，或用 `field` 看存状态的字段 |
| `field` + `equals` | 读控件所在 WBP 的字段：`@self.路径`（WBP 的 UPROPERTY）、`@lua.路径`（它的 UnLua 实例表）、`@item.路径` / `@itemLua.路径`（作为列表条目显示的数据对象和它的 Lua 表）。只读字段，不调函数 |
| `image` | UImage 当前贴图的资源名包含这段（勾是换图的项目） |
| `count`、`selected`、`selectedProperty`、`stateKey` + `equals` | 同 `assert` |
| `any` | `[条件对象, …]`，任一成立就算成立 |

配置字段：`target`（条件看的控件，省略 = 这一步自己的 `target`）、`also`（跳过时连后面几步一起跳过，0–20）、`settleMs`（稳住多久才下结论，默认 300，最多 10000）、`verify`（做完核对，默认开）、`verifyTimeoutMs`（核对等多久，默认 5000，最多 120000）、`by`（`auto` = 录制自动加的，给人看）。

回放时：

- 这一步开始前每 0.1 秒判断一次。条件**成立并稳住** `settleMs` 就跳过，状态记 `already`（「已达成」，**算通过**，和被取消的 `skipped` 不同）；**不成立并稳住** `settleMs`、并且条件看的控件（或这一步的目标）已经在屏上了，才照常做。多等这一小会儿是怕界面数据还没刷上来：按钮先显示「领取」，服务器回包后才变「已领取」。
- 「不存在 / 不可见」这类反面条件只在所在的界面已经在屏上时才算数（找不到既可能是领过了，也可能是界面还没打开），稳住的时间加倍；`target` 要写屏名（`WBP_X:路径`）。
- 等的上限和 UI 动作一样（步骤 `timeoutMs` > 场景 / 运行配置 / 项目设置的 `uiWaitTimeoutMs`，默认不限）。
- 跳过的步骤 `payload.skip`：`{cond, say, actual, target, waitedMs, also?}`；连带跳过的步骤也记 `already`，消息「随第 N 步跳过」，`payload.skip.with` 是带头那一步的序号。报告多一个 `already` 计数。
- 做完核对：照常做了的步骤成功后，条件要在 `verifyTimeoutMs` 里成立，不成立这一步失败（「点了，但 … 没变成 …」）；成立时 `payload.verified` 记核对时的实际值。
- 照常做的步骤 `payload.skipCheck` 写判断时看到的实际值（为什么没跳）。
- 跳过的步骤不算「上一个 UI 操作」，不触发 `uiStepGapMs` 的间隔。

开关和默认值（优先级从低到高）：运行配置 `_settings.json` 的 `skipIf` 段（`false`，或 `{enabled, settleMs, verify, verifyTimeoutMs}`）> 场景顶层的 `skipIf` > 这次运行的 `gt.run ignoreSkipIf: true`（**全部照做**：测第一次领取、新号这类路径）。关掉跳过时做完核对也不做。批量运行不支持 `ignoreSkipIf`。

录制时推断（`gt.record.start inferSkip`：`suggest` 默认（1.8.0 起，**默认不跳过**：只列候选，跳过条件由人在面板步骤详情里加）/ `auto` / `off`；`stateWindowMs` 默认 2000）：

- 按下 UI 控件时读一遍它所在的整个界面（嵌套的 WBP 一起，最多 800 个控件）：显隐、能不能点、勾选、文字、图片，以及按的控件所在 WBP 的字段（`@self`、`@lua`）。松开后 0.15、0.5、1 倍 `stateWindowMs` 各读一次，最后一次和按下前比。
- 每处变化打分：勾选变了 10；按的按钮上的字变了 8（只变了数字和标点的不算）；名字像「勾 / 已领 / 完成 / 选中 / 红点」的图片显隐 7；名字像这些的布尔字段 8、别的布尔字段 6；按的按钮变灰 6；其余 4–5。前 6 个写进 `hint.state.cands`（`[{target?, kind, cond, before, after, score}]`）。
- `auto` 时最高分 ≥ 7 的直接写成 `skipIf`（带 `by: "auto"`），写之前用回放同一套判断核对一遍「此刻条件成立」，核对不过不写；`hint.state.picked` 是写进去的那个候选。工具栏提示「第 N 步：已达成就跳过（…）」。不在录制中途停下来问，录完在面板步骤详情里换候选或删掉。
- 点完打开的新界面记在 `hint.opened`（没开录制截图时也记）；停止录制时给有候选的步骤算 `also`（写了跳过条件的写进条件；只列了候选的记成 `hint.state.also`，面板加条件时带上）：后面紧挨着、目标在这些新界面上的步骤（中间的 `wait` 一起），阻挡界面规则管的界面不算。
- 拖动不推断。

### 运行配置和阻挡界面（1.2.0-beta.6 起）

场景根下的 `_settings.json` 是整个项目共用的运行配置（跟着场景进版本管理，不放 UE 的 Config；列场景时跳过它）。面板 GameTest 页「运行设置」读写它（`gt.config`），也可以直接改文件：

```json
{
  "uiWaitTimeoutMs": 0,
  "uiStepGapMs": 1000,
  "clickVia": "code",
  "randomSeed": 42,
  "setup": [
    { "action": "console", "command": "gm.ResetGuide" }
  ],
  "blockers": [
    { "widget": "WBP_Notice", "action": "close", "note": "登录公告" },
    { "widget": "WBP_Guide", "action": "call", "function": "Skip", "delayMs": 500 },
    { "widget": "/Game/UI/Common/WBP_Reconnect", "action": "command", "command": "gm.Reconnect" }
  ]
}
```

`move`（1.3.0 起）是 `move` 步骤的默认设置和录制快捷键：`fallback`、`teleportCommand`、`radius`、`drive`、`forwardKey`（同 `move` 步骤的字段），`hotkeys`（默认 `true`；项目自己用 Alt+数字时写 `false`）。场景顶层也可以写 `move` 段覆盖，优先级：步骤字段 > 场景 > 运行配置。

`uiStepGapMs`（1.2.6 起，毫秒，0–60000，没写按 1000）是**相邻两个 UI 操作之间至少隔多久**：UI 操作指写了 `target` 的 `click`、`drag`、`wheel`、`type`、`select`、`scroll`、`set`、`key`，从上一个 UI 操作结束（成功或失败）算到下一个开始，中间的 `wait`、`assert` 等步骤用掉的时间也算进去，不够再补。和录制时的手速无关（录的场景 `stepDelayMs` 是 0），也和 `stepDelayMs` 叠加取较晚的那个。`0` 表示不隔。

`clickVia`（1.2.0-beta.7 起）是 `click` 没写 `via` 时的点击方式，`code`（默认）或 `pointer`，场景顶层的 `clickVia` 优先。

`setup`（1.2.5 起）是**前置步骤**：写法和场景里的步骤一样，从场景文件跑（`gt.run scenario=…`、批量）时先跑这几步再跑场景，用来放清引导、重置任务、发道具这类 GM 指令，让每次回放从同一个存档状态开始。场景顶层写 `"setup": false` 的不跑（专门测新号、测引导的场景）；直接给 `steps` 的临时运行（面板「从这一步开始运行」、Agent 试一步）不跑。报告里前置步骤带 `setup: true`，`index` 是它在前置步骤里的序号，不占场景的步骤序号；失败证据放 `failures/setup-NN/`，历史和批量里前置步骤失败记成第 0 步。前置步骤写错时整次运行不开始。

`randomSeed`（1.2.5 起，可选整数）：每次开跑前用它调 `FMath::RandInit`、`FMath::SRandInit`，固定引擎全局随机（蓝图 Random 节点、`FMath::Rand` / `FRand`），日志里记一行。游戏自己的 `FRandomStream`、Lua 的 `math.random`、服务器下发的结果管不到，用前置步骤里项目的 GM 固定。

`skipIf`（1.7.0 起）是**已达成就跳过**的开关和默认值：`false` 整个项目都照做；对象时 `enabled`、`settleMs`（默认 300）、`verify`（默认 true）、`verifyTimeoutMs`（默认 5000），见「已达成就跳过」。

`itemKeys`（1.4.0 起）是**条目怎么认**的规则：录制时点到这类条目先按它拼条件，见「列表、格子里的条目怎么认」。

`blockers` 是**阻挡界面**：公告、新手引导、活动弹窗、断线重连这类不定时弹出、会挡住后面步骤的界面。运行期间（每 0.2 秒看一次）规则里的界面一出现在屏上就处理掉：

| 字段 | 说明 |
| --- | --- |
| `widget` | 类名（`WBP_Notice`，带不带 `_C` 都行）或资产路径（`/Game/UI/WBP_Notice`）；它的子类也算 |
| `action` | `close`（默认）：`RemoveFromParent`，直接挂在 Slate 上移不掉的再折叠；`call`：调这个界面上的无参函数 `function`（蓝图里的函数名，有返回值可以、有输入参数不行）；`command`：执行控制台指令 `command`（先交给本地玩家控制器，GM、作弊管理器上的 exec 都能走到，没有玩家控制器再交给引擎） |
| `delayMs` | 出现后过多久再处理，给打开动画留时间（默认 0，最多 60000） |
| `enabled` | `false` 时暂时不用这条 |
| `note` | 给人看的说明 |

- 正在等的那一步要操作的就是这个界面（选择器里写着它的类名）时不处理它：场景里明写了「点公告的确定」，就不该被规则抢先关掉。
- 同一个界面实例一次运行只处理一次；处理后 1 秒还在屏上，记一条 `ok: false`「处理后还在」，不反复处理。
- 每次处理都记进当时那一步结果的 `payload.blockers`（两步之间发生的记在下一步上）：`[{widget, rule, action, ok, message, note?}]`，同时写一行 LogGameTest。面板的步骤详情里显示「自动关掉了哪些阻挡界面」。
- 配置写错（缺字段、action 不认识）时整次运行不开始，报哪一条错了：规则没生效却照跑，弹窗挡住时只会看到一串莫名其妙的等待。

动手：真实指针 `click` 把光标移到控件内 `at` 的位置，按下、隔一帧松开，和玩家点的效果一样，控件自己处理。按下后核对这次按下的处理者是目标本身或它里面的控件（点在按钮里的文字上，接住的是按钮也算），不是就报「点了 X，但接住按下的是 Y」，不算通过；没人接住按下时（有的控件只在松开时响应）不算失败，结果里 `handled: false`。这个进程拿不到处理者时（见限制）跳过核对，结果里写明「核对不了」。

## 录制回放

1. 面板上点「录制」（`gt.record.start`，可给 `name`、`keepPace`、`shots`）。编辑器里 PIE 正在跑时，场景开头自动加一条 `pie.start`（同一张地图），回放能自己拉起游戏。
2. 在游戏窗口里正常操作。只录发给游戏窗口（和它弹出的子窗口，如下拉菜单）的输入，编辑器其他面板上的操作不录。
3. 需要检查的地方：按住 **Ctrl+Alt 点一个控件**，录一条断言（见下表）；或插检查点（`gt.record.mark`，可带 `screenshot: true`），之后在面板里改成 `assert`。
4. 要录角色走路：按 **Alt+1**（或点工具栏的「录移动」）开始，走到目的地再按一次结束，见下面「录移动」。
5. 停止（`gt.record.stop`），写成 `recorded/<名字>.json`（`schemaVersion: 2`），重名加序号。

**录制工具栏和快捷键**（1.3.0 起）：录制时有一条悬浮的工具栏小窗口（1.5.0 起是编辑器的独立小窗口、不是游戏界面：默认停在游戏视口中下方并跟着视口走，按住左边的把手或空白处可以拖到任何地方，包括拖出游戏视口、拖到别的显示器，拖过就留在那里直到这次录制结束；不抢焦点，点它游戏不失焦；只在录制时出现，回放时没有），点它、拖它本身都不录。鼠标被游戏锁住时用快捷键，录制器自己用掉、不传给游戏：

| 键 | 作用 |
| --- | --- |
| `Alt+1` | 录移动 / 结束移动 |
| `Alt+0` | 放弃这段移动 |
| `Alt+2` | 记下当前位置：写一条只有 `to` + 朝向的 `move` |
| `Alt+3` | 在这附近：写一条 `wait until=actorNear`（角色的类、当前位置、半径 200），检查前面的操作确实把角色带到了这里 |
| `Alt+4` | 断言：下一次点击录成断言（和 Ctrl+Alt 点一样） |
| `Alt+5` | 检查点（`mark`） |
| `Alt+K` | 正在暂停问「这一格怎么认」时：接受推荐的继续（1.4.0 起） |

项目自己用 Alt+数字时在运行配置写 `"move": {"hotkeys": false}`，只用工具栏和面板按钮。

**录移动**：开始到结束之间每帧记角色的位置（离上一个点 50 厘米以上才记，同时记移动模式和起跳），**别的操作都不写成步骤**——WASD、转视角、跳都只体现在轨迹里。结束时轨迹简化（偏离连线不到 30 厘米的点去掉，起跳点和游泳 / 飞行的起止点保留，最多 300 个点），写成一条 `move`。

- 中间**点了界面**：移动段自动结束（以那一刻的位置为终点），这一下点击照常录成 `click`。
- 开始时按着的键（按住 Shift 跑）：在步骤里补一条松开；结束时还按着再补按下。
- 角色换了（上车、变身、重生）、地图换了、进了过场动画（`bCinematicMode`）：这段作废，工具栏提示原因。这些位移不是角色自己走出来的，回放推摇杆复现不了。
- 靠界面或技能传送（点地图传送）不用移动段：照常录点击，再用「在这附近」检查结果。

**怎么认出点的是哪个控件**（1.1.0 起）：不看控件是什么类，看**谁接住了这次输入**。Slate 自己会告诉我们哪个控件处理了这次按下（引擎的 Slate 调试事件，Widget Reflector 看事件用的同一条通道），再把它反查成 UMG 控件、写成选择器。按钮、复选框、在蓝图或 Lua 里重写了 OnMouseButtonDown 的 WBP、UImage 绑的事件都是同一条规则，所以以前「项目的按钮不是 UButton 就只能录成坐标」的问题没有了。

按下没人接住、但点的位置上有游戏界面控件时（只在松开时响应的列表条目、自定义按钮），按命中的那个控件录成点击（1.2.3 起），不再录成「摆光标 + 按鼠标键」；控件写不出选择器时往外找最多 4 层，用写得出的外层控件。只有真点在游戏画面上才录成 `input.cursor` + `key`。

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
| 发给游戏的键盘、手柄、鼠标键 | `key`：短按（< 0.25 秒）录成 `tap`；长按、期间没做别的操作录成 `tap` + `holdMs`；按住期间做了别的操作（按住 Alt 点按钮）录成 `press` … `release` |
| 按住 Ctrl+Alt 点一个控件 | 这次点击被录制器吃掉、不传给游戏，录成 `assert target=… visible: true`；文字类控件顺带断言此刻的 `text` |
| 两次游戏操作隔了 0.15 秒以上 | `wait until=seconds` |

- **UI 操作前不录 `wait`**：回放的 `click` / `type` 等自己会等控件可操作，录下来的秒数只会让慢机器失败、快机器白等。游戏画面上的操作前照旧按间隔补 `wait`（游戏逻辑没有「可操作」的信号可等）。想原样复现手速时开始录制给 `keepPace: true`，所有间隔都录。
- **选择器怎么写出来**：路径用设计器里起的控件名；运行时生成的名字（`WBP_Item_C_12` 这类）换成 `#类名#同类序号`；嵌套的 WBP 用 `>` 进去；容器里的条目按下面「列表、格子里的条目怎么认」挑唯一标识写成 `容器[条件]`，挑不到时写 `容器[序号]`；没有 UMG 父面板、挂在某个控件 Slate 上的子界面写成 `容器/#类名#序号>…`。写完用回放同一套解析当场核对，必须唯一、指回同一个控件；不唯一但按位置、文字挑得回来时 `hint` 里写 `pick`，挑不回来才写 `ambiguous`，这种步骤回放前要人看一眼（多半是同一个 WBP 直接加在视口上好几份，改用 `gt.inspect` 找能区分它们的父界面）。
- **列表、格子里的条目怎么认**（1.4.0 起）：点到容器里的条目时，**按下那一刻**把条目的字段（ListView 看数据对象和它的 Lua 表，别的容器看条目控件自己、Lua 表、文字、行列）和同容器其他条目的值一起读出来打分：数据对象上、唯一、像 Id 的整数或短字符串分高；像位置（index、slot）、小数、布尔、长文字、点一下就变的值分低或不要。松开后再读一次，点完就变了的（选中、数量）不当标识。
  - 最高分够准（唯一、没变、分数 ≥ 5）就直接用（`hint.key.by = "auto"`）；运行配置 `itemKeys` 里有这类条目的规则就按规则（`rule`）。
  - 拿不准时**暂停游戏问人**（`pending`）：推送 `gt.record { phase: "ask" }`，状态里的 `ask` 有这一步的序号、候选、截图，面板弹窗让你挑字段、组合，或选「按位置」「先按文字找」「先跳过」，用 `gt.record.key` 回答。暂停期间不录任何输入，这段时间也不算进录制间隔；游戏原本就停着的不动它。录制工具栏上同时提示「游戏已暂停」，不想切到浏览器时在游戏里按 `Alt+K` 接受推荐的继续。
  - 什么时候问由 `gt.record.start` 的 `askKey` 定：`unsure`（默认，拿不准才问）/ `always`（每次都问）/ `never`（不问，先用分数还行的、都不行按位置，步骤标「待定」录完再定）；`askTimeoutSec` 是停多久自动继续，**默认 0 = 一直等**（面板 GameTest 设置页可改）。自动继续或停止录制时这一步保持「待定」。
  - **同一列表沿用**（1.5.0 起）：这次录制里对一个列表（条目类 + 容器）回答过（挑了字段，或选了按位置、先按文字找；「先跳过」不算），后面再点到同一列表的条目直接按同样的字段拼条件，唯一就用（`hint.key.by = "same"`），不再问；这一格按它认不出（读不到、不唯一）才照常挑、照常问。规则命中（`rule`）和沿用（`same`）的条目在 `always` 下也不问：`always` 的意思是每个列表第一次都问。
  - 回答时勾「以后都按这个认」会把规则写进运行配置的 `itemKeys`：`{"entry": "WBP_BagSlot", "container": "WBP_Bag:Root/ItemList", "by": ["Data.ItemId"]}`（`entry` 条目类名，`container` 可省略 = 这类条目在哪个容器里都按它，`by` 是条件左边的写法）。之后录到同类条目先按规则拼条件，唯一就直接用。
  - 步骤的 `hint.key`：`from`（目标里被改写的那段前缀）、`base`（容器路径）、`pos`（按位置的写法）、`entry`（条目类名）、`count`（容器里条目数）、`by`（`auto` / `rule` / `same` / `pending` / `user` / `index` / `text`）、`cond`（用的条件）、`list`（ListView 族）、`cands`（候选：`by` 条件左边、`cond` 完整条件、`src` 来源、`v` 值、`score`、`hits` 命中几个、`unique`、`changed` 点完变了、`combo` 两个字段组合、`why` 扣分原因）。`cond` 有值时回放不按文字换位置（`hint.text` 只用于显示）。
- 录出来的步骤不对（点的不是想要的控件、选择器有歧义）时，把光标停在那个控件上调 `gt.inspect`，看光标下的命中路径、谁接住了上一次按下、选择器该怎么写。

实现：在 Slate 上挂一个 `IInputProcessor`（引擎公开的输入预处理扩展点）拿原始事件，只看不吃（拾取断言那一下除外）；处理者来自 `FSlateDebugging` 的输入事件，反查 UMG 用 Widget Reflector 同一份元数据。回放时 `key` 走 `FSlateApplication::ProcessKeyDownEvent` 等入口，和真实按键同一条路（Slate → 视口 → PlayerController → EnhancedInput），所以不需要知道项目的输入资产。回放发出的合成输入不会被录进去。run 收尾时按下没松开的键全部松开。不改引擎、不改项目代码。

**录制截图**（1.3.2 起）：每次在 UI 控件上按下、点在游戏画面上时截一小块图，写进那一步的 `hint`：

| 字段 | 内容 |
| --- | --- |
| `shot` | 被点控件的截图（相对项目根）：默认就是控件那一块（1.5.0 起）；开了外扩时左右每边扩 `min(宽 × padX, padXMax)`、上下每边扩 `min(高 × padY, padYMax)`（默认 20% / 80 像素、50% / 40 像素）；超过 640×360 整张缩小；控件描红框（框外放不下时描在框内侧），不画点击位置的十字（1.5.0 起，十字压在字上 OCR 会认错）。点在游戏画面上时是光标周围 120×120 |
| `shotBox` | 红框在这张图里的位置 `[x, y, w, h]`（像素）：面板 OCR 只认框里的字 |
| `screen` | 当时游戏视口的整屏缩略图（宽 480）：顶层界面变了才存新的一张，没变的步骤指向上一张 |
| `opened` | 按下后 2 秒内新出现的顶层界面类名（`["WBP_Shop"]`）：点这一下打开了什么 |

- 截的是**按下那一刻**：按钮默认松开才触发，图里是点之前的界面。截图同步做，每次按下卡一下（几十毫秒，只在录制时有）；裁剪、描框、压 PNG、写盘在后台线程。
- 图放 `Saved/<输出目录>/shots/<开始录制的时间>/`，不放场景根（场景跟着项目文档进版本库）。图丢了不影响回放。截不了（`-nullrhi`、窗口没了）只是没有 `shot`。
- `shots` 参数：`false` 关掉；对象时覆盖 `expand`（外扩，默认 false）、`padX` / `padY`（0–5，边长的倍数）、`padXMax` / `padYMax`（像素）、`maxWidth`、`maxHeight`、`screenWidth`（0 = 不存整屏）、`gameClickBox`，写错字段名或超范围直接报错。1.4.x 及以前的 `padRatio`、`padMin`、`padMax`、`minWidth`、`minHeight` 仍收（旧面板会传）：写了 `padRatio` 就按旧算法外扩。面板在 UE 插件页 GameTest 的「设置」里改。
- **条目字段缓存**（1.6.0 起）：点在列表、格子条目上时，把这个条目的全部字段（和 `gt.keys` 同形状：`{groups, total, count, index, entries, recordedAt}`，不截断）存到 `Saved/<输出目录>/keys/<开始录制的时间>/NNN.json`，路径（相对项目根）写进 `hint.fields`。字段在按下那一刻读（1.7.3 起；之前是松开后读，点一下就重排的列表会存成别的条目），录制中 `gt.keys` 按 `step` 看时也返回这份（带 `pressed: true`）；沿用同一列表的认法、按字段拼条件也取按下时的值，按下和松开之间变了的字段不沿用。面板步骤详情「换一个标识」在游戏没开、界面不在时用它列字段、挑组合；值和命中数是录制那一刻的。文件丢了只是要开游戏再挑，回放不看它。
- 面板录完后用 OCR 从 `shot` 里认出按钮上的字，写进 `hint.ocr`（只用来显示步骤名、给 AI 参考；回放不看它，按文字找回只用 `hint.text`）。

**按钮上的字**（1.3.2 起）：`hint.text` 先收被点控件子树里的文字（`STextBlock`，以及 UMG 是 UTextBlock / URichTextBlock 的其他文字控件，富文本去掉标记）；子树里没有字时，再收上两层父面板里**叠在这个控件框上**（文字框至少 60% 在里面）的可见文字——游戏 UI 常把按钮和文字并排放进同一个 Overlay，文字是按钮的兄弟。回放按文字找回（`GameTestHintPick`）用同一套取字。字画在贴图里的按钮没有文字可取，靠截图 OCR。

一次最多录 2000 步。录完的场景是普通场景文件，可以在面板里改：把 `mark` 换成 `assert`、删掉多余的 `wait`。

## 操作（UELink `/rpc`）

全部在游戏线程执行。返回默认不超过 4000 字，读结构的（快照、报告、动作清单）不超过 12000 字；超了整份写进 `Saved/GameTest/responses/`，只回 `file`（相对项目根）和几项摘要。

| 操作 | 参数 | 返回 |
| --- | --- | --- |
| `gt.health` | — | `version`、`process`（editor / game）、`busy`、`runId`、`editor`（PIE 状态）、`worldPresent`、`playable`、`map`、`scenarioRoot`、`outputRoot`、`record`、`nav`（`{state: missing / building / ready, world}`，1.3.0 起）；装了 EnhancedInput 模块时有 `activeInjections` |
| `gt.actions` | — | `common`、`actions: [{name, params, targetRequired, editorOnly, summary, module}]`、`unavailable: [{name, module, plugin}]` |
| `gt.scenarios` | `query`、`offset`、`limit`（默认 200） | `scenarios: [{name, relativePath, _note, steps, revision, tags, parseError?}]`、`total` |
| `gt.run` | `scenario`（相对场景根）或 `steps` + `name`、`stepDelayMs`、`ignoreSkipIf`（1.7.0 起，全部照做）；批量：`tag`（场景的 `tags` 里有它的全部场景）或 `scenarios`（路径数组），可带 `name` | 单个：`runId`、`steps`、`scenario`、`finished`；批量：`batchId`、`scenarios`（个数）。都在后台跑 |
| `gt.step` | `step: {action, ...}` | 这一步的结果：`ok`、`status`、`message`、`payload` |
| `gt.job` | `steps`（只回最后几步，默认 50） | 当前 / 最近一次 run：`status`、`busy`、`total`（已完成）、`planned`、`failed`、`steps`；批量运行时多 `batch`（见下） |
| `gt.runs` | — | 本次会话最近的 run 摘要（含 `outputDir`） |
| `gt.report` | `runId` | 完整报告（每步的状态、消息、payload、耗时；跑的是场景文件时有 `scenario`；失败的步骤 `payload.evidence` 是失败证据的路径） |
| `gt.stop` | — | 取消当前 run；批量运行中时整批取消（没轮到的标 `skipped`） |
| `gt.history` | `scenario`（可省） | 按场景文件的运行历史（跨编辑器会话，每个场景最近 10 次）。给 `scenario`：`runs: [{runId, succeeded, startedAtUtc, durationMs, total, failed, failedStep?, message?, outputDir, revision?}]`，最新在前；不给：`scenarios: {路径: "pfpp…"}`（最新在前，`p` 通过、`f` 失败），列表上画历史条用 |
| `gt.config` | `config`（可省）、`dryRun` | 运行配置（场景根的 `_settings.json`，见「运行配置和阻挡界面」）。不给 `config` 是读：`{path, exists, config, settingsUiWaitTimeoutMs}`；给 `config` 是校验后整份写回（`dryRun: true` 只校验不写），另带 `message`。格式错时报哪一条错了 |
| `gt.resolve` | `targets`（选择器数组）和 / 或 `scenario`（核对它每一步的 `target`、`toTarget`） | 只解析不执行：`results: [{target, step?, field?, found, path?, class?, text?, visible?, enabled?, error?}]`（`error` 里是候选）、`checked`、`found`、`missing`、`map`。没有游戏世界时报错（这时什么都核对不了）。只看这一刻的界面：后面步骤才打开的界面现在找不到是正常的。`checks`（1.7.0 起，带 `skipIf` 的步骤数组，最多 50 个）：只判断跳过条件此刻成不成立，回 `checks: [{index, holds, say, actual?, error?}]`，和回放同一套判断 |
| `gt.inspect` | `x`、`y`（视口归一化坐标 0–1，可省，省略时用当前光标位置） | `supported`（这个进程能不能拿到处理者）、`handlerSource`、`path`（光标下的命中路径，从外到内，每项含 Slate 类型和对应的 UMG 控件名 / 类，最多 30 项）、`target`（最深的 UMG 控件写出的选择器）、`verified`（这个选择器回解析是否唯一命中它）、`hint`、`lastHandler`（最近一次鼠标按下被谁接住，描述字符串）；1.10.0 起多 `widgetClass`、`kind`（`button` / `check` / `text` / `other`）、`rect`（控件在视口里的归一化 `[x, y, w, h]`）。只读，不动光标、不发输入 |
| `gt.record.start` | `name`、`startPie`（默认 true）、`keepPace`（默认 false：UI 操作之间的间隔不录）、`shots`（录制截图，默认开，见「录制截图」）、`askKey`（`unsure` 默认 / `always` / `never`）、`askTimeoutSec`（默认 0 = 一直等）（1.4.0 起，见「列表、格子里的条目怎么认」）、`inferSkip`（`suggest` 默认 / `auto` / `off`）、`stateWindowMs`（默认 2000）（1.7.0 起，见「已达成就跳过」） | 录制状态 |
| `gt.record.mark` | `text`、`screenshot` | 录制状态 |
| `gt.record.move` | `phase`：`start` / `end` / `toggle`（默认）/ `cancel` / `here` / `near`（可带 `radius`）/ `pick`（下一次点击录成断言，再调一次取消） | 录制状态（1.3.0 起；和游戏里的工具栏、Alt+数字同一个入口） |
| `gt.record.key` | `step`（从 0 数；省略 = 正在问的那一步）+ `cond`（`Data.ItemId=1001`）或 `by`（字段数组，按当时的值拼条件）或 `mode`（`index` 只按第几个 / `text` 按位置写、回放先按文字找 / `skip` 先跳过）；`rule`（存成规则）；`all`（1.5.0 起，这次录制里同一列表已录的其他步骤也按同样的字段 / 认法改写）；`resume`（默认 true，答完继续游戏）。条件在容器里不唯一或找不到时报错。录制中随时可以传 `step` 改前面的步骤（候选在那一步的 `hint.key.cands`） | `step`、改写后的 `target`、`by`、`cond`，存了规则 `rule: true`（存不进去 `ruleError`），继续了 `resumed: true`（1.4.0 起）；`all` 时 `applied`（另外改了几步）、`skipped`（`[{step, why}]` 改不了的）（1.5.0 起） |
| `gt.keys` | `step`（录制中，用按下那一刻的站点）或 `target`（现在的界面里解析）；`filter`（按路径或值筛）、`offset` / `limit`（默认 60，最多 200） | 条目的全部字段，按来源分组：`groups: [{source, title, class?, note?, fields: [{by, src, v, hits, unique?, cond?, why?}]}]`，`total`、`count`、`offset`、`hasMore`、`nextOffset`，超长时 `truncated`，没有 Lua 时 `luaNote`；另有 `key`（同 `hint.key`）、`index`（1.4.0 起） |
| `gt.keys.preview` | `step` 或 `target` + `cond` | 在游戏画面上框出命中的条目 3 秒（唯一绿框，多个橙框）；返回 `hits`、`unique`、`includesMine`（命中里有没有点的那个）、`count`、`items: [{index, name?, offscreen?, mine?}]`（1.4.0 起） |
| `gt.record.stop` | `save`（默认 true） | `path`、`absolutePath`、`steps`、`seconds`、`preview`（前 20 步） |
| `gt.record.status` | `last`（默认 10） | `recording`、`hotkeys`、在录移动时 `moving: {meters, seconds, points}`、有提示时 `notice`、`pickArmed`、`handlerSource`（`slateDebugging`：处理者来自 Slate 调试事件，准；`hitPath`：这个进程没编 Slate 调试，按命中路径推断，录不到打字）；在录时还有 `name`、`steps`、`seconds`、`keepPace`、`last`、`askKey`、`askTimeoutSec`（不是 0 时），超过步数上限时 `truncated: true`；正在问条目标识时 `ask: {step, target, pausedSec, gamePaused, container, containerClass, entryName, index, key, shot?, screen?, text?}`（1.4.0 起） |

同一时间只跑一个 run（批量运行也是一个接一个）。每次 run 的报告和截图在 `Saved/GameTest/<名字>-<时间>/`（`report.json`、`*.png`）。

### 失败证据（1.2.0 起）

一步失败（取消不算）时自动留下三样东西，写进这次 run 产物目录的 `failures/step-<第几步>/`，报告里那一步的 `payload.evidence` 只记路径（相对项目根）：

| 字段 | 内容 |
| --- | --- |
| `dir` | 这一份证据的目录 |
| `screenshot` | `screen.png`，失败那一刻的游戏窗口（截不了时是 `screenshotError`） |
| `widgets` | `widgets.json`，那一刻屏上的控件树（结构和几何，不含属性，最多 3000 个节点）；`screens` 是屏数 |
| `target` / `selectorError` | 步骤有 `target` 时再解析一次：找到了是 `target`（路径、类、文字、`visibility`、`enabled`，说明「控件在，但没点成」）；找不到是 `selectorError`，带候选 |
| `log` | `log.txt`：先是本次 run 里 LogGameTest 的每一行（每一步的结果、每次点击送到了哪）和任何类别的警告 / 错误；再是失败前后各 50 行日志，某个类别在这段里超过 20 行（每帧打印的 Lua 日志之类）时只留它的警告和错误，其余折成一句「省略了多少行」（之后的最多等 3 秒，Stop 模式下失败后马上收尾，可能不满 50 行） |

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
| `gt.record` | 录制开始、每录一步、录移动时每秒、快捷动作改了状态、停止 | 录制状态 + `phase`（`started` / `step` / `moving` / `quick` / `ask`（暂停问条目标识，1.4.0）/ `askEnd`（问完继续）/ `key`（改了某一步的认法）/ `stopped`），`last` 只有最新一步；只有 `step` 表示多了一步，其余按 `steps` 计数判断，别把 `last` 再追加一遍 |

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
- 点击方式（`clickVia`）不是 `pointer` 的 run 用**虚拟光标**（1.2.3 起）：`input.cursor`、`drag`、`wheel`、鼠标键的 `key` 都把位置带进发给 Slate 的事件，不移系统光标，回放时可以照常用鼠标。代价：游戏里直接读系统光标位置的逻辑（`APlayerController::GetMousePosition` 瞄准）看不到虚拟光标；多帧的拖动期间如果人动了鼠标，Slate 按真实光标合成的移动可能打断拖动。需要真实光标时把 `clickVia` 设成 `pointer`，这时 `click` / `drag` / `wheel` 会移动真实光标
- 点在游戏画面上（不是界面）时，项目设置成「点击时捕获鼠标」的游戏视口会锁住鼠标，这是游戏视口自己的行为，虚拟光标也绕不开：回放时别动鼠标，游戏窗口要在前台。
- Shipping 不可用：GameTest 不进 Shipping，录制和 `gt.inspect` 用的 Slate 调试事件、控件元数据在 Shipping 里也没有。
- 没编 Slate 调试的进程（`gt.record.status` / `gt.inspect` 的 `handlerSource` 是 `hitPath`）：拿不到处理者，点击按光标下最深的控件推断（可能不如 `slateDebugging` 准），**录不到打字**；回放的 `click` 也核对不了接住按下的是不是目标。
- 纯 Slate 控件只认游戏窗口和它弹出的子窗口里的；编辑器工具窗口上的操作不录。
- 列表条目只认已经生成的：虚拟化列表里没滚到的条目没有控件；回放的 UI 动作和跳过判断会自动滚过去或翻找（见「自动滚动」），`gt.resolve`、`find`、`assert` 只看不滚，会报「没找到」，先 `scroll index=…`。`列表[序号]` 在列表顺序变化时会点错（带录制线索的步骤会按文字在同容器里找回，见「容器里顺序变了按文字找回」），能用 `列表[属性=值]` 就用它。
- `text=` 和 `hint.text` 受当前语言影响；录制优先用控件名。
- 跳过条件看的控件在列表里没滚到时，判断和 UI 动作一样自动滚、翻找（1.7.1 起，见「自动滚动」），翻到底还没有就原地等；反面条件（不存在 / 不可见）找不到就是要的结果，不翻。`textLike` 和自动推断出的文字条件同样受当前语言影响。
- 手柄和 CommonUI 的焦点导航：按键照旧录成 `key`，不按焦点控件录。
- `move` 的探路只认在地上走；坐载具、骑乘、过场动画驱动的位移录不了也走不了。项目的移动组件不读 `AddMovementInput` 时用 `drive: "keys"`。
- 录制工具栏是 UEKit「UE 里不放界面」规则的例外（用户 2026-11-17 同意），只在录制时出现。
