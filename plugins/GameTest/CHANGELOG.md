# GameTest 更新日志

格式说明见仓库根目录的 `docs/changelog.md`（`scripts/check.mjs` 和发布流程依赖这个格式）。

## v1.0.0-beta.1

> 从原项目的 TkGameTest 搬进 UEKit 后的第一版，只支持 UE 5.8。还没在本地用 UE 编译过，先发预发布（客户端查更新只认正式版）。

### 新功能
- **录制回放** — 在游戏窗口里操作一遍，录成场景文件（`Config/GameTest/recorded/`），回放就是跑这份场景。点按钮录成「点哪个控件」而不是坐标，界面布局变了也能回放；改输入框、下拉框、滑条录最终值；点在游戏画面上和按键盘、手柄键录成真实按键，节奏用等待还原。录的时候可以插检查点（顺带截图），之后在面板里改成断言。用引擎公开的输入预处理器实现，不改引擎和项目代码
- **按键动作 `key`** — 按下、松开或点按一个键盘键、鼠标键，走和真实按键一样的输入路径，不需要知道项目的输入资产
- **在 Agent 面板里跑测试** — 场景列表、运行、逐步结果、截图、录制都在面板 UE 页的 GameTest 页；Agent 通过面板的 `ue_gametest` 工具用同一套操作。运行进度实时推到面板

### 改进
- **通信交给 UELink** — 去掉自己开的 HTTP 服务和端口设置（原来没有令牌，游戏进程里默认关着），操作挂在 UELink 的 `/rpc` 上（`gt.*`），共用令牌、只听本机；游戏进程里装了 UELink 就能用
- **CommonUI、GAS、EnhancedInput 拆成可选模块** — `stack.push/pop`、`ability.input`、`input.start/stop` 搬进各自的模块，主模块不再依赖这几个插件；动作清单里会标出哪些动作因为缺插件不可用，写了这些动作的场景会提示要开哪个插件
- **动作登记表** — 动作的说明、参数和实现在一处登记，不会再出现「清单里有、实现里没有」
- **返回内容有上限** — 快照、报告超过 12000 字时写进 `Saved/GameTest/responses/`，只返回路径，不撑爆 Agent 的上下文
- **场景可以自带步间隔** — 场景文件写 `stepDelayMs` 时覆盖项目设置（录制的场景写 0）
- **去掉原项目的名字** — 控制台命令改成 `GameTest.Step / Health / Actions`，测试名改成 `GameTest.*`，默认场景目录改成 `Config/GameTest`；测试探针只用插件自带的 C++ 类，不引用项目资产

### 注意
- 需要 UELink 2.2.0-beta.1 或更新的版本
- 编辑器里的 GameTest 窗口去掉了，改用 Agent 面板
- 测试探针控件挪到了 `GameTestTests` 模块（只在编辑器里加载），引用原来 `TkGameTest` 探针类的蓝图要重新指定父类
- `.uplugin` 把 CommonUI、GameplayAbilities、EnhancedInput 列成可选依赖：它们是引擎自带插件，启用 GameTest 时会一起打开
- 装好后要在本地完整编译一次（Live Coding 不够）
