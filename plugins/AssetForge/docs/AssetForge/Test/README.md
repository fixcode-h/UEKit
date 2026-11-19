# AssetForge 示例测试界面

AssetForge 插件自带的一套多页面 UMG 界面，用来练习和验证 GameTest 录制回放。编辑器启动时 AssetForge 把这个目录拷进项目的配置根 `Test/`（配套场景另拷进 GameTest 场景根的 `AssetForgeSample/`），在 Agent 面板 UE 页的 **AssetForge** 页就能看到这些配置。

## 用法

1. AssetForge 页左栏的 `Test/` 下导入这两份（先演练再导入也行）：
   - `umg/WBP_ForgeSample.json` → `/Game/AgentTest/AssetForge/WBP_ForgeSample`（界面外壳；每一页是 `umg/pages/` 下的一个子界面，导入外壳时会先把它们导入成 `/Game/AgentTest/AssetForge/Pages/WBP_ForgeSample_<页>`）
   - `map/Map_ForgeSample.json` → `/Game/AgentTest/AssetForge/Map_ForgeSample`（只有世界设置的空地图，GameMode 进图就显示界面）
2. 打开 `Map_ForgeSample` 点 PIE，界面盖满屏幕、鼠标可以直接点。在别的地图里 PIE 时，控制台敲 `AssetForge.Sample` 也能打开 / 关掉它。
3. GameTest 页：点「录制」开始录；或者直接跑 `AssetForgeSample/` 下的场景（GameTest 1.9 的脚本写法，要 GameTest 1.9.0 起）：

| 场景 | 练什么 |
| --- | --- |
| `ForgeSample-冒烟` | 跳页、叠两层弹窗、返回、下拉框、输入框、连按返回；参数、变量、表达式断言、loop until、块里的守护 |
| `ForgeSample-条目标识` | 按条目标识点列表（见下面「列表条目页」），`select` 按数据选中、`scroll` 按序号滚动、`Widget.ItemCount` |
| `ForgeSample-控件操作` | 「控件」页的每种控件：`set`（复选框、滑条、微调框、下拉框、切换器、菜单开合、按键）、`type`（按键 / 代码 / 追加 / 回车提交）、`scroll`、禁用到可用、菜单项（另一个界面里的控件）；`ping`、`mark`、`find` 句柄、`snapshot`、`probe`、标注截图、`widget.add` / `widget.remove` |
| `ForgeSample-流程脚本` | 「流程」页：loop 四种写法和 break / continue、if / elif / else、`invoke` 调界面函数、对象定位器、`api` 和表达式函数、skipIf + also、`wait until=cond`、守护处理定时弹出的公告、块级重试和时限、`expectNoErrors`、`disabled`、`widget.call`、`return` |
| `ForgeSample-真实输入` | 真实鼠标和键盘：悬停、按住 1.3 秒蓄力、拖滑条、滚轮、点开下拉框选项、菜单、退格和回车、按键选择器。会真的移动光标，跑的时候别动鼠标 |
| `common/打开示例界面` | 上面几份都先 `call` 它：进图、重置界面、切到要测的页，返回面包屑文字 |

项目的默认世界设置类不是 `WorldSettings` 时，导入地图会报「世界设置类不一致」：把 `map/Map_ForgeSample.json` 的 `root.class` 换成项目用的那个类。

## 有什么

侧栏九个页（首页、背包、商店、任务、列表、控件、流程、设置、资料）+ 一个只能从任务列表进去的「任务详情」页，每一页是一个子 WBP；背包、任务、设置各有子页签；十个弹窗（购买确认上还能再叠一层「帮助」，「活动公告」由流程页定时弹出）；列表（公告、商店、任务）、物品格子、复选框、滑条、下拉框、输入框、进度条。顶栏的 `Crumb` 显示当前页、`Status` 显示最近一次操作的结果，断言这两个就能确认点对了。

## 列表条目页

侧栏「列表」进去的一页，给 GameTest 1.4.0 起的「这一格怎么认」当靶子（录制时点到列表、格子里的条目，GameTest 挑一个能唯一认出这一条的字段写进选择器，拿不准时暂停游戏、在 Agent 面板上弹窗问你）：

| 容器 | 条目 | 录制时 |
| --- | --- | --- |
| 仓库 `ItemList`（TileView，48 格） | 数据对象 `ForgeSampleItem`：`ItemId`（1001 起，不重复）、`Name`（12 种，每种 4 格）、`Count`（点「使用」会变）、`Quality` | 按 `ItemId` 自动认，不弹窗（GameTest 设置里改成「每次都暂停问我」才弹） |
| 邮件 `MailList`（ListView，10 封） | 数据对象 `ForgeSampleMail`：`SentAt`（不重复，但名字不像标识）、`Title`、`Sender`、`Reward`（都重复）、`bClaimed`（点「领取」会变） | 拿不准，暂停问；推荐 `SentAt` |
| 技能栏 `SkillBar`（WrapBox，8 格） | 运行时 `CreateWidget` 再加进去的 `ForgeSkillCell`：`SkillType`（11–14）、`Level`（1–2），各自都重复 | 单个字段都认不出，暂停问；推荐 `SkillType` + `Level` 组合 |

在弹窗里勾「以后都按这个认」，规则存进场景根的 `_settings.json`，之后再点同类条目不再问。

## 跳页怎么实现的

界面的父类是 `ForgeSampleScreen`（AssetForge 测试模块里的 C++ 类），**按控件名约定**接线，所以整套界面只用 JSON 描述、不需要蓝图图表和 UnLua。`__` 后面的部分只用来让名字不重复（`Go_Page_Shop__Hero`）：

| 控件名 | 作用 |
| --- | --- |
| `Go_<目标>` | 切到某个 WidgetSwitcher 里叫这个名字的子控件；切的是 `Pages` 时记返回历史、关掉弹窗。当前页 / 页签的按钮自动高亮 |
| `Back` | 先关最上面的弹窗，没有弹窗就回上一页 |
| `Open_Popup_<名>` / `Close_Popup_<名>` | 打开 / 关掉弹窗 |
| `Pick_<文字控件>` | 把按钮文字写进那个 TextBlock（公告已读、物品详情） |
| `Submit_<输入框>` | 把输入框的文字写进 `<输入框>_Echo` |
| 按钮的提示文字 | `Go_` / `Open_` / `Pick_` 时写进 `<目标>_Subject`（「购买：红宝石」「任务详情：熔火之心」） |
| 滑条、微调框、下拉框、复选框、按键选择器 | 值写进 `<控件名>_Value`；滑条还更新进度条 `<控件名>_Bar` |
| `Enable_<控件>`（复选框） | 勾上让那个控件可用，取消就禁用 |
| `Hold_<名>` | 按住到松开：写 `<名>_Value`「蓄力完成」（按满 1 秒）或「短按」 |
| `Hover_<名>` | 鼠标移上 / 移开：写 `<名>_Value`「悬停中」/「未悬停」 |
| `Menu_<菜单锚点>` | 开 / 关菜单；菜单内容是 C++ 界面 `ForgeSampleMenu`（复制、重命名、删除，点了写状态栏「菜单：…」并关菜单） |
| `Inc_` / `Dec_` / `Reset_<文字控件>` | 数字 +1 / -1 / 归零（`Counter` 同时写界面的 `Counter` 属性，脚本可以 `invoke` `AddCounter`、`SetCounter`、`GetCounter`） |
| `Claim_<名>` | 领取：按钮字变「已领取」，状态栏写「领取「名」（共领了 N 个）」，再点只说已经领过（`ClaimCount`、`IsClaimed(名)` 可读） |
| `Toggle_<控件>` | 显示 / 隐藏那个控件 |
| `Delay_<控件>` / `Arm_<弹窗>` | 1.5 秒后显示那个控件 / 1 秒后弹出那个弹窗（给等待和守护当靶子） |

`ResetScreen()` 回首页、关弹窗并调 `ResetFlow()`（清计数、领取记录、定时器，把 `Reveal_*` 收起来），示例场景开头都调它。

示例资产依赖只在编辑器里加载的测试模块，不要打进包：把 `/Game/AgentTest` 加进项目设置「打包 → 永不烘焙的目录」。

## 改这套界面

配置由 UEKit 仓库的 `docs/asset-forge-sample/gen.mjs` 生成（近 900 个控件，手改容易漏改名字和场景里的选择器）。控件名在外壳和所有子页面里都不能重名：父类按名字跨子界面找控件。只是自己项目里试着改，直接改这里的 JSON 也可以：改过的文件插件升级时不会被覆盖；删掉的文件不会再装回来（想重装就删掉 `Saved/AssetForge/samples.json`）。不想要示例：项目设置 → 插件 → Asset Forge →「安装示例测试界面」关掉。
