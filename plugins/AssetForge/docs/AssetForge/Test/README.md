# AssetForge 示例测试界面

AssetForge 插件自带的一套多页面 UMG 界面，用来练习和验证 GameTest 录制回放。编辑器启动时 AssetForge 把这个目录拷进项目的配置根 `Test/`（配套场景另拷进 GameTest 场景根的 `AssetForgeSample/`），在 Agent 面板 UE 页的 **AssetForge** 页就能看到这些配置。

## 用法

1. AssetForge 页左栏的 `Test/` 下有两份配置，都导入（先演练再导入也行）：
   - `umg/WBP_ForgeSample.json` → `/Game/AgentTest/AssetForge/WBP_ForgeSample`（界面）
   - `map/Map_ForgeSample.json` → `/Game/AgentTest/AssetForge/Map_ForgeSample`（只有世界设置的空地图，GameMode 进图就显示界面）
2. 打开 `Map_ForgeSample` 点 PIE，界面盖满屏幕、鼠标可以直接点。在别的地图里 PIE 时，控制台敲 `AssetForge.Sample` 也能打开 / 关掉它。
3. GameTest 页：点「录制」开始录；或者直接跑（这两个场景用 GameTest 1.9 的脚本写法：分段、参数和变量、表达式断言、循环、守护，要 GameTest 1.9.0 起） `AssetForgeSample/ForgeSample-冒烟.json`（跳页、叠两层弹窗、返回、下拉框、输入框、多层返回）、`AssetForgeSample/ForgeSample-条目标识.json`（按条目标识点列表，见下面「列表条目页」）。

项目的默认世界设置类不是 `WorldSettings` 时，导入地图会报「世界设置类不一致」：把 `map/Map_ForgeSample.json` 的 `root.class` 换成项目用的那个类。

## 有什么

侧栏七个页（首页、背包、商店、任务、列表、设置、资料）+ 一个只能从任务列表进去的「任务详情」页；背包、任务、设置各有子页签；九个弹窗（购买确认上还能再叠一层「帮助」）；列表（公告、商店、任务）、物品格子、复选框、滑条、下拉框、输入框、进度条。顶栏的 `Crumb` 显示当前页、`Status` 显示最近一次操作的结果，断言这两个就能确认点对了。

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
| 滑条、下拉框、复选框 | 值写进 `<控件名>_Value` |

示例资产依赖只在编辑器里加载的测试模块，不要打进包：把 `/Game/AgentTest` 加进项目设置「打包 → 永不烘焙的目录」。

## 改这套界面

配置由 UEKit 仓库的 `docs/asset-forge-sample/gen.mjs` 生成（600 多个控件，手改容易漏改名字和场景里的选择器）。只是自己项目里试着改，直接改这里的 JSON 也可以：改过的文件插件升级时不会被覆盖；删掉的文件不会再装回来（想重装就删掉 `Saved/AssetForge/samples.json`）。不想要示例：项目设置 → 插件 → Asset Forge →「安装示例测试界面」关掉。
