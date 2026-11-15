# UEKit

放进 Unreal Engine 项目 `Plugins/` 的插件合集。用 [Agent 面板](https://github.com/fixcode-h/agent-panel) 的 UE 页「UE 插件」可以按项目一键安装、升级、卸载；也可以从 [Releases](https://github.com/fixcode-h/UEKit/releases) 下载源码包，解压到项目的 `Plugins/` 后完整编译。

| 插件 | 说明 | 引擎 |
| --- | --- | --- |
| [UELink](plugins/UELink) | 让本机工具连上 UE 进程：控制台、GM、对象属性、DataTable、资源和蓝图读写、日志、性能、Lua。Agent 面板和 UEToVS 共用，其他插件也通过它和面板通信 | 5.4+ |
| [AssetForge](plugins/AssetForge) | 用 JSON 配置管理 UMG、蓝图、行为树、状态树、材质、Niagara 等资产：导入、导出、演练、同步状态、属性差异 | 5.8 |
| [GameTest](plugins/GameTest) | 用 JSON 场景写游戏测试：按选择器操作界面、注入输入、等待、断言、截图；录制真人操作再回放 | 5.8 |

插件目录（客户端查更新读这份）：[catalog.json](catalog.json)。

开发约定见 [AGENTS.md](AGENTS.md)。
