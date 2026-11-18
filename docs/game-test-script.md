# GameTest 脚本（schemaVersion 3，1.9.0 起）

这份是 GameTest 脚本化的**协议**：场景格式 v3、统一条件、表达式、守护、调函数、对象定位、接口库和新增的 `/rpc` 操作。
GameTest（C++）、面板 UE 插件（`agent-plugins` 的 `ue/`：编辑页、流程图、Agent 工具和技能）都按这份实现；改字段先改这里。
平铺步骤、选择器、动作、录制这些 1.x 的内容仍以 [game-test.md](game-test.md) 为准，这里只写新增和变化。
设计取舍见项目设计稿「GameTest 脚本化与流程图 · 定稿」。

## 1. 场景文件

```json
{
  "kind": "gametest-scenario",
  "schemaVersion": 3,
  "name": "每日福利",
  "tags": ["daily"],
  "params": { "account": { "default": "qa_01", "type": "string", "note": "登录用的账号" } },
  "vars": { "skipped": 0 },
  "objects": { "shopNpc": { "class": "BP_ShopNpc_C", "key": { "NpcId": 1001 } } },
  "guards": [ { "id": "g1", "name": "新手引导出现", "when": { "target": "WBP_Guide:.", "onScreen": true },
                "do": [ { "action": "call", "script": "common/翻完引导" } ] } ],
  "setup": [ … ],
  "steps": [ … ]
}
```

| 字段 | 说明 |
| --- | --- |
| `schemaVersion` | `3`。1 / 2 照读（平铺步骤就是只有主流程、没有块的 v3）；比支持的高就拒绝并提示升级 |
| `params` | 参数：`{名字: {default, type?, note?}}`。`type`：`string` / `int` / `float` / `bool` / `vector` / `object` / `any`（默认按 `default` 推断）。`gt.run params` 和 `call with` 传入 |
| `vars` | 脚本变量初值：`{名字: 值}` |
| `objects` | 对象定位器（第 7 节）：`{名字: 定位器}` |
| `guards` | 本脚本的守护（第 5 节） |
| `setup`、`steps` | 步骤数组；`steps` 是主流程 |

**步骤 id**：每个步骤（含块里的）可以有 `id`（字母数字下划线，场景内唯一）。面板存盘时自动补（`s3`、`if1`、`loop2`、`g1`）。
报告、历史、断点、`from` / `to` 都按 id；没有 id 的按路径（`steps.3.do.0.then.1`）。读 v1 / v2 时不补 id、不改文件。

### 1.1 引用

| 写法 | 意思 |
| --- | --- |
| `$名字` | 参数或变量（先找变量，再找参数）。整个值就是 `$名字` 时保持原类型；嵌在字符串里时按文字替换（`"第 $i 页"`） |
| `$名字.字段`、`$名字[0]` | 结构体、Map、数组、返回值的成员 |
| `$item` | `loop each` 当前条目：选择器里 `$item>BtnClaim`；值里 `$item.字段`（条目数据对象，同 `@item`） |
| `{"$obj": "名字"}` | `objects` 里的对象（运行时按定位器找） |
| `"=表达式"` | 以 `=` 开头的字符串是表达式（第 3 节）。要写真正以 `=` 开头的文字，写 `"\\=..."` |

## 2. 块和新动作

所有步骤共用的字段：`id`、`note`、`onFail`（`stop` 默认 / `continue` / `retry`）+ `retries`、`timeoutMs`、`skipIf`（统一条件 + 原有 `also` / `verify`）、`guards`（只在这一块里生效）、`expectNoErrors`（这一步 / 这一块执行期间日志里出现 Error、Ensure、Lua 报错就算失败，默认 false）、`disabled`（true 时跳过，报告记 `disabled`）。

| `action` | 字段 | 说明 |
| --- | --- | --- |
| `group` | `name`, `do`, `recorded?`（录制产生的段，面板默认折叠） | 步骤段 |
| `if` | `cond`, `then`, `elif: [{cond, then}]`, `else` | 从上往下第一条成立的走；都不成立走 `else`（可省）。`branch: "state"` 只是面板画成「按状态分支」的提示，执行一样 |
| `loop` | 四选一：`until`（每轮做完看，成立就出）/ `while`（每轮开始前看）/ `times`（整数或 `$var`）/ `each`（容器选择器）+ `as`（默认 `item`）或 `in`（数组或 `$var`，配 `as`）；`max`（除 `times` 外必填，默认面板填 30，上限 1000）、`gapMs`、`onMax`（`fail` 默认 / `continue`）、`do` | 循环变量 `$index`（从 0）。`each` 遍历容器条目，自动滚动沿用 1.7 |
| `call` | `script`（相对场景根，`.json` 可省）、`with`（参数） | 子脚本自己的 `guards` 在调用期间生效、和外层叠加；自己的 `vars` 独立；最多嵌套 8 层；加载时查环。返回值：子脚本 `return` 带的 `value` 存进 `saveAs` |
| `set` | `var`、`value`（值、`$ref`、表达式）；或 `from: {target, read: "text"/"number"/"visible"/"enabled"}` | 没在 `vars` 里声明的变量第一次 `set` 时创建 |
| `invoke` | `object`, `function`, `args`, `saveAs`, `expect`, `client` | 调函数（第 6 节） |
| `api` | `api`, `args`, `saveAs`, `expect`, `client` | 调接口库里的接口（第 8 节） |
| `break` / `continue` | `cond?` | 出最内层循环 / 进下一轮 |
| `return` | `cond?`, `value?` | 结束当前脚本（被 `call` 时回到调用处） |
| `fail` | `message`（可用表达式） | 主动失败 |
| `widget.close` | `target`（界面） | 把界面从父级移除（原阻挡界面 `close`） |
| `widget.call` | `target`（界面）, `function`（无参） | 调界面上的无参函数（原阻挡界面 `call`） |
| `assert` | 原有字段 + `expr` / `cond` | `cond` 是统一条件；`expr` 是表达式 |
| `wait` | 原有字段 + `until: "cond"` + `cond` | 等到统一条件成立 |

- 块里的超时：`timeoutMs` 写在块上是整块的总时限；块里的每步各自的等待时限不变。
- `onFail: retry` 写在块上是整块重做（从块的第一步开始）。
- 校验：未知字段、未知动作、循环缺 `max`、`call` 找不到 / 成环、`$变量` 未定义、`id` 重复都在加载和 `gt.validate` 时报出来。

## 3. 表达式

只算值、不改游戏。写在任何填值的地方（`"=…"`）、条件的 `{"expr": "…"}`、`assert` 的 `expr`。

**字面量**：数字 `3` `1.5` `-2`、字符串 `"abc"` / `'abc'`、`true` / `false` / `null`、数组 `[1, 2]`、对象 `{"X": 1, "Y": 0, "Z": 50}`。
**引用**：`$var`、`$var.a.b`、`$var[0]`、`$var[-1]`、`$map["k"]`、`@来源.路径`（第 4.3 节）、`@来源.Fn(参数)`（只读函数）。
**运算**（优先级从低到高）：`a ? b : c`、`or`、`and`、`not`、`== != > >= < <=`、`in`（`x in [1,2]`、子串）、`+ -`（`+` 两边有字符串就拼接）、`* / %`、一元 `-`、成员 / 下标 / 调用。
**内置函数**：

| 函数 | 说明 |
| --- | --- |
| `text(sel)` `number(sel)` `visible(sel)` `enabled(sel)` `exists(sel)` `onScreen(sel)` `checked(sel)` `count(sel)` | 控件状态（同第 4.2 节的判断，`number` 按 1.x 规则从文字里取数） |
| `len(x)` `str(x)` `num(x)` `int(x)` `bool(x)` | 长度、类型转换 |
| `round(x, n?)` `floor(x)` `ceil(x)` `abs(x)` `min(…)` `max(…)` `clamp(x, a, b)` | 数值 |
| `contains(a, b)` `startsWith(a, b)` `endsWith(a, b)` `lower(s)` `upper(s)` `trim(s)` `replace(s, a, b)` `split(s, sep)` `join(arr, sep)` `format(fmt, …)`（`{0}` `{1}`） | 文字 |
| `vec(x, y, z)` `dist(a, b)` | 向量：拼 FVector、两点距离（a、b 可以是向量或 Actor） |
| `now()` | 游戏时间（秒，`UWorld::GetTimeSeconds`） |
| `接口名(参数…)` | 只读接口（第 8 节），如 `Object.Count("BP_Monster_C")`、`Widget.IsShown("WBP_Shop")` |

- 只读：函数调用只放行内置函数、只读接口、`BlueprintPure` + `const` 的 UFUNCTION、`_settings.json` 里 `allowLuaInCond: true` 时的 Lua 函数。其余在解析时就报 `expr_not_readonly`。
- 没有赋值、循环、自定义函数、lambda。长度上限 1000 字。
- 类型：数字（double）、字符串、布尔、null、数组、对象、向量、对象引用（UObject 弱引用）。比较字符串忽略大小写；`==` 数字和数字字符串按数字比。
- 取不到（对象不在、字段不存在、下标越界）：结果是 **unresolved**，条件里按不成立算，`set` / 参数里让这一步失败；报告写出哪一段取不到。
- 解析和类型检查在加载、`gt.validate` 时做（`expr_syntax`、`expr_type`、`var_undefined`、`path_missing`）。

## 4. 统一条件

```json
{ "all": [ 判断, { "any": [ 判断, 判断 ] }, { "not": 判断 }, { "expr": "@ps.Gold >= $gold0 + 500" } ],
  "settleMs": 300, "elseSettleMs": 1000 }
```

`all` / `any` / `not` 任意嵌套；只有一条判断时不用包。`if`、`loop until/while`、`wait until:"cond"`、守护 `when`、`skipIf`、`assert cond`、`invoke expect` 都用它。
旧的条件对象（1.6 起的 `exists` / `visible` / `field` / `any` …）就是一条判断，不用迁移。

### 4.1 判断

| 看什么 | 写法 |
| --- | --- |
| 界面在不在屏上 | `target`（界面选择器 `WBP_X:.`）+ `onScreen: true/false`：在视口里、整条父链可见、有尺寸。界面只按继承 `UUserWidget` 认，不按名字 |
| 控件 | `target` + `exists` / `visible` / `enabled` / `checked` / `selected` / `text` / `textContains` / `textLike` / `textMatches`（正则）/ `image` / `count` + 比较 |
| 文字里的数字 | `target` + `number: {eq/ne/gt/gte/lt/lte/between:[a,b]}` |
| 控件 / 界面的字段 | `target` + `field: "@self.X" / "@lua.X" / "@item.X" / "@itemLua.X"` + 比较 |
| 代码变量 | `value: "@来源.路径"` + 比较 |
| 脚本变量 | `value: "$var"` + 比较 |
| 接口 | `api` + `args` + 比较（只读接口） |
| 表达式 | `expr` |
| 附近的 Actor | `actorNear`（同 wait 谓词） |
| 常用名 | `alias: "名字"` + 比较（展开成 `_vars.json` 里的定义） |

比较：`equals` / `ne` / `gt` / `gte` / `lt` / `lte` / `in: [..]` / `contains` / `matches`（正则）；没写比较时，`value` / `api` / `field` 按真值算（非 0、非空、true）。

### 4.2 稳住和反面

- `settleMs`（默认 300）：连续成立这么久才算成立。写在条件根上，对整棵树算。
- `elseSettleMs`（默认 1000）：`if` 走 `else`、`loop while` 出循环、`until` 判「还不成立」时，要连续不成立这么久。
- 反面判断（`exists:false`、`visible:false`、`onScreen:false`）只在它所在的屏已经在屏上时才算数（同 skipIf 规则）；目标所在的屏都不在时，`onScreen:false` 照常成立。
- 读不到（对象不在、字段不存在）：这条判断不成立，报告里 `unresolved: 原因`。

### 4.3 取值来源

| 来源 | 读哪里 |
| --- | --- |
| `@gi` / `@gs` / `@gm` / `@ps` / `@pc` / `@pawn` / `@hud` | GameInstance、GameState、GameMode（只有服务器 / 单机）、本地 PlayerState、PlayerController、Pawn、HUD |
| `@sub.<类名>` | 子系统（GameInstance / World / LocalPlayer / Engine，按类名找，`U` 前缀可省） |
| `@actor[tag=X]` / `@actor[class=BP_X]` / `@actor[name=大纲名]` | 世界里唯一命中的 Actor（多个时报错并列出） |
| `@obj.<名字>` | `objects` 里的对象 |
| `@luaMod.<require 路径>` | Lua 模块表（`require` 过的；没加载不主动 require，读不到） |
| `@luaG.<名字>` | Lua 全局变量 |
| `@cvar.<名字>` | 控制台变量 |
| `$var` | 变量里的对象 |

路径：`.成员`、`[下标]`、`["Map 键"]`；UObject 走反射（`FProperty`，结构体、数组、Set、Map、对象引用、软引用不加载），Lua 表走 UnLua（按项目里的 UnLua 头选接口，二次开发版）。
`.Fn()` / `.Fn(参数)`：只读函数（见第 3 节）。多客户端时判断和步骤可带 `client`。

## 5. 守护

```json
{ "id": "g1", "name": "新手引导出现",
  "when": { "target": "WBP_Guide:.", "onScreen": true, "settleMs": 300 },
  "do": [ { "action": "call", "script": "common/翻完引导" } ],
  "check": "both", "after": "retry", "max": 10, "cooldownMs": 1000,
  "unlessStepTargets": true, "onFail": "stop", "disabled": false }
```

| 字段 | 说明 |
| --- | --- |
| `when` | 统一条件 |
| `do` | 步骤数组（任意步骤和块，常见 `call`、`widget.close`、`widget.call`、`console`、`invoke`） |
| `check` | `beforeStep`（每步开始前）/ `whileWaiting`（等待中每 0.2 秒）/ `both`（默认） |
| `after` | 处理完：`retry`（回到被打断的那一步重新开始等，默认）/ `skip`（那一步算跳过）/ `fail` |
| `max` | 一次 run 里最多触发几次（默认 20），超过算失败 |
| `cooldownMs` | 两次触发的最短间隔（默认 1000） |
| `unlessStepTargets` | 正在执行的那一步的 `target` / `toTarget` 落在这个界面里时不触发（默认 true） |
| `onFail` | `do` 失败时：`stop`（整个 run 失败，默认）/ `ignore`（记一条警告继续） |

- 生效范围：`_settings.json` 的 `guards`（全局）+ 场景顶层 + 每层块的 `guards` + 正在被 `call` 的子脚本的，全部叠加。顺序：块内层 → 脚本 → 全局，同层按数组顺序。
- 同时看、按序处理：所有守护每次都一起判断；同时成立的按上面的顺序一次只处理一条，处理完重新看。
- 不重入：执行守护的 `do` 时不再检查守护；被打断那一步的等待计时暂停，`after: retry` 时重新计时。
- 不在动作中途插入：拖拽、长按、`move` 进行中不检查，做完再看。
- 处理完核对：`do` 做完后再判断一次 `when`，还成立记 `stillThere`（`after` 照常），连续 3 次还成立算失败。
- 报告：步骤 `payload.guards: [{id, name, at: "beforeStep"/"waiting", ms, steps, ok, message?}]`；run 的 `guards: {id: 次数}`；推送 `gt.flow phase=guard`。

### 5.1 取代阻挡界面

1.9.0 第一次读到 `_settings.json` 里的 `blockers` 时一次性改写成 `guards`，原文件存成 `_settings.json.bak`（已存在就 `.bak.1`…），写回后日志记一条。
之后再出现 `blockers` 报错：「blockers 已改成 guards，见 docs/game-test-script.md 第 5.1 节」。

| `blockers[]` | `guards[]` |
| --- | --- |
| `widget: "WBP_X"` | `when: {"target": "WBP_X:.", "onScreen": true}` |
| `delayMs` | `when.settleMs` |
| `action: "close"` | `do: [{"action": "widget.close", "target": "WBP_X:."}]` |
| `action: "call"` + `function` | `do: [{"action": "widget.call", "target": "WBP_X:.", "function": "…"}]` |
| `action: "command"` + `command` | `do: [{"action": "console", "command": "…"}]` |
| `name` | `name`；`id` 按 `b1`、`b2` 补 |
| 处理同一实例只一次 | `cooldownMs` + 处理完核对 |

`payload.blockers` 不再写，改成 `payload.guards`。

## 6. 调函数（`invoke`）

```json
{ "action": "invoke", "object": "@sub.UQuestSubsystem", "function": "AcceptQuest",
  "args": { "QuestId": "$questId", "bForce": false }, "saveAs": "ok", "expect": { "equals": true } }
```

- `object`：第 4.3 节的来源、`{"target": "WBP_Bag:."}`（界面）、`{"$obj": "名字"}`、`$var`、`@luaMod.路径`（模块表，`function` 是模块上的函数）、`@lua` 配 `target`（界面绑的 Lua 实例）。省略 `object` 且 `function` 写成 `类名.函数` 时调静态函数（蓝图函数库、`static UFUNCTION`）。
- 能调的：`BlueprintCallable`、`BlueprintPure`、`Exec`（含 CheatManager）UFUNCTION；Lua 表上的函数（`_settings.json` `allowLuaInvoke`，默认 true）。私有、`DevelopmentOnly` 在 Shipping、编辑器专用函数在游戏进程里报 `fn_not_callable`。
- `WorldContextObject` / `meta=(WorldContext=…)` 参数自动填当前世界。
- `args` 按参数名给（Lua 函数按顺序，也可以给数组）。参数类型：

| 类型 | 写法 |
| --- | --- |
| bool / 整数 / 浮点 / FString / FName / FText | JSON 值或表达式 |
| 枚举 | `"EQuestType::Main"` 或 `"Main"` 或整数 |
| 结构体（字段都可构造：FVector、FRotator、FVector2D、FLinearColor、FColor、FTransform、FGameplayTag、FGameplayTagContainer、项目 USTRUCT） | JSON 对象（`FJsonObjectConverter`）或引擎文本 `"(X=1,Y=2,Z=3)"`（`ImportText`）；FGameplayTag 写 `"Tag.A.B"` |
| 数组 / Set / Map（元素是上面这些） | JSON 数组 / 对象 |
| 资产、类、软引用 | 路径字符串（`/Game/…`），类写类名或路径 |
| UObject（Actor、组件、界面、数据对象） | 只能是能重新找到的：`{"$obj": "名字"}`、`$var`（这次运行里拿到的对象）、`{"target": "…"}`（界面 / 控件）、`@来源` |
| 委托、接口（`TScriptInterface`）、原始指针、`out` 的复杂引用 | 不支持：`gt.vars` 里标 `callable: false` + `why` |

- 返回：`saveAs` 存返回值；有输出参数时存成 `{ReturnValue, 参数名…}`。`expect` 是统一条件，对返回值判断（判断里不写 `target` / `value` 时就是看返回值本身）。
- 报告：`payload.invoke: {object, function, args, returned, ms}`（值超长截到 500 字）。
- 实现：参数打包和 `ProcessEvent` 复用 UELink 调函数（`ue.call`）同一份代码。

## 7. 对象定位器

```json
{ "class": "BP_ShopNpc_C", "in": "world", "key": { "NpcId": 1001 }, "component": null, "near": null, "client": 0 }
```

| 字段 | 说明 |
| --- | --- |
| `class` | 类名（`BP_X_C`、`ANpc`、`U` / `A` 前缀可省）或路径；子类也算 |
| `in` | `world`（默认：关卡里的 Actor）/ `widgets`（屏上的界面）/ 来源路径（`@sub.UQuestSubsystem.ActiveQuests`：容器属性里的对象）/ `all`（`TObjectIterator`，最后才用） |
| `key` | `{路径: 值}`，都要等于（`$var`、表达式可用）；路径同第 4.3 节，Lua 字段写 `@lua.id` |
| `component` | 在找到的 Actor 上再取组件（类名或名字） |
| `near` | `[x, y, z]` 或 `$var` 或 `@pawn`：多个时取最近的 |

- 解析：按 `in` + `class` 找候选，按 `key` 过滤。0 个：等到超时（受步骤的 `timeoutMs`）。1 个：用它。多个：有 `near` 取最近；没 `key` 取**第一个**（排序固定：关卡里放的在前，再按名字），报告标 `byFirst: true`；有 `key` 还多个报 `object_ambiguous`，列出每个候选的标识值。
- `_settings.json` 的 `objectKeys: [{class, by: [路径…]}]`：抓这一类时默认勾这些。
- 报告：`payload.object: {name?, class, picked, by, byFirst?}`。

## 8. 接口库

接口 = 名字 + 分类 + 参数 + 返回 + 说明 + 是否只读。`api` 步骤、条件的 `api`、表达式里调用都按名字找。

### 8.1 自带接口

参数写法：`obj` 指对象（第 6 节 UObject 写法任一种），`sel` 指控件选择器，`cls` 指类名，`loc` 指定位器或类名。✓ = 只读（能当条件、能进表达式）。

| 分类 | 接口 | 只读 | 说明 |
| --- | --- | --- | --- |
| 对象 | `Object.Find(loc)` → obj | ✓ | 第 7 节规则；找不到等到超时 |
| | `Object.FindAll(loc)` → [obj] | ✓ | 不等待 |
| | `Object.Count(loc)` → int、`Object.Exists(loc)` → bool | ✓ | 不等待 |
| | `Object.IsValid(obj)` → bool、`Object.Class(obj)` → string、`Object.Name(obj)` → string、`Object.IsA(obj, cls)` → bool | ✓ | |
| | `Object.Get(obj, path)` → 值 | ✓ | 反射或 `@lua.` |
| | `Object.Set(obj, path, value)` | | 只许可构造的值类型；网络同步属性在服务器改（`client` 指定） |
| | `Object.Call(obj, fn, args)` → 值 | 看函数 | 等于 `invoke` |
| 界面 | `Widget.IsShown(cls 或 sel)` → bool | ✓ | 按继承 UUserWidget 认，子类也算 |
| | `Widget.Find(cls, key?)` → obj、`Widget.List()` → [{class, name, z, visible, obj}]、`Widget.Top()` → obj | ✓ | `Top` = 最上层可见、可交互的界面（按视口 ZOrder，再按创建先后）；`Find` 同类开了几个取最上面的 |
| | `Widget.Focused()` → obj | ✓ | 当前键盘焦点所在控件 |
| | `Widget.State(sel)` → {exists, visible, enabled, text, checked, selected} | ✓ | |
| | `Widget.Text(sel)` → string、`Widget.Number(sel)` → number、`Widget.Items(sel)` → [obj]、`Widget.ItemCount(sel)` → int | ✓ | 条目含没滚到的（ListView 按数据对象） |
| | `Widget.Close(sel)`、`Widget.CallFn(sel, fn)` | | 同 `widget.close` / `widget.call` |
| | `Widget.InputMode()` → `game` / `ui` / `gameAndUi`、`Widget.CursorVisible()` → bool | ✓ | 判断现在键盘、鼠标在给谁（5.4 读不到输入模式本身：视口忽略输入算 `ui`，否则光标显示算 `gameAndUi`） |
| 玩家 | `Player.Pawn(client?)` / `Player.Controller(client?)` / `Player.State(client?)` → obj | ✓ | |
| | `Player.Location()` → vec、`Player.Rotation()` → rot | ✓ | |
| | `Player.Teleport(vec, rot?)` | | 准备状态用；走路用 `move` |
| | `Player.Possessed()` → cls | ✓ | |
| 角色移动 | `Character.Velocity(obj?)` → vec、`Character.Speed(obj?)` → float、`Character.IsFalling(obj?)` → bool、`Character.MovementMode(obj?)` → string | ✓ | 默认本地 Pawn |
| Actor | `Actor.Location(obj)` → vec、`Actor.Distance(a, b)` → float、`Actor.Near(cls, point, radius)` → [obj]、`Actor.HasTag(obj, tag)` → bool、`Actor.IsHidden(obj)` → bool | ✓ | |
| | `Actor.Destroy(obj)` | | |
| 动画 | `Anim.IsMontagePlaying(obj, montage?)` → bool、`Anim.CurrentMontage(obj)` → string | ✓ | 技能、交互动作有没有播 |
| 能力系统（可选模块 GameTestGAS，开了 GameplayAbilities 才有；`obj` 必填，本地玩家写 `@pawn`） | `Ability.Attribute(obj, "Set.Attr")` → float、`Ability.HasTag(obj, tag)` → bool、`Ability.IsActive(obj, abilityCls 或 tag)` → bool、`Ability.Cooldown(obj, tag)` → float | ✓ | |
| | `Ability.Activate(obj, abilityCls 或 tag)` → bool、`Ability.ApplyEffect(obj, effectCls, level?)`、`Ability.SetAttribute(obj, "Set.Attr", value)` | | 准备状态用 |
| 世界 | `World.Map()` → string、`World.IsLoaded()` → bool、`World.Streaming(level?)` → {loaded, visible, count} | ✓ | 切图、流送关卡加载完没有 |
| | `World.TimeSeconds()`、`World.RealSeconds()` → float、`World.IsPaused()` → bool、`World.TimeDilation()` → float | ✓ | |
| | `World.Open(map, options?)`、`World.SetPaused(bool)`、`World.SetTimeDilation(x)` | | `Open` 会等到新地图加载完 |
| 网络 | `Net.Mode()` → `standalone` / `listen` / `dedicated` / `client`、`Net.IsServer()` → bool、`Net.PlayerCount()` → int、`Net.Ping(client?)` → float | ✓ | |
| 子系统与全局 | `Subsystem.Get(cls)` → obj | ✓ | |
| | `CVar.Get(name)` → 值 | ✓ | |
| | `CVar.Set(name, value)`、`Console.Run(cmd, client?)` → 输出、`Cheat.Run(cmd)` | | `Cheat.Run` 走 CheatManager（Exec 函数） |
| 配置与数据 | `DataTable.Row(table, row)` → obj、`DataTable.Field(table, row, field)` → 值、`DataTable.Rows(table)` → [string] | ✓ | 测试数据和期望值从配置表取 |
| | `Config.Get(section, key, file?)` → string | ✓ | ini |
| 资产 | `Asset.Exists(path)` → bool、`Asset.Load(path)` → obj | ✓ | |
| 存档 | `SaveGame.Exists(slot, user?)` → bool | ✓ | |
| | `SaveGame.Delete(slot, user?)` | | 重置状态 |
| Lua | `Lua.Get(module, path)` → 值、`Lua.Global(name)` → 值、`Lua.Loaded(module)` → bool | ✓ | 模块按 require 路径；没加载的不主动加载 |
| | `Lua.Call(module, fn, args)` → 值 | | 受 `allowLuaInvoke`；多个返回值给数组 |
| | `Lua.Errors(since?)` → [{message, traceback, at}] | ✓ | 只 Lua 报错（含被 pcall 接住但打了日志的） |
| 日志 | `Log.Mark()` → {log, lua} | | 记下日志和 Lua 报错的当前位置，给 `since` 用 |
| | `Log.Contains(text, category?, since?)` → bool、`Log.Count(text, category?, since?)` → int、`Log.Errors(since?)` → [{category, verbosity, message}] | ✓ | `since` 省略 = 本次 run 开始；Errors 含 Error、Ensure、Lua 报错 |
| 等待 | `Wait.Seconds(n)`、`Wait.Frames(n)`、`Wait.Until(cond, timeoutMs)`、`Wait.Loading(timeoutMs?)` | | `Loading`：等地图、流送关卡加载完，且没有异步加载中的包（`IsAsyncLoading`） |
| 性能 | `Perf.Fps()` → float、`Perf.FrameMs()` → float、`Perf.MemoryMB()` → float | ✓ | 冒烟检查用 |
| 随机 | `Random.Seed(n)` | | `FMath::RandInit` + Lua `math.randomseed`（有 Lua 时），让结果可复现 |
| 截图 | `Screen.Shot(name?)` → path | | 同 `screenshot` 动作 |

现有动作（`click`、`key`、`type`、`move`、`drag`、`wait`、`assert`、`screenshot`、`console`、`gm`…）保持原样；面板把它们和接口一起列在节点面板里。

### 8.2 项目接口

| 写在哪 | 认什么 |
| --- | --- |
| C++ | `_settings.json` `api.cpp` 列的文件夹（相对项目根，默认 `Source/**/GameTestApi`）下头文件里的 `UBlueprintFunctionLibrary` 子类的 `static` + `BlueprintCallable` / `BlueprintPure` 函数。`BlueprintPure` 算只读。注释是说明，`Category` 是分类，`DisplayName` 是显示名 |
| Lua | `api.lua` 列的文件夹（相对 Lua 脚本根，默认 `GameTestApi`）下的模块，模块表上的函数。EmmyLua 注释：`---@param 名 类型 说明`、`---@return 类型 说明`、`---@readonly`（只读）、函数上面的普通注释是说明 |
| 蓝图 | 不认 |

- 接口名：C++ `项目.<类名去掉前后缀>.<函数名>`（`UQuestTestLibrary::AcceptQuest` → `项目.QuestTest.AcceptQuest`），Lua `项目.<模块名>.<函数名>`。`DisplayName` 只是显示，不改名字。
- 扫描：编辑器启动、编译、Live Coding 后，GameTestEditor 扫 C++ 部分（按类元数据 `ModuleRelativePath` 过滤文件夹）；Lua 部分由面板扫。两边写进场景根的 `_api.json`（生成物，进版本库，不手改），按 `src` 分段，谁写谁那一段：

```json
{ "kind": "gametest-api", "version": 1,
  "cpp": { "scannedAt": "…", "folders": ["Source/Demo/GameTestApi"],
           "items": [ { "name": "项目.QuestTest.AcceptQuest", "class": "/Script/Demo.QuestTestLibrary", "function": "AcceptQuest",
                        "category": "任务", "display": "接任务", "note": "…", "readOnly": false, "file": "Source/Demo/GameTestApi/QuestTestLibrary.h", "line": 21,
                        "args": [ { "name": "QuestId", "type": "int32" } ], "returns": "bool" } ],
           "rejected": [ { "class": "…", "function": "…", "why": "参数 OnDone：OnDone 是委托，脚本里给不了" } ] },
  "lua": { "scannedAt": "…", "folders": ["GameTestApi"], "items": [ { "name": "项目.QuestApi.Finish", "module": "GameTestApi.QuestApi", "function": "Finish", … } ], "rejected": [] },
  "renamed": { "旧名": "新名" } }
```

- 运行时按 `class` + `function` 反射调用、按 `module` + `function` 调 Lua；打包的游戏里也能用。`renamed` 里的旧名调用时报「改用 …」。
- `WorldContext` 参数自动填；参数类型不在第 6 节支持范围里的进 `rejected`（UObject 参数支持，调用时传能重找的对象）。
- `args` 每项：`{name, type, constructible, object?, out?, auto?, why?}`——`object: true` 是 UObject 参数（第 6 节的对象写法），`out: true` 是输出参数（不用给，调完进返回值），`auto: "world"` 是自动填的世界参数。`gt.vars` 的函数参数用同一个格式。
- 接口名重了（两个类去掉 `Library` 后缀后同名）时后一个进 `rejected`。扫描结果和原来一样（不比 `scannedAt`）时不写文件，免得每次开编辑器都留一个只改了时间的差异。

## 9. 运行配置新增（`_settings.json`）

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `guards` | `[]` | 全局守护 |
| `api` | `{"cpp": ["Source/**/GameTestApi"], "lua": ["GameTestApi"]}` | 项目接口文件夹 |
| `objectKeys` | `[]` | 对象定位的默认标识 |
| `allowLuaInvoke` | `true` | 步骤能调 Lua 函数 |
| `allowLuaInCond` | `false` | 条件、表达式能调 Lua 函数 |
| `maxCallDepth` | `8` | `call` 最多嵌套 |
| `blockers` | — | 已废弃，见 5.1 |

场景根的 `_vars.json`：`{"常用名": 判断对象}`，条件里 `alias` 引用。

## 10. 操作（`/rpc`，新增和变化）

| 操作 | 参数 | 返回 |
| --- | --- | --- |
| `gt.run`（变化） | 原有 + `params`（对象）、`from` / `to`（步骤 id：从哪一步开始、跑到哪一步停下暂停）、`breakpoints`（id 数组）、`pauseOnFail`（失败时暂停等人看，默认 false） | 同原来 |
| `gt.debug` | `action`：`pause` / `resume` / `step`（单步：执行下一个节点后暂停；块算一个节点时用 `into: true` 进块）/ `breakpoints`（替换为 `ids`） | `state`（`running` / `paused` / `idle`）、`at`（暂停所在步骤 id 和路径）、`vars`（此刻变量，截到 4000 字） |
| `gt.step`（变化） | `step` 可以是块（`if` / `loop` / `group` / `call`…）、可带 `vars` / `params` | 单步同原来；块：后台跑成临时 run，回 `runId` |
| `gt.resolve`（变化） | 原有 + `conds: [{id?, cond}]`、`exprs: [{id?, expr}]`、`vars` | `conds: [{id, holds, parts: [{path, holds, actual, unresolved?}]}]`、`exprs: [{id, value?, type?, unresolved?, error?}]`。不等待、不稳住，只看这一刻 |
| `gt.validate` | `scenario`（路径）或 `script`（整份 JSON）、`deep`（默认 true：查函数、选择器、路径，需要编辑器或游戏进程） | `ok`、`errors` / `warnings`：`[{id?, path, code, message}]`、`checked: {steps, calls, functions, selectors, exprs}`。不开游戏也能跑（选择器查 WBP 资产，运行时生成的条目只查到容器） |
| `gt.vars` | `scope`：`widget`（`target` 或 `class`）/ `class`（`name`）/ `subsystem`（`name`，省略列全部子系统）/ `player`（`which`：`gi gs gm ps pc pawn hud`）/ `object`（定位器）/ `lua`（`module`）；`values`（游戏开着时带此刻的值）、`functions`（默认 true）、`filter`、`offset` / `limit`（默认 60，最多 200） | `owner: {class, asset?, luaModule?}`、`groups: [{src: "cpp" / "bp" / "lua", title, class?, asset?, file?, items: [{name, path, type, kind: "prop" / "fn", readOnly, sig?, args?: [{name, type, constructible, why?}], callable?, why?, note?, line?, value?, dim?}]}]`、`total`、`hasMore`、`nextOffset`、`truncated?`。组顺序固定 C++ → 蓝图 → Lua；C++ 组按声明类分小节（离它最近的在前，引擎基类 `UUserWidget` / `AActor` / `UObject` 标 `collapsed`）；`dim`：像动画、计时（Anim / Time / Tick / Hover）的。Lua 组只有游戏开着时由实例表给出（文件扫描由面板做、按同样结构合并）。`subsystem` 不写 `name` 时返回 `subsystems: [{name, path, type, kind: "obj"}]`（游戏开着时）。最多 12000 字 |
| `gt.widgets` | `asset`（WBP 路径）或 `class`；`filter`、`offset` / `limit`（默认 200） | `asset`、`class`、`parent`（原生父类）、`selector`（整个屏 `WBP_X:.`）、`treeFrom?`（控件树继承自父 WBP 时是父 WBP 的路径）、`luaModule?`、`nodes: [{name, class, selector, depth, isVariable, text?, visibility?, container?: {entryClass?}, userWidget?}]`（`userWidget: true` 是嵌套的 WBP，里面的控件用 `>` 进去，再按它的类调一次）、`total`、`hasMore?`、`nextOffset?`、`truncated?`。只在编辑器进程（GameTestEditor 模块）。读资产的 WidgetTree，不用开游戏。最多 12000 字 |
| `gt.api` | `query`、`category`、`src`（`builtin` / `cpp` / `lua`）、`readOnly`、`offset` / `limit`（默认 60） | `items: [{name, category, src, display?, sig, args, returns, readOnly, waits?, note?, file?, line?}]`、`rejected`（`src` 不是 builtin 时；不写 `src` 时有就带）、`total`、`hasMore`、`nextOffset?`、`truncated?`、`loadError?`（`_api.json` 读不了，自带接口照常列）。`_api.json` 改了（面板写 Lua 段）按修改时间自动重读 |
| `gt.api.scan` | `dryRun` | 编辑器进程：重扫 C++ 项目接口，写 `_api.json` 的 `cpp` 段。`counts: {items, rejected}`、`path`、`changed`（内容变没变，没变不写文件）、`folders`、`rejected`（前 30 条，含原因） |

### 推送

| 主题 | 什么时候 | 字段 |
| --- | --- | --- |
| `gt.flow`（新） | 节点开始 / 结束、循环换轮、守护触发 / 结束、暂停 / 继续 | `runId`、`phase`（`enter` / `leave` / `iter` / `guard` / `guardEnd` / `pause` / `resume`）、`id`、`path`、`status?`（`passed` / `failed` / `skipped` / `already`）、`iter?`、`guard?`、`ms?`、`vars?`（只在 `pause` 时） |
| `gt.job`（变化） | 同原来 | 多 `at`（当前步骤 id）、`guards`（{id: 次数}，面板画在守护右上角的圆底角标上，超过 999 显示 999+） |

## 11. 报告的变化

- 每步 `id`、`path`；块步骤有 `children`（块里执行过的步骤，循环按轮 `iter`）。
- `payload.guards`、`payload.invoke`、`payload.object`、`payload.cond`（判断的每一项：`path`、`holds`、`actual`、`unresolved?`）、`payload.vars`（这一步用到的变量值）。
- 历史（`history.json`）按 id 记失败步骤；旧记录按序号显示。

## 12. 错误码（`gt.validate` 和运行失败的 `code`）

`unknown_field`、`unknown_action`、`bad_type`、`dup_id`、`loop_no_max`、`call_missing`、`call_cycle`、`call_depth`、`var_undefined`、`param_missing`、
`expr_syntax`、`expr_type`、`expr_not_readonly`、`path_missing`、`unresolved`、`fn_missing`、`fn_not_callable`、`arg_missing`、`arg_type`、`arg_object`（对象参数来源不可重找）、
`api_missing`、`api_renamed`、`selector_missing`、`object_not_found`、`object_ambiguous`、`guard_max`、`guard_stuck`、`lua_disabled`、`blockers_removed`、`schema_too_new`。

## 13. 版本

- GameTest 1.9.0：本文全部（GameTestEditor 模块里的 `_api.json` 扫描、`gt.widgets` 读资产也在 1.9.0）。
- 需要 UELink ≥ 2.11.1（调函数复用 `ue.call` 的参数打包）。
- 面板 UE 插件按 `gt.health.version` 判断：低于 1.9.0 时编辑页只读、提示升级。

## 14. 代码结构（扩展点）

用户要求（2026-11-18）：模块化、框架化，方便后续扩展。新能力一律**注册**到对应的表里，不在执行器、条件、表达式的主流程里加 `if/else` 分支。

| 扩展点 | 接口 | 注册表 | 一个实现一个文件 |
| --- | --- | --- | --- |
| 块（`if` / `loop` / `call` / `group` / `set` / `break`…） | `IGameTestBlock`：`Validate`、`Enter`、`Tick`（返回 继续 / 进子节点 / 完成 / 失败）、`Leave` | `FGameTestBlockRegistry`（按 `action` 名） | `Core/Flow/Blocks/GameTestBlock<名字>.cpp` |
| 条件判断（`onScreen`、`number`、`field`、`value`、`api`、`expr`…） | `IGameTestCheck`：`Keys`、`Validate`、`Evaluate(ctx) → {holds, actual, unresolved}`、`IsNegative` | `FGameTestCheckRegistry` | `Observe/Checks/GameTestCheck<名字>.cpp` |
| 取值来源（`@gi` `@sub` `@actor` `@luaMod` `@cvar`…） | `IGameTestValueSource`：`Prefix`、`Resolve(path, ctx) → FGameTestValue` | `FGameTestValueSourceRegistry` | `Observe/Sources/GameTestSource<名字>.cpp` |
| 表达式内置函数 | `FGameTestExprFunction`：名字、参数个数、只读、`Call` | `FGameTestExprRegistry` | `Core/Expr/Functions/GameTestExpr<分类>.cpp` |
| 自带接口 | `IGameTestApi`：名字、分类、参数、返回、只读、`Call(args, ctx)`（可返回「还在等」） | `FGameTestApiRegistry`（项目接口也注册进来，来自 `_api.json`） | `Api/Builtin/GameTestApi<分类>.cpp`（一个分类一个文件） |
| 可选依赖（GAS、CommonUI、EnhancedInput…） | 可选模块启动时往上面的注册表里加 | — | `GameTest<依赖>` 模块 |

- 统一的值类型 `FGameTestValue`（数字、字符串、布尔、null、数组、对象、向量、对象弱引用、unresolved + 原因），条件、表达式、接口、invoke 都用它，不各自转 JSON。
- 运行上下文 `FGameTestFlowContext`：变量作用域栈、参数、`objects`、当前世界 / 客户端、守护栈、报告写入器，显式传进各扩展点，不用全局变量。
- 执行器只认「节点」：栈帧 = {块实现, 块状态, 子节点位置}；等待、守护、超时、`onFail`、报告、推送是执行器的公共逻辑，块不自己处理。
- 每个扩展点的注册表能列出全部实现（名字、说明、参数），`gt.actions`、`gt.api`、`gt.validate` 和面板的节点面板都从注册表生成，不另写清单。
- 面板 UE 插件同理：节点类型、条件编辑行、接口选择器按「一种一个文件 + 注册表」写（`gt-script/blocks/*.ts`、`gt-cond/checks/*.tsx`），数据层 `gt-script.ts` 只做 v3 读写和结构操作，不含界面。
