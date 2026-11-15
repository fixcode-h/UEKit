// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FGameTestRecorderInput;

/**
 * 录制：把人在游戏窗口里的操作记成一份场景（步骤 JSON），回放就是跑这份场景。
 *
 * **怎么录**：在 Slate 上挂一个 `IInputProcessor`（引擎公开的输入预处理扩展点），只看、不吃掉事件，
 * 不改引擎和项目代码。只记发给游戏视口窗口的输入，编辑器其他面板上的操作不录。
 *
 * **录成什么**：尽量录成「对哪个控件做了什么」，而不是坐标——
 * - 点到 UMG 的按钮 / 复选 / 菜单锚 → `click`，目标是选择器（`屏名:路径`），界面布局变了也能回放；
 * - 改了输入框、下拉框、滑条 → 离开它时录一条 `type` / `set`（录最终值，不录每个按键）；
 * - 没点到 UMG（点在游戏画面上）→ `input.cursor` 摆好位置 + `key LeftMouseButton`；
 * - 键盘 / 手柄键 → `key`（短按录成 tap，长按录成 press … release）；
 * - 两次操作之间隔得久就插一条 `wait seconds`，回放的节奏和录的时候一样。
 *
 * 回放时 `key` 发出的合成输入也会经过这个处理器，靠 `GameTestInput::IsSendingSynthetic` 跳过，不会录进去。
 * 只在游戏线程上用。
 */
class GAMETEST_API FGameTestRecorder
{
public:
	static FGameTestRecorder& Get();

	FGameTestRecorder();
	/** FPendingEdit 只在 .cpp 里定义，析构必须放在 .cpp，否则 TUniquePtr 删除不完整类型。 */
	~FGameTestRecorder();

	/**
	 * 开始录制。`InName` 是场景名（也用来起文件名）。
	 * `bInStartPie`：编辑器里 PIE 正在跑时，在开头加一条 `pie.start`（同一张地图），回放时自己把游戏拉起来。
	 */
	bool Start(const FString& InName, bool bInStartPie, FString& OutError);

	/** 插一条说明（`mark`），可以顺带截一张图；之后在面板里把它改成断言。 */
	bool Mark(const FString& InText, bool bInScreenshot, FString& OutError);

	/**
	 * 停止录制。`bInSave` 时写成场景文件（场景根目录下 `recorded/<名字>.json`，重名加序号）。
	 * OutResult：`path`（相对场景根）、`absolutePath`、`steps`（步数）、`preview`（前几步）。
	 */
	bool Stop(bool bInSave, TSharedPtr<FJsonObject>& OutResult, FString& OutError);

	bool IsRecording() const
	{
		return bRecording;
	}

	/** 录制状态：是否在录、场景名、已录步数、已录秒数、最近几步。 */
	TSharedRef<FJsonObject> DescribeStatus(int32 InLastSteps) const;

	/** 模块关闭时调：还在录就丢掉，摘掉输入处理器。 */
	void Shutdown();

	//~ 输入处理器回调（游戏线程）。
	void OnKey(const struct FKey& InKey, bool bInDown);
	void OnMouseButton(const struct FKey& InButton, bool bInDown, const FVector2D& InScreenPosition);

private:
	struct FPendingEdit;

	/** 两次操作之间隔得久，补一条 wait。 */
	void AddGapWait();
	void AddStep(const TSharedRef<FJsonObject>& InStep);

	/** 离开输入框 / 下拉框 / 滑条时，把它的最终值录成 `type` / `set`。 */
	void FlushPendingEdit();

	/** 光标下的 UMG 控件 → 选择器（`屏名:路径`）。没点到 UMG 时返回 false。 */
	bool LocateWidgetUnderCursor(const FVector2D& InScreenPosition, TSharedPtr<FJsonObject>& OutWidgetInfo) const;

	void Broadcast(const TCHAR* InPhase) const;

	bool bRecording = false;
	FString Name;
	double StartedAt = 0.0;
	double LastEventAt = 0.0;
	TArray<TSharedRef<FJsonObject>> Steps;
	bool bTruncated = false;

	/** 按下的鼠标键落在哪个控件上（松开时还在同一个控件上才算一次点击）。 */
	FString MouseDownTarget;
	bool bMouseDownOnGame = false;

	/** 按下还没松开的键和按下的时刻（判断短按还是长按）。 */
	TMap<FName, double> KeyDownAt;

	TUniquePtr<FPendingEdit> PendingEdit;
	TSharedPtr<FGameTestRecorderInput> InputProcessor;
};
