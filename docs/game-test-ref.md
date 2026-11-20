# GameTest 对象引用（schemaVersion 4，2.0.0 起）

这份是 GameTest 2.0 的**协议**：所有「指向游戏里某个东西」的地方只用一种写法——**对象引用 = 类 + 唯一标识组**。
GameTest（C++）、面板 UE 插件（`agent-plugins` 的 `ue/`）都按这份实现；改字段先改这里。
[game-test-script.md](game-test-script.md)（v3 的块、条件、表达式、守护、接口库）仍然有效，本文写**取代和新增**的部分；两份冲突时以本文为准。
设计取舍见项目设计稿「GameTest 对象定位与判断重设计」。

## 0. 原则

- 脚本里**永远不存** UObject、运行时生成的名字（`WBP_Item_C_12`）、路径序号（「第 6 个」）、选择器字符串。
- 只存：类（按继承认）+ 唯一标识组（属性 + 按属性类型的比较）+ 部件名（设计器里起的控件名，是类的一部分）。
- 录制、从游戏里抓、手写、Agent 写，产出同一种结构。
- C++ 只认 v4。读到 v1–v3 报 `schema_old`：「这是旧格式脚本，请在面板里用『升级脚本』转成 v4（会先备份）」。迁移由面板做（第 9 节）。

## 1. 对象引用（Ref）

```jsonc
{
  "class": "WBP_Shop_C",
  "in": "screen",
  "ident": [ { "prop": "@TxtTitle:text", "op": "eq", "value": "杂货铺" },
             { "prop": "@self.ShopId", "op": "eq", "value": 1001 } ],
  "many": "error",
  "near": null,
  "client": null
}
```

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `class` | 是 | 类名或资源路径。`U`/`A` 前缀、`_C` 后缀可省（`WBP_Shop`、`WBP_Shop_C`、`/Game/UI/WBP_Shop` 等价）。**按继承认**，子类也算。界面只认直接或间接继承 `UUserWidget` 的 |
| `in` | 否 | 在哪找。字符串：`screen`（屏上的界面，含游戏窗口弹出的子窗口）/ `world`（关卡里的 Actor）/ `all`（`TObjectIterator`，最后才用）/ `@来源.路径`（容器属性里的对象，来源同 game-test-script 4.3）。对象：`{"obj": 引用, "part": "容器部件"}`（另一个对象里某个容器控件的条目：ListView 族、Panel 子项、DynamicEntryBox、挂在 Slate 上的子界面）；`{"obj": 引用}` 且 `class` 是组件类时 = 那个 Actor 的组件。省略时按类推：UUserWidget 子类 → `screen`；AActor 子类 → `world`；子系统（`USubsystem` 子类）→ 直接取单例，`ident` 必须为空；UActorComponent 子类且没给 `in` → 校验报 `ref_need_owner` |
| `ident` | 否 | 唯一标识组，数组，**全部满足**。每条是一个判断项（第 3 节）。空 / 省略 = 只按类认 |
| `many` | 否 | 过滤后还剩多个时：`error`（默认；运行配置 `refDefaults.many` 可改）/ `first`（排序固定：`world` 里关卡放置的在前，再按创建顺序；`screen` 按 ZOrder 从小到大、同 ZOrder 先加入的在前，即最底层）/ `last`（`screen`：最上层，ZOrder 最大、同 ZOrder 后加的在上；其余：最后一个）/ `nearest`（离 `near` 最近，`near` 省略 = 本地 Pawn；只在有世界位置的候选里挑，都没有位置时报 `ref_ambiguous`） |
| `near` | 否 | `[x,y,z]` / `$var` / `@pawn` / 另一个引用 |
| `client` | 否 | 多客户端时在哪个世界找（`server` 或序号）。2.0.0 只认字段、不生效：引用一律在步骤的世界里找 |

**引用的三种写法**（凡是写引用的地方都接受）：

| 写法 | 意思 |
| --- | --- |
| `"shop"` | 场景顶层 `objects` 里的命名引用 |
| `{ "class": …, … }` | 内联引用 |
| `"$var"` | 变量里存的对象（这次运行里 `obj.find` / `saveAs` 拿到的弱引用，不落盘） |

**解析**：按 `in` + `class` 收候选 → 按 `ident` 逐条过滤 → 0 个：等（受步骤 `timeoutMs`）→ 1 个：用 → 多个：按 `many`。
`error` 时报 `ref_ambiguous`，`message` 列出每个候选（最多 8 个）的标识值和一个能区分它们的属性建议。
报告 `payload.ref: {name?, class, picked: 显示名, by: "only"/"ident"/"first"/"last"/"nearest", candidates, ident: [{prop, actual}]}`。

## 2. 部件（target）

动作落在对象里的某个控件上时，`target` 写：

```jsonc
{ "obj": "shop", "part": "Header.BtnClose", "slate": "STableRow[text=高]" }
```

| 字段 | 说明 |
| --- | --- |
| `obj` | 引用（三种写法） |
| `part` | 可选。控件在**这个类自己的** WidgetTree 里的名字；嵌套 WBP 用 `.` 进去（`Header.BtnClose`：`Header` 是嵌在里面的 UUserWidget，`BtnClose` 在 `Header` 的类自己的树里）。Actor 时是组件名。省略 = 对象本身 |
| `slate` | 可选。纯 Slate 控件（下拉选项、菜单行）：`类型[text=子串][序号]`，多段 `/`，在 `part`（或对象）的 Slate 子树和它弹出的窗口里找。这是唯一保留的文字定位，只用于没有 UObject 的 Slate 控件 |

- 运行时生成、没有设计器名字的控件**不能当 part**，它应该是一个引用（`in: {obj, part: 容器}`）。
- 名字在类的树里找不到：校验报 `part_missing`（读 WBP 资产，不用开游戏）。

`target` 也接受直接写一个引用（字符串或对象）= `{obj: 它}`。

## 3. 属性路径和判断项

### 3.1 属性路径（`prop`）

统一 `@` 开头，`.` 是**对象上存在的数据**，`:` 是**算出来的状态**。

| 写法 | 读哪里 |
| --- | --- |
| `@self.X` | 对象自己的反射属性（C++ `UPROPERTY`、蓝图变量）；后续 `.成员`、`[下标]`、`["键"]` 进结构体、对象引用、数组、Map |
| `@部件.X` | 对象里部件（第 2 节 `part` 的写法，可以多段 `@Header.TxtTitle.Text`）的属性。第一段先按部件名找，找不到再按 `self` 的属性名找（`@ShopId` = `@self.ShopId`） |
| `@lua.X` / `@部件.lua.X` | 对象 / 部件绑的 UnLua 实例表（按项目 UnLua 头文件选接口，二次开发版）。面板编辑时 Lua 字段名的下拉来自 Lua 源码的 `---@field` 声明（第 7.3 节），也可以直接填文字 |
| `@item.X` / `@itemLua.X` | ListView 族条目的数据对象 / 它的 Lua 表（只有 `in` 是 ListView 族容器时有） |
| `@部件:状态` / `:状态` | 算出状态（3.2）。`:状态` = `@self:状态` |

路径段大小写不敏感。读不到（部件不存在、属性不存在、Lua 表没有这个字段、对象引用为空）：这一项结果是 **unresolved**，按不成立算，报告写哪一段读不到。

### 3.2 算出状态（注册表，一种一个文件）

| 状态 | 适用 | 类型 | 意思 |
| --- | --- | --- | --- |
| `:exists` | 任何 | bool | 引用能解析到（对象层用；部件层 = 部件在） |
| `:onScreen` | 控件 | bool | 在视口里、整条父链可见、有尺寸 |
| `:shown` | 控件 | bool | 自己和父链都不是 Collapsed / Hidden |
| `:enabled` | 控件 | bool | 自己和父链都 enabled |
| `:text` | 文字类、按钮 | text | 显示的字（富文本去标记；按钮取里面的字，规则同录制 `hint.text`） |
| `:textKey` | 文字类 | string | FText 的本地化键 `命名空间,键`（没有键时 unresolved） |
| `:count` | 容器、ListView 族 | int | 子项 / 条目数 |
| `:index` | 列表条目 | int | 第几个（只用于显示，面板标「不建议当标识」） |
| `:selected` | 列表条目 | bool | 是否选中 |
| `:checked` | CheckBox | enum ECheckBoxState | 同 `@X.CheckedState` |
| `:image` | Image、Button | asset | 当前 Brush 的资源名 |
| `:hovered` / `:focused` | 控件 | bool | 光标在上面 / 有键盘焦点 |
| `:pos` / `:size` | 控件 | vector2 | 视口归一化位置（左上角）/ 大小 |
| `:location` / `:distance` | Actor、场景组件 | vector / float | 世界位置 / 离本地 Pawn 的距离（厘米） |
| `:hiddenInGame` | Actor、场景组件 | bool | 是否隐藏 |
| `:class` | 任何 | class | 实际类名 |

### 3.3 判断项（`{prop, op, value}`）

```jsonc
{ "prop": "@ChkAutoBuy.IsChecked", "op": "is", "value": "Checked" }
```

| 字段 | 说明 |
| --- | --- |
| `prop` | 属性路径 |
| `op` | 比较（3.4）。省略：bool 按真值，其余 `eq` |
| `value` | 常量 / `$var` / `"=表达式"`（只读，同 game-test-script 第 3 节）；`between` 是 `[a,b]`，`in` 是数组 |
| `tol` | `eq` / `near` 的容差（浮点默认 0.001，向量 10，旋转 1，颜色每通道 0.02；运行配置 `refDefaults.tol` 可改） |
| `ignoreCase` | 文字比较，默认 true（`refDefaults.ignoreCase`） |

### 3.4 比较（按属性类型，注册表；面板和 C++ 同名同表）

属性类型分类 `kind`（`gt.props` 返回、比较按它选）：

| kind | 来自 | 可用 `op` |
| --- | --- | --- |
| `bool` | bool 属性、bool 状态 | `is` |
| `int` / `float` | 整数 / 浮点、`:count` 等 | `eq` `ne` `gt` `gte` `lt` `lte` `between` `in` |
| `string` | FString、FName、Lua 字符串、`:textKey` | `eq` `ne` `contains` `startsWith` `endsWith` `matches` `in` |
| `text` | FText、`:text` | 同 `string` + `key`（`value` 是本地化键，和 `:textKey` 比） |
| `enum` | UEnum / 枚举字节属性、ECheckBoxState、ESlateVisibility | `is` `isNot` `in`（`value` 写成员名，`EX::A` 或 `A` 都认，也认整数） |
| `visibility` | ESlateVisibility（enum 的特例） | enum 的全部 + `visible`（Visible / HitTestInvisible / SelfHitTestInvisible 任一，不要 `value`）、`hidden`（Collapsed / Hidden 任一） |
| `object` | 对象引用、软引用、类引用、`:class` | `isNull` `notNull` `classIs`（按继承）`nameIs` |
| `asset` | 贴图、材质等资源引用、`:image` | `eq`（资源名或路径，任一相等）`contains` `isNull` `notNull` |
| `vector` / `rotator` / `vector2` | FVector、FRotator、FVector2D、`:pos` | `near`（`tol`）、`eq`（逐分量，带 `tol`） |
| `color` | FLinearColor、FColor、FSlateColor | `near` |
| `array` | 数组、Set、Map | `count`（配 `cmp`：`eq`/`gte`/… 和数字 `value`）`has`（含某个值）`empty` `notEmpty` |
| `struct` | 其他结构体 | 不能直接比（展开成员选）；`gt.props` 标 `comparable: false` |
| `none` | 委托、接口、原始指针 | 不能当标识也不能判断，`why` 写原因 |

Lua 值的 kind 按运行时值推：number → `float`（整数值也收 `int` 的 op）、string → `string`、boolean → `bool`、table → `array`（只有 `count`、`empty`）。

类型和 op 对不上：校验报 `op_type`（不开游戏时只查得到反射属性的类型；Lua 字段按 `---@field` 的类型查，查不到不报）。

## 4. 条件里的对象判断

game-test-script 第 4 节的统一条件里，「一条判断」新增并**取代**原来按控件写的那些：

```jsonc
{ "obj": "shop", "prop": "@ChkAutoBuy.IsChecked", "op": "is", "value": "Checked" }
{ "obj": "shop", "prop": ":exists", "op": "is", "value": false }
{ "obj": { "class": "WBP_Toast" }, "prop": "@TxtMsg:text", "op": "contains", "value": "购买成功" }
```

- 一个对象引用 + 一个判断项。`prop` 省略 = `:exists is true`。
- 反面判断（`:exists` / `:onScreen` / `:shown` 为 false）规则同 game-test-script 4.2：引用的类在屏上有实例但 `ident` 都不满足也算「不存在」。
- 删掉的写法（v4 不认，校验报 `cond_removed` 并说改成什么）：`target` + `exists` / `visible` / `enabled` / `checked` / `selected` / `text` / `textContains` / `textLike` / `textMatches` / `image` / `count` / `number` / `field` / `onScreen`。保留：`value: "$var"` / `value: "@来源.路径"`（取值来源，不是对象）、`api`、`expr`、`alias`、`actorNear`、`all` / `any` / `not`、`settleMs` / `elseSettleMs`。
- 表达式里新增 `prop(引用, "属性路径")`（只读），删掉 `text(sel)` 等按选择器的内置函数（`expr_removed`）。

## 5. 动作

### 5.1 `target` 改成部件写法

`click` `drag`（`toTarget` 同）`wheel` `type` `select` `scroll` `set` `key`（给 target 时）`probe` `snapshot` `screenshot`（给 target 时）`stack.push` / `stack.pop`、`loop each`（容器：`each: {obj, part}`，条目在 `$item`，`$item` 是引用，`{"obj": "$item", "part": "BtnClaim"}`）：`target` 一律是第 2 节的写法。字符串选择器报 `selector_removed`。

`skipIf` 改成 `{cond, also?, settleMs?, verify?, verifyTimeoutMs?}`：条件写进 `cond`（第 4 节写法或表达式字符串），旧的直接写在 skipIf 上的 `checked` / `textLike` 等报 `cond_removed`；求值时引用找不到按「还不能判断」再等，不再自动把没生成的列表条目滚出来。

`wait until=widget/widgetGone` 删掉，用 `wait until=cond`。`find` 改名 `obj.find`。`widget.add` 不变（`class`）、`widget.remove` 的 `target` 用第 2 节写法。

### 5.2 `obj.close`

```jsonc
{ "action": "obj.close", "obj": "shop",
  "how": [ { "by": "call", "function": "CloseShop", "args": { "bPlaySound": false } },
           { "by": "click", "part": "Header.BtnClose" } ],
  "verify": "gone", "verifyMs": 2000 }
```

| `by` | 字段 | 做什么 | 适用 |
| --- | --- | --- | --- |
| `call` | `function`, `args` | 调对象（或 `part`）上的反射函数，参数规则同 game-test-script 第 6 节 | 任何 |
| `lua` | `function`, `args`（数组） | 调对象绑的 Lua 表上的函数（`allowLuaInvoke`） | 有 UnLua 绑定 |
| `click` | `part` | 用代码点部件（和 `click` 动作 `via: "code"` 同一套：按钮的 OnClicked 等）。2.0.0 只做代码点击，不收 `via` | 界面 |
| `visibility` | `value`（`Collapsed` 默认 / `Hidden`），`part?` | `SetVisibility` | 控件 |
| `remove` | — | `RemoveFromParent` | 界面 |
| `pop` | — | CommonUI：`DeactivateWidget`；在层级容器里时弹出（GameTestCommonUI 模块注册，没装时报 `close_unavailable`） | CommonUI 可激活控件 |
| `key` | `key`（默认 `Escape`） | 按键发给游戏视口 | 任何 |
| `destroy` | — | `AActor::Destroy`（运行配置 `allowDestroy: true` 才可用） | Actor |

- `how` 按顺序试：一种做完按 `verify` 核对，没过就试下一种；全没过这一步失败（`close_failed`），报告 `payload.close: [{by, ok, ms, message?}]`，用了推荐 / 规则时带 `payload.closeFrom`（`how` / `rule` / `default`）。每种关闭方式本身是同步的一次调用，跨帧的只有核对。
- `verify`：`gone`（默认，引用找不到）/ `hidden`（`:onScreen` 为 false）/ `none`。`verifyMs` 默认 2000（`refDefaults.closeVerifyMs`）。
- `how` 省略：用运行配置 `closeRules` 里这个类（按继承，最近的规则优先）的 `how`；没有规则按默认推荐：类上有名字像 `Close` / `Hide` / `Back` / `Exit` / `Dismiss` 的无参可调函数 → `call`；有名字像 `BtnClose` / `CloseBtn` / `Close` 的按钮部件 → `click`；是 CommonUI 可激活控件 → `pop`；最后 `remove`。
- 守护 `do` 里常用。旧的 `widget.close` / `widget.call` 删掉（`action_removed`）。

### 5.3 其他对象动作

| 动作 | 字段 | 说明 |
| --- | --- | --- |
| `obj.set` | `obj`, `prop`, `value` | 设属性。控件状态走控件自己的 setter 让委托照常触发：`Visibility` → `SetVisibility`、CheckBox `IsChecked`/`CheckedState` → `SetIsChecked`/`SetCheckedState`、`Text` → `SetText`、Slider `Value` → `SetValue`、`IsEnabled` → `SetIsEnabled`；其余反射写入（`ImportText` / JSON 转换）或 Lua 字段赋值。只读、`none` 类的报 `prop_readonly` |
| `obj.call` | `obj`, `part?`, `function`, `args`, `saveAs`, `expect` | 取代 `invoke`（`object` 字段改 `obj`；`obj` 也可以是 game-test-script 4.3 的来源字符串 `@sub.X` / `@luaMod.X`；静态函数省略 `obj`，`function` 写 `类名.函数`） |
| `obj.find` | `obj`, `saveAs` | 找到后把对象存进变量（这次运行里的弱引用） |
| `obj.wait` | `obj`, `prop?`, `op?`, `value?` | `wait until=cond` 的简写：等到这个判断成立 |

`invoke`、`widget.close`、`widget.call`、`find` 在 v4 不认（`action_removed`）。

## 6. 场景和运行配置

```jsonc
{
  "kind": "gametest-scenario", "schemaVersion": 4, "name": "杂货铺",
  "objects": { "shop": { "class": "WBP_Shop", "ident": [ … ] }, "bag": { "class": "WBP_Bag" } },
  "steps": [ { "id": "s1", "action": "click", "target": { "obj": "shop", "part": "BtnBuy" } },
             { "id": "s2", "action": "obj.close", "obj": "shop" } ]
}
```

- `objects` 的值是引用（只能对象形式，不能再引用别的名字，`in.obj` 可以引用别的名字但不能成环：`ref_cycle`）。
- `_settings.json` 新增，取代 `itemKeys` 和 `objectKeys`（读到旧字段报 `settings_old`，面板升级时一起迁移）：

| 字段 | 说明 |
| --- | --- |
| `identRules` | `[{class, in?, ident: [prop…], note?}]`：录制、抓取、Agent 找这个类（按继承）的对象时默认用这些属性当标识。`in` 省略 = 在哪都按它 |
| `closeRules` | `[{class, how: […], verify?}]`：`obj.close` 省略 `how` 时用 |
| `refDefaults` | `{many: "error", ignoreCase: true, tol: {float: 0.001, vector: 10, rotator: 1, color: 0.02}, closeVerifyMs: 2000}` |
| `allowDestroy` | 默认 false |

## 7. 新增操作（`/rpc`）

### 7.1 `gt.tree`：此刻游戏页面上的 UMG 树（类似控件反射器，只列 UMG）

参数：`root?`（节点 id，只要这个子树）、`depth?`（默认 不限）、`filter?`（类名 / 控件名子串）、`onlyVisible?`（默认 false）、`limit?`（默认 1500 个节点，最多 5000）。

返回：

```jsonc
{ "frame": 17, "nodes": [
  { "id": "n12", "parent": "n3", "depth": 2, "class": "TextBlock", "name": "TxtTitle",
    "user": false, "owner": "n3", "ownerClass": "WBP_Shop_C", "part": "TxtTitle",
    "visible": true, "onScreen": true, "rect": [0.31, 0.12, 0.2, 0.04], "text": "杂货铺",
    "container": null, "entry": null }
], "total": 238, "truncated": false }
```

- 只认 `UWidget`（UMG）：从每个游戏视口里的根 UUserWidget（和游戏窗口弹出的子窗口里的）开始，走 `WidgetTree` 和 ListView 族生成出来的条目控件；纯 Slate 不列。
- `id` 是本次会话的临时句柄（`gt.tree` 每次调用重新编号，引擎对象没了就失效），**只用于 `gt.highlight` / `gt.props` / `gt.refFor`，不能存进脚本**。
- `user`：是 UUserWidget（可以当对象）；`owner` / `ownerClass`：它所在的最近的 UUserWidget；`part`：它在 owner 类树里的部件写法（运行时生成的为空）。
- `container`：是容器时 `{kind: "list"/"panel"/"entrybox", entryClass?, count}`；`entry`：是容器条目时 `{container: 容器节点 id, index}`。
- `text`：文字类控件的显示字（截 60 字）。

### 7.2 `gt.highlight`：在游戏画面上描边

参数：`node`（`gt.tree` 的 id）或 `target`（第 2 节写法，解析到的全部描出来，多个时编号）；`color?`（默认主色橙 `#d97757`）；`ms?`（默认 0 = 一直描到下一次 `gt.highlight` 或 `clear`）；`clear: true` 清掉。

- 在游戏视口上画一层不接输入的 Slate 覆盖层（框 + 左上角类名标签），不改游戏的控件。2.0.0 的覆盖层加在游戏视口里，截图（`screenshot`、`gt.pick` 的画面）时还没有临时藏起来：截图前先 `clear`。
- 面板在树里悬停 / 选中节点时调；面板关掉抓取窗口时 `clear`。租约同 `gt.pick`：60 秒没再调就自动清。

### 7.3 `gt.props`：一个对象的属性树（标识和判断共用）

参数三选一：`node`（`gt.tree` id，游戏开着）/ `target`（第 2 节写法，游戏开着）/ `class`（只按类，不开游戏也行：读 WBP 资产和反射）；可选 `part`（只看这个部件）、`filter`、`offset` / `limit`（默认 120 项，最多 400）、`siblings`（默认 true：算「同类里」那一列）。

返回：

```jsonc
{ "class": "WBP_Shop_C", "luaModule": "UI.Shop.ShopPanel", "in": "screen",
  "siblings": 3,
  "groups": [
    { "id": "self", "title": "自己", "items": [
      { "prop": "@self.ShopId", "name": "ShopId", "src": "cpp", "type": "int32", "kind": "int",
        "value": 1001, "distinct": 3, "changing": false, "score": 9, "ops": ["eq", "ne", "gt", "gte", "lt", "lte", "between", "in"] } ] },
    { "id": "part:TxtTitle", "title": "TxtTitle", "class": "TextBlock", "part": "TxtTitle", "items": [ … ] },
    { "id": "part:Header", "title": "Header", "class": "WBP_PanelHeader_C", "part": "Header", "user": true, "collapsed": true, "items": [] },
    { "id": "item", "title": "条目的数据对象", "class": "UBagItemData", "items": [ … ] },
    { "id": "states", "title": "算出状态", "items": [ … ] }
  ],
  "suggest": [ "@TxtTitle:textKey", "@self.ShopId" ],
  "matches": 1, "total": 64, "hasMore": false, "truncated": false }
```

- 组顺序：`self`（C++ → 蓝图 → Lua，`src` 标出）、`item`（条目时）、各部件（WidgetTree 顺序；嵌套 UUserWidget 给 `user: true` 和 `collapsed: true`，`items` 空，面板展开时再用 `part` 调一次）、`states`。
- 每项：`prop`（直接能写进脚本的路径）、`name`、`src`（`cpp` / `bp` / `lua` / `state`）、`type`（引擎类型名）、`kind`（3.4）、`value?`（游戏开着时，超长截 200 字）、`distinct?`（同类 `siblings` 个实例里有几种值；`siblings` 是同 `in` 下同类的实例数，最多看 50 个）、`members?`（enum 和 visibility：全部成员名，面板给下拉）、`score`（0–10，当标识的推荐分，同录制条目打分：唯一、像 Id、数据对象上分高；位置、选中、计数、长文字、会变的分低；`changing`「1 秒内自己变过」2.0.0 没做，会变的只按名字和状态的 `IsStable` 压分）、`ops`、`comparable`（false 时带 `why`）、`readOnly`。
- `suggest`：能把当前对象和同类其他实例区分开的最少属性组合（最多 3 个），按 `identRules` 先、再按分数。
- `matches`：按 `suggest` 组成的 `ident` 此刻命中几个。
- Lua 组：游戏开着时由实例表给（`src: "lua"`）；面板再用 `---@field` 声明补全（见 7.5）。
- 最多 12000 字，超出截断、给 `nextOffset`。

### 7.4 `gt.refFor`：从游戏里点到的东西出引用

参数：`node`（`gt.tree` id）或 `x` / `y`（视口归一化坐标，同 `gt.pick at`）；`level?`（命中链里第几层当对象，默认自动）。

返回：`chain: [{node, class, kind: "entry"/"widget"/"actor"/"component", part?, note}]`（从点到的往外：条目 → 界面 …；`part` 是该层能用的部件写法）、`ref`（推荐的引用，`ident` 按 `identRules` 和打分挑）、`target`（`{obj: ref, part}` 当点到的是部件时）、`matches`、`ask`（拿不准时 true：没有分数够的组合，面板应弹属性树让人挑）。
`gt.pick action=at` 的返回里也带同样的 `chain` / `ref` / `target`（2.0.0 起）。

### 7.5 Lua 字段的来源（面板侧）

- 面板按对象的 `luaModule`（`gt.props` / `gt.widgets` 给）找到 Lua 源文件，用已有的 `lua-parse` 读 `---@class` 下的 `---@field 名 类型 说明`（也收 `self.x = …`、模块表字段），给 `@lua.` 后面的字段名做**可搜索下拉**；同时允许直接填文字（下拉里没有的也收）。
- 类型按 `---@field` 写的类型映射到 3.4 的 kind（`number`/`integer` → `float`/`int`，`string`，`boolean` → `bool`，`table`/`X[]` → `array`，其他 → 按运行时值）。

### 7.6 变化

| 操作 | 变化 |
| --- | --- |
| `gt.resolve` | `conds` 里接受第 4 节写法；新增 `refs: [{id?, ref, show?: [prop…]}]`（可同时给场景的 `objects`，`ref` 才能写名字）→ `refs: [{id, count, total, candidates: [{name, match, ident: [{prop, actual}]}]}]`（最多 8 个），`show` 里的属性按候选给实际值放进 `ident`，面板「命中几个」用它（配 `gt.watch` 推送）。`targets`（部件写法数组）和 `scenario` 核对改按引用解析，字符串选择器报 `selector_removed` |
| `gt.vars` | `scope: "object"` 的参数从定位器改成引用：`{scope: "object", ref}`（写 `locator` 报错）；其余不变 |
| `gt.validate` | 按本文校验；新错误码见第 10 节。`deep: true` 的选择器检查换成引用检查：类在不在、`part` 第一段在不在 WBP 控件树里（`part_missing`）、`@self.X` 在类上有没有（`prop_missing`）、op 和属性类型对不对（`op_type`）；`checked.selectors` 改名 `checked.refs` |
| `gt.record.*` | 录出来就是 v4（第 8 节）。暂停问标识（`gt.record { phase: "ask" }` 和 `gt.record.status` 的 `ask`）带 `ref`（条目的引用）、`node`（`gt.tree` 的 id，调过 `gt.tree` 才有）、`target`（`{obj, part}`，界面引用写名字时同时给 `objects`）；原来的选择器文本挪到 `selector`。`gt.record.key` 的回答：`{step?, resume?, ident: [{prop, op: "eq", value}], rule?: true（写进 identRules）, all?: true（同一列表后面的条目都按它认）}`，先跳过 `{mode: "skip"}`；空 `ident` = 只按第几个认。录制时挑标识只认 `eq` 和条目上的字段（`@item.` / `@self.` / `@lua.` / `@itemLua.`、`:text`） |
| `gt.inspect` | 多 `chain` / `ref` / `target` / `matches` / `ask`（同 `gt.refFor`），`hint` 保留 |
| `gt.keys` | `target` 改成部件写法；返回的字段带 `prop`（能直接写进 `ident` 的路径，行列号没有），其余不变 |
| `gt.actions` | 按本文列；另给 `compares`（`[{op, kinds, verb, summary, noValue?}]`）、`states`（`[{prop, kind, on, summary, changing?}]`）、`closers`（`[{by, summary, fields}]`），面板核对自己的表 |
| `gt.config` | 读写都带 `identRules` / `closeRules` / `refDefaults` / `allowDestroy`（写回时只改认得的字段，别的原样留着） |
| `gt.props` | 7.3；`class` 模式用类的默认对象和 WBP 生成类的设计器树，不开游戏也行（只给结构，不给值） |

## 8. 录制

- 点击：接住输入的控件 → 最近的 UUserWidget 当对象，控件在它类树里的名字当 `part`（运行时生成的往外找能写的）；对象是容器条目时 `in: {obj: 父对象, part: 容器}`。对象引用的 `ident`：同类此刻只有一个 → 空；有 `identRules` → 按规则；否则按打分挑最少的能区分的组合；挑不准按原来的 `askKey` 规则暂停问人（`gt.record { phase: "ask" }`），面板弹属性树。
- 同一个对象在一次录制里多次出现：引用去重，写进 `objects`（名字按类名去掉前缀：`shop`、`bagSlot`，重名加序号），步骤里写名字。条目对象的标识值不同时各自内联，不去重。
- Ctrl+Alt 点 / Alt+4：录 `wait until=cond`/`assert`，判断项默认：文字类 `:text eq 此刻`、其余 `:onScreen is true`；面板录完可以改（属性树）。
- `hint` 不变（截图、OCR、字段缓存只用于显示和换标识）。
- 2.0.0 的做法：录制期间内部仍按选择器文本记目标（挑标识、问人、改方括号都按它），**存盘时**统一改写成 v4（`Record/V4/GameTestRecordV4`，规则同第 9 节的迁移表）；改写不准的步骤写 `todo`，`gt.record.stop` 返回 `todo: 步数`。所以录制中 `gt.record.status` 给的步骤预览仍是选择器写法。界面对象的 `ident`：同类只有一个时不写（录制时同一界面一般只开一份），容器条目按挑好的标识；「按打分挑最少的能区分的组合」只用在条目上，界面本身不挑。

## 9. 迁移 v3 → v4（面板做）

面板「升级脚本」：读 v1–v3 → 改写 → 原文件存 `.v3.bak`（已存在加序号）→ 写回。`_settings.json` 一起迁移（`itemKeys` / `objectKeys` → `identRules`，`.bak` 同理）。迁不准的地方写进步骤的 `todo: "说明"`，面板标「待定」，校验报 warning `migrate_todo`，运行时照跑（引用 `many: "first"`）。

| v3 | v4 |
| --- | --- |
| `WBP_X:Root/A/B` | `{obj: {class: WBP_X}, part: "B"}`（只留最后一段控件名） |
| `WBP_X:.` | `{obj: {class: WBP_X}}` |
| `A>B` | `part: "A.B"`（A 是嵌套 WBP 的部件名） |
| `容器[字段=值]>B` | 条目引用 `{class: 条目类?, in: {obj, part: 容器}, ident: [{prop: "@item.字段"（ListView）/ "@self.字段", op: "eq", value}]}` + `part: B`。条目类不知道时写 `class: "UserWidget"` + todo |
| `@句柄` | `$句柄`（`find` → `obj.find saveAs`） |
| `列表[n]`、`#类名#n`、`text=…`、无屏名路径、`>>Slate` 前没有 UMG | 尽量从 `hint.key` / `hint.fields` 挑标识；挑不出 `many: "first"` + todo |
| `>>Slate段` | `slate: "Slate段"` |
| `objects.X {class, in, key, component, near, client}` | `{class, in, ident: [{prop: "@self.路径" 或原 `@lua.`, op: "eq", value}], near, many: near ? "nearest" : (key 空 ? "first" : "error")}`；`component` → 另起组件引用 |
| 条件 `target` + `visible`/`exists`/`enabled`/`checked`/`selected`/`onScreen` | `{obj, prop: ":shown"/":exists"/":enabled"/":checked"/":selected"/":onScreen", op: "is", value}` |
| `text` / `textContains` / `textLike` / `textMatches` | `:text` + `eq` / `contains` / `contains`+todo / `matches` |
| `number: {gt: 3}` | `:text` 不能直接比数字 → `expr: "num(prop(引用, ':text')) > 3"` |
| `count` / `image` / `field: "@self.X"` | `:count` / `:image` / `@self.X` |
| `widget.close` / `widget.call` | `obj.close how: [{by: "remove"}]` / `[{by: "call", function}]` |
| `invoke` / `find` | `obj.call` / `obj.find` |
| `wait until=widget` / `widgetGone` | `wait until=cond` + `:exists is true/false` |
| 表达式 `text(sel)` 等 | `prop(引用, ":text")` 等；改不了的 todo |

## 10. 错误码（新增）

`schema_old`、`settings_old`、`ref_need_owner`、`ref_cycle`、`ref_ambiguous`、`ref_not_found`、`ref_bad_class`、`part_missing`、`prop_missing`、`prop_readonly`、`op_type`、`op_unknown`、`cond_removed`、`expr_removed`、`selector_removed`、`action_removed`、`close_unavailable`、`close_failed`、`migrate_todo`（warning）。
`object_not_found` / `object_ambiguous` 改名 `ref_not_found` / `ref_ambiguous`。

## 11. 代码结构（扩展点，接 game-test-script 第 14 节）

| 扩展点 | 接口 | 注册表 | 一个实现一个文件 |
| --- | --- | --- | --- |
| 算出状态（`:onScreen` …） | `IGameTestState`：名字、适用（控件 / Actor / 任何）、kind、`Read(UObject*, ctx) → FGameTestValue` | `FGameTestStateRegistry` | `Observe/Ref/States/GameTestState<名字>.cpp` |
| 比较（`eq` `contains` `visible` …） | `IGameTestCompare`：名字、适用 kind、`Test(actual, item) → {holds, why}` | `FGameTestCompareRegistry` | `Observe/Compare/GameTestCompare<分类>.cpp` |
| 关闭方式（`call` `click` `pop` …） | `IGameTestCloser`：名字、字段、`Validate`、`Close`（同步做一次，跨帧核对由 `obj.close` 统一做）、`Recommend` / 推荐顺序 | `GameTestClosers::Get()` | `Operate/Close/GameTestClose<名字>.cpp`（`pop` 在 GameTestCommonUI） |
| 属性 kind 分类 | `FGameTestPropKind::Of(FProperty*)` | — | `Observe/Ref/GameTestPropKind.cpp` |

面板：`shared/gt-script/ref/`（类型、校验、人话描述、比较表、迁移）、`web/sections/uekit/gt-ref/`（`PropTree`、`RefCard`、`CompareEditor`、`RefDialog`、`WidgetTreePick`、`CloseHowList`）、`gt-cond/checks/prop.tsx`。比较表在面板和 C++ 各一份，名字和适用 kind 必须一致（`shared/gt-script/ref/compare.ts` 的单测核对本文第 3.4 节表格）。

## 12. 版本

- GameTest 2.0.0：本文全部（实现和本文不同的地方已在对应小节写明：`obj.close` 的 `click` 只做代码点击、关闭方式同步做一次再跨帧核对、`gt.highlight` 不避开截图、`gt.props` 没有 `changing`、`gt.tree` 和引用的 `client` 没做、skipIf 不再自动把没生成的列表条目滚出来）。需要 UELink ≥ 2.12.0（不变）。
- 面板 UE 插件按 `gt.health.version` 判断：低于 2.0.0 时编辑页只读、提示升级 GameTest；脚本是 v3 时提示「升级脚本」。
