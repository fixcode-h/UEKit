# GameTest 对象引用（schemaVersion 5，2.1.0 起）

这份是 GameTest 2.x 的**协议**：所有「指向游戏里某个东西」的地方只用一种写法——**对象引用 = 类 + 唯一标识组**。
GameTest（C++）、面板 UE 插件（`agent-plugins` 的 `ue/`）都按这份实现；改字段先改这里。
[game-test-script.md](game-test-script.md)（v3 的块、条件、表达式、守护、接口库）仍然有效，本文写**取代和新增**的部分；两份冲突时以本文为准。
设计取舍见项目设计稿「GameTest 对象定位与判断重设计」。

## 0. 原则

- 脚本里**永远不存** UObject、运行时生成的名字（`WBP_Item_C_12`）、路径序号（「第 6 个」）、选择器字符串。
- 只存：类（按继承认）+ 唯一标识组（属性 + 按属性类型的比较）+ 部件名（设计器里起的控件名，是类的一部分）。
- 录制、从游戏里抓、手写、Agent 写，产出同一种结构。
- C++ 只认 schemaVersion 5（2.1.0 起；v5 和 v4 的差别是属性路径，见 3.1）。读到其他版本报 `schema_old`：「旧格式不再支持，请按新写法重写」。**不做迁移**（2.0 的「升级脚本」已删）。

## 1. 对象引用（Ref）

```jsonc
{
  "class": "WBP_Shop_C",
  "in": "screen",
  "ident": [ { "prop": "@/TxtTitle:text", "op": "eq", "value": "杂货铺" },
             { "prop": "@.ShopId", "op": "eq", "value": 1001 } ],
  "many": "error",
  "near": null,
  "client": null
}
```

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `class` | 是 | 类名或资源路径。`U`/`A` 前缀、`_C` 后缀可省（`WBP_Shop`、`WBP_Shop_C`、`/Game/UI/WBP_Shop` 等价）。**按继承认**，子类也算。界面只认直接或间接继承 `UUserWidget` 的。2.1.0 起也可以写 `lua:模块名`（`lua:UI.ShopPanel`，require 用的名字）= 绑定的 Lua 模块是它或它的子类（沿元表 `__index` 的 Lua 继承链）的 UObject；模块没加载（`package.loaded` 里没有）报 `lua_class_missing`，项目没有 UnLua 也报它 |
| `in` | 否 | 在哪找。字符串：`screen`（屏上的界面，含游戏窗口弹出的子窗口）/ `world`（关卡里的 Actor）/ `lua`（2.1.0：绑了 UnLua 的 UObject，经 UELink `lua.objects`，不含类本身和已标记销毁的）/ `all`（`TObjectIterator`，最后才用）/ 前三个的数组（2.1.0：`["screen", "lua"]` = 几处的并集，同一个对象只算一次、按数组顺序排；数组里有 `all` 或是空数组 = `all`；面板「都不选」写 `all`）/ `@来源.路径`（容器属性里的对象，来源同 game-test-script 4.3）。对象：`{"obj": 引用, "part": "容器部件"}`（另一个对象里某个容器控件的条目：ListView 族、Panel 子项、DynamicEntryBox、挂在 Slate 上的子界面）；`{"obj": 引用}` 且 `class` 是组件类时 = 那个 Actor 的组件。省略时按类推：`lua:` 类 → `lua`；UUserWidget 子类 → `screen`；AActor 子类 → `world`；子系统（`USubsystem` 子类）→ 直接取单例，`ident` 必须为空；UActorComponent 子类且没给 `in` → 校验报 `ref_need_owner` |
| `ident` | 否 | 唯一标识，数组，**每项都要满足**。每项是一个判断项（第 3 节），或一个条件组（3.5，2.1.6 起）。空 / 省略 = 只按类认 |
| `many` | 否 | 过滤后还剩多个时：`error`（默认；运行配置 `refDefaults.many` 可改）/ `first`（排序固定：`world` 里关卡放置的在前，再按创建顺序；`screen` 按 ZOrder 从小到大、同 ZOrder 先加入的在前，即最底层）/ `last`（`screen`：最上层，ZOrder 最大、同 ZOrder 后加的在上；其余：最后一个）/ `nearest`（离 `near` 最近，`near` 省略 = 本地 Pawn；只在有世界位置的候选里挑，都没有位置时报 `ref_ambiguous`） |
| `near` | 否 | `[x,y,z]` / `$var` / `@pawn` / 另一个引用 |
| `client` | 否 | 多客户端时在哪个世界找（`server` 或序号）。2.0.0 只认字段、不生效：引用一律在步骤的世界里找 |

**引用的三种写法**（凡是写引用的地方都接受）：

| 写法 | 意思 |
| --- | --- |
| `"shop"` | 场景顶层 `objects` 里的命名引用 |
| `{ "class": …, … }` | 内联引用 |
| `"$var"` | 变量里存的对象（这次运行里 `obj.find` / `saveAs` 拿到的弱引用，不落盘） |
| `{"$obj": "shop"}` | 同 `"shop"`（2.1.0 起，和函数参数里的对象写法一致） |

`objects` 的名字 2.1.0 起可以用中文（字母、数字、下划线、中文，不以数字开头）。

**解析**：按 `in` + `class` 收候选 → 按 `ident` 逐条过滤 → 0 个：等（受步骤 `timeoutMs`）→ 1 个：用 → 多个：按 `many`。
`error` 时报 `ref_ambiguous`，`message` 列出每个候选（最多 8 个）的标识值和一个能区分它们的属性建议。
报告 `payload.ref: {name?, class, picked: 显示名, by: "only"/"ident"/"first"/"last"/"nearest", candidates, ident: [{prop, actual}]}`；`obj.call each` 时 `by: "each"`、`count`（调了几个）。
`lua:` 类每次解析问一次 UELink（`lua.objects` 一次 + 每个用到的模块 `lua.inspect` 一次），比按 UE 类找慢，能写 UE 类时优先写 UE 类。

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

- `part` / `slate`（以及条件、判断项的 `prop`，如 `@/Claim_$what:text`）里可以嵌 `$变量`，运行时按文字替换（规则同 game-test-script 1.1 节；被 `call` 的子脚本按参数点不同的部件：`"part": "$nav"`）；深度检查跳过带 `$` 的段。
- 运行时生成、没有设计器名字的控件**不能当 part**，它应该是一个引用（`in: {obj, part: 容器}`）。
- 名字在类的树里找不到：校验报 `part_missing`（读 WBP 资产，不用开游戏）。

`target` 也接受直接写一个引用（字符串或对象）= `{obj: 它}`。

## 3. 属性路径和判断项

### 3.1 属性路径（`prop`，v5，2.1.0 起）

从 `@`（对象自己）或 `@item`（列表条目的数据对象）出发，**每段一个符号**：

| 写法 | 意思 | 位置 |
| --- | --- | --- |
| `@` | 对象自己 | 开头 |
| `@item` | 列表条目的数据对象（只有 `in` 是 ListView 族容器时有；条目控件上没给时从 `IUserObjectListEntry` 取） | 开头 |
| `/名字` | 子控件（这个类自己的 WidgetTree 里的设计器名，嵌套 WBP 再 `/` 进去）或 Actor 组件 | 只能在属性前面，可以连续多段 |
| `.名字` | 反射属性（C++ `UPROPERTY`、蓝图变量、结构体字段；对象引用会跟进去） | 任意 |
| `.名字()` / `.名字(1, "a")` | 有返回值的取值函数：只许 `BlueprintPure` 或 `const`；只读，回放时每帧调。2.1.6 起括号里可以写参数，只能是常量（数字、`"文字"` / `'文字'`、`true` / `false` / `null`，逗号分开，按参数顺序；没写的参数用零值，多写报错；WorldContext 参数自动填、不用写） | 落在对象上时 |
| `#名字` | 绑的 UnLua 实例表字段；`#a.b`、`#a[1]` 里后面的 `.b`、`[1]` 继续是 Lua 表的键（后面不能再写 `()` 或 `:状态`） | 任意（落在对象上时） |
| `:名字` | 运行状态（3.2 注册表） | 只在最后 |
| `[i]` / `["键"]` | 数组下标（从 0 起）/ Map 键 | 跟在属性后面 |

例：`@/Header/TxtTitle.Text`、`@.ShopId`、`@/ItemList:selectedCount`、`@#shopName`、`@/ShopComp.Goods[0].Id`、`@:text`、`@item.Data.ItemId`、`@item#cfgId`、`@.GetLevel()`、`@.GetCount(1001)`。

- 名字是 ASCII 标识符（字母、数字、下划线，不以数字开头），大小写不敏感；名字里可以嵌 `$变量`（`@/Claim_$what:text`），解析时当名字的一部分，运行时先按文字替换再读。解析规则和面板 `ue/shared/gt-script/ref/path.ts`（`parsePropPath` / `formatPropPath`）一致，改一边要改另一边。
- 写法不对报 `path_syntax`，消息写第几个字符（从 1 数）和原因。
- 旧写法（v4：`@self.X`、`@部件.X`、`@lua.X`、`@itemLua.X`、`@部件:状态`、开头的 `:状态`）报 `path_old`，消息里给新写法（`@self.X` → `@.X`，`@lua.X` → `@#X`，`@itemLua.X` → `@item#X`，`:text` → `@:text`；`@部件.X` 按部件写 `@/部件.X`、按属性写 `@.部件.X`）。
- 读不到（部件不存在、属性不存在、Lua 表没有这个字段、对象引用为空、函数不能调）：这一项结果是 **unresolved**，按不成立算，报告写哪一段读不到。例外：`@/部件:exists` 在部件不存在时是 false（不是 unresolved）。
- 深度检查（`gt.validate deep`）按段对类：第一个 `/` 段在 WBP 控件树里（`part_missing`），紧跟在对象上的 `.` 段是类上的属性（`prop_missing`）或可调的取值函数（`fn_not_callable`），`:` 段在状态注册表里。

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
| `:selectedIndex` | ComboBoxString | int | 选中第几项（从 0 起，没选 -1） |
| `:selectedCount` | ListView 族 | int | 选中了几条 |
| `:scrollOffset` | ScrollBox | float | 当前滚动位置（下一帧布局后才变） |
| `:open` | MenuAnchor | bool | 菜单开着没有 |
| `:selectedKey` | InputKeySelector | string | 选的键名（如 `SpaceBar`） |
| `:activeIndex` | WidgetSwitcher | int | 当前显示第几页 |
| `:expanded` | ExpandableArea | bool | 展开没有 |

### 3.3 判断项（`{prop, op, value}`）

```jsonc
{ "prop": "@/ChkAutoBuy.IsChecked", "op": "is", "value": "Checked" }
```

| 字段 | 说明 |
| --- | --- |
| `prop` | 属性路径 |
| `op` | 比较（3.4）。省略：bool 按真值，其余 `eq` |
| `value` | 常量 / `$var` / `"=表达式"`（只读，同 game-test-script 第 3 节）；`between` 是 `[a,b]`，`in` 是数组 |
| `tol` | `eq` / `near` 的容差（浮点默认 0.001，向量 10，旋转 1，颜色每通道 0.02；运行配置 `refDefaults.tol` 可改） |
| `ignoreCase` | 文字比较，默认 true（`refDefaults.ignoreCase`） |
| `not` | 取反（2.1.6，只在引用的 `ident` 里认，条件里的对象判断不认） |

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

### 3.5 条件组（2.1.6）

`ident` 顶层的各项都要满足（条件组之间是「且」）；一项也可以是条件组，组里的项按 `all`（都满足）或 `any`（任一满足）合起来，项可以是判断项或子组，**最多套 4 层**；判断项和条件组都能写 `not: true` 取反。

```jsonc
"ident": [
  { "all": [ { "prop": "@.ItemId", "op": "eq", "value": 1001 },
             { "any": [ { "prop": "@.GetCount()", "op": "gt", "value": 3 },
                        { "prop": "@.IsSoldOut()", "not": true } ] } ] },
  { "any": [ { "prop": "@:onScreen" }, { "prop": "@/Header/TxtTitle:text", "op": "contains", "value": "杂货" } ] }
]
```

- 条件组只写 `all` 或 `any` 其中一个（两个都写报 `bad_type`），数组不能为空；字段只认 `all` / `any` / `not`。
- 旧的平铺写法 `[判断项, 判断项]` 就是「每项都满足」，照样认；只有一项、`all`、不取反的组和直接写那一项等价，面板存的时候会拆掉这一层。
- 匹配时组里按顺序算、能短路就短路；报告的 `payload.ref.ident`、歧义说明、`gt.resolve` 的 `candidates[].ident` 把条件组展开成判断项，每个 `prop` 一条此刻值。
- 面板「每组命中几个」= 只拿这一组当 `ident` 再发一次 `gt.resolve`，不另加操作。

## 4. 条件里的对象判断

game-test-script 第 4 节的统一条件里，「一条判断」新增并**取代**原来按控件写的那些：

```jsonc
{ "obj": "shop", "prop": "@/ChkAutoBuy.IsChecked", "op": "is", "value": "Checked" }
{ "obj": "shop", "prop": "@:exists", "op": "is", "value": false }
{ "obj": { "class": "WBP_Toast" }, "prop": "@/TxtMsg:text", "op": "contains", "value": "购买成功" }
```

- 一个对象引用 + 一个判断项。`prop` 省略 = `@:exists is true`。
- 反面判断（`@:exists` / `@:onScreen` / `@:shown` 为 false）规则同 game-test-script 4.2：引用的类在屏上有实例但 `ident` 都不满足也算「不存在」。
- 删掉的写法（v4 不认，校验报 `cond_removed` 并说改成什么）：`target` + `exists` / `visible` / `enabled` / `checked` / `selected` / `text` / `textContains` / `textLike` / `textMatches` / `image` / `count` / `number` / `field` / `onScreen`。保留：`value: "$var"` / `value: "@来源.路径"`（取值来源，不是对象）、`api`、`expr`、`alias`、`actorNear`、`all` / `any` / `not`、`settleMs` / `elseSettleMs`。
- 表达式里新增 `prop(引用, "属性路径")`（只读，路径同 3.1：`prop(shop, "@/TxtTitle:text")`），删掉 `text(sel)` 等按选择器的内置函数（`expr_removed`）。

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
- `verify`：`gone`（默认，引用找不到）/ `hidden`（`@:onScreen` 为 false）/ `none`。`verifyMs` 默认 2000（`refDefaults.closeVerifyMs`）。
- `how` 省略：用运行配置 `closeRules` 里这个类（按继承，最近的规则优先）的 `how`；没有规则按默认推荐：类上有名字像 `Close` / `Hide` / `Back` / `Exit` / `Dismiss` 的无参可调函数 → `call`；有名字像 `BtnClose` / `CloseBtn` / `Close` 的按钮部件 → `click`；是 CommonUI 可激活控件 → `pop`；最后 `remove`。
- 守护 `do` 里常用。旧的 `widget.close` / `widget.call` 删掉（`action_removed`）。

### 5.3 其他对象动作

| 动作 | 字段 | 说明 |
| --- | --- | --- |
| `obj.set` | `obj`, `part?`, `prop`, `value` | 设属性（`prop` 是 3.1 的路径：`@.Count`、`@/ChkAuto.IsChecked`、`@#hp`；只能落在属性或 Lua 字段上，不能是 `()` 和 `:状态`）。控件状态走控件自己的 setter 让委托照常触发：`Visibility` → `SetVisibility`、CheckBox `IsChecked`/`CheckedState` → `SetIsChecked`/`SetCheckedState`、`Text` → `SetText`、Slider `Value` → `SetValue`、`IsEnabled` → `SetIsEnabled`；其余反射写入（`ImportText` / JSON 转换）或 Lua 字段赋值。只读、`none` 类的报 `prop_readonly` |
| `obj.call` | `obj`, `part?`, `function`, `args`, `saveAs`, `expect`, `each?` | 取代 `invoke`（`object` 字段改 `obj`；`obj` 也可以是 game-test-script 4.3 的来源字符串 `@sub.X` / `@luaMod.X`，或 `objects` 里写成来源字符串的名字；静态函数省略 `obj`，`function` 写 `类名.函数`）。2.1.0：`each: true` 时引用命中几个就每个都调（不按 `many` 挑，命中多个不算歧义；每个都按 `part` 找部件，有一个找不到整步失败），按命中顺序调；`payload.invoke` 变成数组（每个一条），返回值是数组（存 `saveAs`、`expect` 对着数组判断）；任一个调失败整步失败，消息写第几个，**已调的不回滚**。`obj` 是来源字符串时 `each` 不起作用 |
| `obj.find` | `obj`, `saveAs` | 找到后把对象存进变量（这次运行里的弱引用） |
| `obj.wait` | `obj`, `prop?`, `op?`, `value?` | `wait until=cond` 的简写：等到这个判断成立 |

`invoke`、`widget.close`、`widget.call`、`find` 在 v4 不认（`action_removed`）。

## 6. 场景和运行配置

```jsonc
{
  "kind": "gametest-scenario", "schemaVersion": 5, "name": "杂货铺",
  "objects": { "shop": { "class": "WBP_Shop", "ident": [ … ] }, "bag": { "class": "WBP_Bag" }, "me": "@pawn" },
  "steps": [ { "id": "s1", "action": "click", "target": { "obj": "shop", "part": "BtnBuy" } },
             { "id": "s2", "action": "obj.close", "obj": "shop" } ]
}
```

- `objects` 的值是引用（只能对象形式，不能再引用别的名字，`in.obj` 可以引用别的名字但不能成环：`ref_cycle`），或 2.1.0 起的来源字符串（`"@pawn"`、`"@sub.UQuestSubsystem"`、`"@luaMod.UI.Shop"`，game-test-script 4.3；用的时候现取，不缓存）。
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

参数三选一：`node`（`gt.tree` id，游戏开着）/ `target`（第 2 节写法，游戏开着）/ `class`（只按类，不开游戏也行：读 WBP 资产和反射；2.1.0 起 `lua:模块` 读 Lua 类，见下）；可选 `part`（只看这个部件）、`filter`、`offset` / `limit`（默认 120 项，最多 400）、`siblings`（默认 true：算「同类里」那一列）。

返回：

```jsonc
{ "class": "WBP_Shop_C", "luaModule": "UI.Shop.ShopPanel", "in": "screen",
  "siblings": 3,
  "groups": [
    { "id": "self", "title": "自己", "items": [
      { "prop": "@.ShopId", "name": "ShopId", "src": "cpp", "type": "int32", "kind": "int",
        "value": 1001, "distinct": 3, "changing": false, "score": 9, "ops": ["eq", "ne", "gt", "gte", "lt", "lte", "between", "in"] } ] },
    { "id": "part:TxtTitle", "title": "TxtTitle", "class": "TextBlock", "part": "TxtTitle", "items": [ … ] },
    { "id": "part:Header", "title": "Header", "class": "WBP_PanelHeader_C", "part": "Header", "user": true, "collapsed": true, "items": [] },
    { "id": "item", "title": "条目的数据对象", "class": "UBagItemData", "items": [ … ] },
    { "id": "states", "title": "算出状态", "items": [ … ] }
  ],
  "suggest": [ "@/TxtTitle:textKey", "@.ShopId" ],
  "matches": 1, "total": 64, "hasMore": false, "truncated": false }
```

- 组顺序：`self`（项目字段 C++ → 蓝图 → Lua，`src` 标出；后面是其余编辑器或蓝图看得见的属性：蓝图变量、项目 C++、控件 / 引擎类自带的依次排，可见性、文字、勾选、颜色这些常用的在前，最多 80 条，读不到或不能比较的不列）、`item`（条目时）、各部件（WidgetTree 顺序，每个部件都给 `collapsed: true`、`items` 空，面板展开时再用 `part` 调一次；嵌套 UUserWidget 另给 `user: true`）、`states`。
- 带 `part` 时（2.1.0 起）：返回的 `prop` 和 `suggest` **相对那个部件**写（`@.X`、`@:text`、`@#X`），面板写进脚本时补上 `/部件`（`@/部件.X`）；`self` 组标题是「属性」，没有 `item` 组；里面再嵌的部件组 `part` 从外层对象写（`Header.TxtTitle`，下次展开直接用）。
- Actor：每个组件一个部件组（`part` = 组件名，`class` = 组件类，折叠），面板的组件树靠它；按类读时只有 C++ 默认子对象，蓝图里加的组件要开游戏。
- `class: "lua:模块"`（2.1.0）：读模块表（UELink `lua.inspect`，模块要已经 require）：`groups` 是 `lua`（模块表自己的字符串键字段，`prop: "@#字段"`、`kind` 按值推、`value` 是模块表上的值 = 默认值）和 `functions`（`{name, params, from?}`，`from` = 第几层父类继承来的）；另给 `luaModule`、`chain`（Lua 继承链）。模块没加载报 `lua_class_missing`。
- UE 类和实例（2.1.5）：组的最后多一个 `functions`（标题「函数」）：类上蓝图能调的公有函数（`BlueprintCallable` / `BlueprintPure`，含继承来的，自己的在前，最多 300 个），项是 `{name, params, ret?, from?, src, pure?}`：`params` 是「`[out ]类型 名`」，`ret` 是返回类型，`from` = 定义它的父类，`src` 是 `cpp` / `bp`。有返回值的取值函数（Pure 或 const，和路径里 `.名字()` 的要求一样），参数都是数字 / 布尔 / 文字 / 枚举时（2.1.6 起；之前只认无参的）另给 `path`（`@.名字(0, "")`，参数按顺序先填零值）、`args`（要写几个参数，没有参数时不给）、`kind`、`ops`，能加进唯一标识；其余只列出来看，调用用 `obj.call`。不开游戏按类也能读。
- 每项：`prop`（直接能写进脚本的 v5 路径：自己的属性 `@.X`、Lua 字段 `@#X`、条目数据对象 `@item.X` / `@item#X`、状态 `@:X`）、`name`、`src`（`cpp` / `bp` / `lua` / `state`）、`type`（引擎类型名）、`kind`（3.4）、`value?`（游戏开着时，超长截 200 字）、`distinct?`（同类 `siblings` 个实例里有几种值；`siblings` 是同 `in` 下同类的实例数，最多看 50 个）、`members?`（enum 和 visibility：全部成员名，面板给下拉）、`score`（0–10，当标识的推荐分，同录制条目打分：唯一、像 Id、数据对象上分高；位置、选中、计数、长文字、会变的分低；`changing`「1 秒内自己变过」2.0.0 没做，会变的只按名字和状态的 `IsStable` 压分）、`ops`、`comparable`（false 时带 `why`）、`readOnly`、`cat?`（2.0.5 起：UPROPERTY 的 Category，即 UE 细节面板里的分类，如 `Behavior`、`Appearance|Font`；只在带元数据的构建里有，面板按它分组，没有就不分）。
- `suggest`：能把当前对象和同类其他实例区分开的最少属性组合（最多 3 个），按 `identRules` 先、再按分数。
- `matches`：按 `suggest` 组成的 `ident` 此刻命中几个。
- Lua 组：游戏开着时由实例表给（`src: "lua"`）；面板再用 `---@field` 声明补全（见 7.5）。
- 最多 12000 字，超出截断、给 `nextOffset`。

### 7.4 `gt.refFor`：从游戏里点到的东西出引用

参数：`node`（`gt.tree` id）或 `x` / `y`（视口归一化坐标，同 `gt.pick at`）；`level?`（命中链里第几层当对象，默认自动）。

返回：`chain: [{node, class, kind: "entry"/"widget"/"actor"/"component", part?, note}]`（从点到的往外：条目 → 界面 …；`part` 是该层能用的部件写法）、`ref`（推荐的引用，`ident` 按 `identRules` 和打分挑）、`target`（`{obj: ref, part}` 当点到的是部件时）、`matches`、`ask`（拿不准时 true：没有分数够的组合，面板应弹属性树让人挑）。
`gt.pick action=at` 的返回里也带同样的 `chain` / `ref` / `target`（2.0.0 起）。

### 7.5 Lua 字段的来源（面板侧）

- 面板按对象的 `luaModule`（`gt.props` / `gt.widgets` 给）找到 Lua 源文件，用已有的 `lua-parse` 读 `---@class` 下的 `---@field 名 类型 说明`（也收 `self.x = …`、模块表字段），给 `#` 后面的 Lua 字段名做**可搜索下拉**；同时允许直接填文字（下拉里没有的也收）。
- 类型按 `---@field` 写的类型映射到 3.4 的 kind（`number`/`integer` → `float`/`int`，`string`，`boolean` → `bool`，`table`/`X[]` → `array`，其他 → 按运行时值）。

### 7.6 变化

| 操作 | 变化 |
| --- | --- |
| `gt.resolve` | `conds` 里接受第 4 节写法；新增 `refs: [{id?, ref, show?: [prop…]}]`（可同时给场景的 `objects`，`ref` 才能写名字）→ `refs: [{id, count, total, candidates: [{name, path, match, ident: [{prop, actual}]}]}]`（最多 8 个；`path` 是对象路径，2.1.3 起，面板点了某个实例时拿它给 `gt.props path` 读这一个），`show` 里的属性按候选给实际值放进 `ident`，面板「命中几个」用它（配 `gt.watch` 推送）。`targets`（部件写法数组）和 `scenario` 核对改按引用解析，字符串选择器报 `selector_removed` |
| `gt.vars` | `scope: "object"` 的参数从定位器改成引用：`{scope: "object", ref}`（写 `locator` 报错）；2.1.0：界面、类成员的 `path` 是 v5 路径（`@.X`，Lua 实例字段 `@#X`）；函数参数是对象（UObject 指针、软引用、类引用）时带 `objectClass`（参数的类名，如 `PlayerController`、`WBP_Shop_C`），面板据此只列类型对得上的对象 |
| `gt.validate` | 按本文校验；新错误码见第 10 节。`deep: true` 的选择器检查换成引用检查：类在不在、`part` 第一段在不在 WBP 控件树里（`part_missing`）、`@.X` 在类上有没有（`prop_missing`）、`@.F()` 能不能调（`fn_not_callable`）、op 和属性类型对不对（`op_type`）；`checked.selectors` 改名 `checked.refs` |
| `gt.record.*` | 录出来就是 v4（第 8 节）。暂停问标识（`gt.record { phase: "ask" }` 和 `gt.record.status` 的 `ask`）带 `ref`（条目的引用）、`node`（`gt.tree` 的 id，调过 `gt.tree` 才有）、`target`（`{obj, part}`，界面引用写名字时同时给 `objects`）；原来的选择器文本挪到 `selector`。`gt.record.key` 的回答：`{step?, resume?, ident: [{prop, op: "eq", value}], rule?: true（写进 identRules）, all?: true（同一列表后面的条目都按它认）}`，先跳过 `{mode: "skip"}`；空 `ident` = 只按第几个认。录制时挑标识只认 `eq` 和条目上的字段（v5 路径：`@item.X` / `@item#X` / `@.X` / `@#X`、`@:text`；写旧写法报 `path_old`） |
| `gt.inspect` | 多 `chain` / `ref` / `target` / `matches` / `ask`（同 `gt.refFor`），`hint` 保留 |
| `gt.keys` | `target` 改成部件写法；返回的字段带 `prop`（能直接写进 `ident` 的 v5 路径，行列号没有），其余不变 |
| `gt.actions` | 按本文列；另给 `compares`（`[{op, kinds, verb, summary, noValue?}]`）、`states`（`[{prop, kind, on, summary, changing?}]`）、`closers`（`[{by, summary, fields}]`），面板核对自己的表 |
| `gt.config` | 读写都带 `identRules` / `closeRules` / `refDefaults` / `allowDestroy`（写回时只改认得的字段，别的原样留着） |
| `gt.props` | 7.3；`class` 模式用类的默认对象和 WBP 生成类的设计器树，不开游戏也行（2.1.4 起 `value` 给类默认对象上的默认值，此前不给值）；`class` 给资产路径（`/Game/A/BP_X.BP_X`）时蓝图没加载会先加载（2.1.3）；`path`（对象路径，2.1.3）读一个具体实例 |
| `gt.step` | 2.1.0：新参数 `objects`（同场景的 `objects`：名字 → 引用或来源字符串）。给了时步骤里可以写名字、`{"$obj": 名}`；照样等这一步跑完再返回（同块步骤的临时场景） |
| `gt.resolve` / `gt.watch` | 候选的 `name` 是控件名（同 `gt.tree` 的 `name`），调过 `gt.tree` 时另带 `node`（节点 id）；`in` 数组、`lua`、`class: "lua:…"` 随引用生效；`objects` 也收来源字符串 |

### 7.7 `gt.classes`：类树（2.1.0）

面板「选对象」的类下拉用。参数：`parent?`（列它的直接子类；省略 = `Object`，Lua 类树省略 = 最顶层的模块）、`q?`（名字**包含**即命中，大小写不敏感，开头对上的排前面；Lua 模块只比最后一段（q 里带 `.` 或 `/` 时比整条路径）；给了时不看 `parent`，返回命中的和它们的全部祖先，祖先在前）、`lua?`（true：Lua 类树）、`offset` / `limit`（默认 200，最多 500）、`fresh?`（true：不用缓存重建类表；平时 15 秒内的请求复用上一次建好的类表，逐层展开不再每次重扫）。

```jsonc
{ "items": [
    { "name": "UserWidget", "parent": "Widget", "kind": "umg", "kids": 214 },
    { "name": "WBP_Shop", "parent": "WBP_PanelBase", "kind": "umg", "asset": "/Game/UI/Shop/WBP_Shop", "kids": 0, "live": 1 } ],
  "total": 37, "hasMore": false }
```

- `name`：原生类是不带前缀的类名（`Actor`、`UserWidget`），蓝图是去掉 `_C` 的生成类名（和引用的 `class` 写法一致，可以直接写进去）；Lua 类是模块名（写进引用时加 `lua:`）。
- `parent`：父类名（`Object` 没有）；`kind`：`umg`（继承 `Widget`）/ `actor` / `component`（继承 `ActorComponent`）/ `object` / `lua`；`asset`：蓝图的资产包名（原生类没有）；`kids`：直接子类个数；`live`：游戏开着时这个世界里此刻的实例数（只算这个类本身，不含子类；0 不写）。Lua 类的 `live` 是绑定这个模块的对象数（`lua.modules` 的 `objects`），另带 `file`。
- Lua 按目录列（2.1.3）：`lua: true` 且给了 `dir`（字符串，空 = 最顶层；`.` 和 `/` 都当分隔）、不搜时，`items` 是正好在这个目录下的模块（不管父类，模块的目录 = 名字按 `/`、`.` 切段去掉最后一段），第一页另带 `dirs: [{name, count}]`：下一层子目录（`name` 是 `/` 连的完整目录）和它下面（含更深层）的模块数；子目录最多占回包预算的一半，太多时截断并带 `dirsMore: true`（多打几个字搜）。点开模块看子类照旧用 `parent`。
- UE 类 = 已加载的 `UClass` + 编辑器里资产注册表的蓝图（只读 `GeneratedClass` / `ParentClass` 标签，**不加载资产**）；打包的游戏进程里只有已加载的类。资产注册表还在扫描时带 `note`。
- 结果按名字排；回包最多 12000 字，超了截断，`hasMore: true` + `nextOffset`（至少给一项）。
- Lua 类树要 UnLua（没有时报错说明）；父类 = `lua.inspect` 元表链上的下一层 Lua 模块，只看已经 require 的模块。

## 8. 录制

- 点击：接住输入的控件 → 最近的 UUserWidget 当对象，控件在它类树里的名字当 `part`（运行时生成的往外找能写的）；对象是容器条目时 `in: {obj: 父对象, part: 容器}`。对象引用的 `ident`：同类此刻只有一个 → 空；有 `identRules` → 按规则；否则按打分挑最少的能区分的组合；挑不准按原来的 `askKey` 规则暂停问人（`gt.record { phase: "ask" }`），面板弹属性树。
- 同一个对象在一次录制里多次出现：引用去重，写进 `objects`（名字按类名去掉前缀：`shop`、`bagSlot`，重名加序号），步骤里写名字。条目对象的标识值不同时各自内联，不去重。
- Ctrl+Alt 点 / Alt+4：录 `wait until=cond`/`assert`，判断项默认：文字类 `@/部件:text eq 此刻`、其余 `@/部件:onScreen is true`；面板录完可以改（属性树）。
- `hint` 不变（截图、OCR、字段缓存只用于显示和换标识）。
- 2.0.0 的做法：录制期间内部仍按选择器文本记目标（挑标识、问人、改方括号都按它），**存盘时**统一改写成 v5（`Record/V4/GameTestRecordV4`，属性路径按 3.1）；改写不准的步骤写 `todo`，`gt.record.stop` 返回 `todo: 步数`。所以录制中 `gt.record.status` 给的步骤预览仍是选择器写法。界面对象的 `ident`：同类只有一个时不写（录制时同一界面一般只开一份），容器条目按挑好的标识；「按打分挑最少的能区分的组合」只用在条目上，界面本身不挑。

## 9. 迁移

不做。2.1.0 起只认 schemaVersion 5，旧脚本报 `schema_old`，按本文重写（面板和 GameTest 2.0 的「升级脚本」都已删掉）。

## 10. 错误码（新增）

`schema_old`、`settings_old`、`path_syntax`（2.1.0）、`path_old`（2.1.0）、`lua_class_missing`（2.1.0）、`fn_not_callable`、`ref_need_owner`、`ref_cycle`、`ref_ambiguous`、`ref_not_found`、`ref_bad_class`、`part_missing`、`prop_missing`、`prop_readonly`、`op_type`、`op_unknown`、`cond_removed`、`expr_removed`、`selector_removed`、`action_removed`、`close_unavailable`、`close_failed`。（`migrate_todo` 随迁移一起删了。）
`object_not_found` / `object_ambiguous` 改名 `ref_not_found` / `ref_ambiguous`。

## 11. 代码结构（扩展点，接 game-test-script 第 14 节）

| 扩展点 | 接口 | 注册表 | 一个实现一个文件 |
| --- | --- | --- | --- |
| 算出状态（`:onScreen` …） | `IGameTestState`：名字、适用（控件 / Actor / 任何）、kind、`Read(UObject*, ctx) → FGameTestValue` | `FGameTestStateRegistry` | `Observe/Ref/States/GameTestState<名字>.cpp` |
| 比较（`eq` `contains` `visible` …） | `IGameTestCompare`：名字、适用 kind、`Test(actual, item) → {holds, why}` | `FGameTestCompareRegistry` | `Observe/Compare/GameTestCompare<分类>.cpp` |
| 关闭方式（`call` `click` `pop` …） | `IGameTestCloser`：名字、字段、`Validate`、`Close`（同步做一次，跨帧核对由 `obj.close` 统一做）、`Recommend` / 推荐顺序 | `GameTestClosers::Get()` | `Operate/Close/GameTestClose<名字>.cpp`（`pop` 在 GameTestCommonUI） |
| 属性 kind 分类 | `FGameTestPropKind::Of(FProperty*)` | — | `Observe/Ref/GameTestPropKind.cpp` |

面板：`shared/gt-script/ref/`（类型、属性路径 `path.ts`、校验、人话描述、比较表）、`web/sections/uekit/gt-ref/`（`PropTree`、`RefCard`、`CompareEditor`、`RefDialog`、`WidgetTreePick`、`CloseHowList`）、`gt-cond/checks/prop.tsx`。比较表在面板和 C++ 各一份，名字和适用 kind 必须一致（`shared/gt-script/ref/compare.ts` 的单测核对本文第 3.4 节表格）。

## 12. 版本

- GameTest 2.0.0：本文全部（实现和本文不同的地方已在对应小节写明：`obj.close` 的 `click` 只做代码点击、关闭方式同步做一次再跨帧核对、`gt.highlight` 不避开截图、`gt.props` 没有 `changing`、`gt.tree` 和引用的 `client` 没做、skipIf 不再自动把没生成的列表条目滚出来）。需要 UELink ≥ 2.12.0（不变）。
- GameTest 2.1.6：`ident` 条件组（3.5）、判断项 `not`、路径里的函数带常量参数（3.1）、`gt.props` 的 `functions` 给带参数的 `path` 和 `args`。
- GameTest 2.1.4：`gt.props` 的 `class` 模式给默认值。
- GameTest 2.1.3：`gt.classes` 的 Lua 按目录列（`dir`、`dirs`）；`gt.props` 收 `path`、`class` 给资产路径时加载蓝图；`gt.resolve` 候选带 `path`。
- GameTest 2.1.0：schemaVersion 5（属性路径 v5，3.1；旧写法报 `path_old`，不迁移）、引用的 `in` 数组 / `lua` / `class: "lua:…"`、`objects` 收来源字符串和中文名、`{"$obj": 名}` 当引用、`gt.classes`、`gt.step objects`、`obj.call each`、`gt.vars` 的 `objectClass`、`gt.props` 的 `cat`。
- 面板 UE 插件按 `gt.health.version` 判断：低于 2.1.0 时编辑页只读、提示升级 GameTest；脚本不是 v5 时提示按新写法重写。
