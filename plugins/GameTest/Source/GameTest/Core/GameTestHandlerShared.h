// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/GameTestRunner.h"

class FJsonObject;
struct FGameTestWidgetNode;

/**
 * 动作实现的跨文件契约。
 *
 * **加一个动作 = 在对应子域的 handler 文件里写一个函数 + 在这里声明 + 在 `GameTestAction.cpp`
 * 的 `MakeBuiltinActions` 里登记一行（说明和实现一起）。** 可选模块的动作在模块自己的
 * StartupModule 里 `GameTestActions::Register`，不进这张表。
 * 共享辅助的实现在 `GameTestHandlers.cpp`。
 *
 * 实现按子域拆在四处（`Observe/`、`Operate/`、`Input/`、`Editor/`），这里放**真正跨文件**的东西：
 * 各 handler 的声明，以及被多个子域共用的目标解析辅助。可选模块（GameTestCommonUI 等）也 include 它，
 * 所以辅助函数带导出宏。
 *
 * 只被单个文件用到的辅助（列表条目定位、actor 查找）留在各自 `.cpp`
 * 的匿名 namespace 里，不外泄符号——不要把"只在一处用"的东西搬进来。
 *
 * **注意 unity build**：本模块的 `.cpp` 会被合并进同一翻译单元，所以任何跨文件同名的东西
 * （常量、辅助函数、类型）都只能有一份定义——各文件在自己的匿名 namespace 里各写一份，
 * 合并后就是重定义。共享的东西放这里，或给不同名字。
 */
namespace GameTestHandlers
{
	/**
	 * 动作返回值的短名。
	 *
	 * **必须只有这一处定义**：UE 的 unity build 会把同一模块的多个 `.cpp` 合并成一个翻译单元，
	 * 各文件在自己匿名 namespace 里各写一份同名常量会撞成重定义（实测踩过：拆分 handler 后
	 * 5 个文件各定义一份，直接编不过）。`inline constexpr` 让它们既只有一份实体、又能被
	 * 各翻译单元共用。
	 */
	inline constexpr EGameTestStepStatus StepSucceeded = EGameTestStepStatus::Succeeded;
	inline constexpr EGameTestStepStatus StepFailed = EGameTestStepStatus::Failed;
	inline constexpr EGameTestStepStatus StepPending = EGameTestStepStatus::Pending;

	/** 快照的节点上限：超过就标记 truncated，避免一次返回把上下文撑爆。 */
	inline constexpr int32 MaxSnapshotNodes = 1000;

	/**
	 * 选择器 / `@句柄` → 控件节点。失败时 OutMessage 里是候选清单。
	 *
	 * 跨文件共享是必须的：观察（find/snapshot/probe）与交互（click/select/set/stack…）都要用它，
	 * 各写一份必然漂移。
	 *
	 * 给了录制线索 `InHint`（步骤的 `hint`）时，选择器命中同一个 WBP 的几个实例不直接失败，
	 * 按线索挑回录的那一个（`GameTestHintPick`），怎么挑的写进 `OutPickNote`；
	 * 唯一命中但文字和录制时对不上时，在同容器里换成文字对得上的那个（`GameTestHintPick::Rematch`）。
	 */
	GAMETEST_API bool TryResolveTarget(
		const FString& InTarget,
		TMap<FString, TWeakObjectPtr<UWidget>>* InHandles,
		TSharedPtr<FGameTestWidgetNode>& OutNode,
		FString& OutMessage,
		const FJsonObject* InHint = nullptr,
		FString* OutPickNote = nullptr);

	/**
	 * 解析这一步的 target，顺便记下"碰过谁"——视口高亮靠它把控件框出来。
	 *
	 * 所有以 target 为靶心的动作都该走这里而不是直接调 `TryResolveTarget`：
	 * 漏一处，那类动作在游戏窗口里就只会静默地没有高亮。
	 * 步骤带录制线索（`hint`）时一并用来消歧，挑的理由写进结果的 `picked`。
	 */
	GAMETEST_API bool TryResolveStepTarget(FGameTestStepContext& InContext, TSharedPtr<FGameTestWidgetNode>& OutNode, FString& OutError);

	/**
	 * "可见"= 没有 Hidden/Collapsed；HitTestInvisible 这类仍是看得见的。
	 *
	 * `assert` 的 `visible` 条件与将来任何"看得见吗"的判断都要用它，一致的定义只能有一份。
	 */
	bool IsActuallyVisible(const FGameTestWidgetNode& InNode);

	//~ 跨子域共用：步骤字段 → 世界寻址 ---------------------------------------

	/**
	 * 读步骤里的 `client` 字段（`"server"` 或从 0 起的客户端序号；字符串与数字都接受）。
	 * 字段缺省不报错——由 `ResolveStepWorld` 决定缺省世界。
	 */
	GAMETEST_API bool TryReadClientField(const FJsonObject& InParams, FString& OutClientText, bool& bOutSpecified, FString& OutError);

	/** 步骤 → 目标世界：`client` 缺省时取服务器世界（多世界）或唯一的世界（单机）。 */
	GAMETEST_API UWorld* ResolveStepWorld(const FGameTestStepContext& InContext, FString& OutError);

	//~ handler：每个子域一个文件 ----------------------------------------------

	// 观察（Observe/GameTestHandlersObserve.cpp）
	EGameTestStepStatus HandlePing(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleMark(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleFind(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleSnapshot(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleScreenshot(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleConsole(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleProbe(FGameTestStepContext& InContext);

	// 等待与断言（Observe/GameTestHandlersExpect.cpp）
	EGameTestStepStatus HandleWait(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleAssert(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleNetCompare(FGameTestStepContext& InContext);

	// 交互（Operate/GameTestHandlersOperate.cpp）
	EGameTestStepStatus HandleClick(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleType(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleSelect(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleScroll(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleSet(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleWidgetAdd(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleWidgetRemove(FGameTestStepContext& InContext);

	// 真实指针的 UI 动作（Operate/GameTestHandlersPointer.cpp）：不认控件类，等可操作后发真实输入
	EGameTestStepStatus HandleClickPointer(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleClickCode(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleDrag(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleWheel(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleTypeKeys(FGameTestStepContext& InContext);

	// 游戏内：光标与 actor（Input/GameTestHandlersInput.cpp）；技能和输入注入在可选模块里
	EGameTestStepStatus HandleInputCursor(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleKey(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleActorGet(FGameTestStepContext& InContext);

	// 编辑器专属（Editor/GameTestHandlersEditor.cpp）
	EGameTestStepStatus HandleEditorEnsure(FGameTestStepContext& InContext);
	EGameTestStepStatus HandlePieStart(FGameTestStepContext& InContext);
	EGameTestStepStatus HandlePieStop(FGameTestStepContext& InContext);
	EGameTestStepStatus HandleLevelLoad(FGameTestStepContext& InContext);
}
