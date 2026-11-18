// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectKey.h"

class FJsonObject;
class FJsonValue;
class SWidget;
class UUserWidget;
class UWidget;

/** 录制时推断跳过条件的模式（`gt.record.start inferSkip`）。 */
enum class EGameTestInferSkip : uint8
{
	/** 高把握（≥ AutoScore）的直接写成 skipIf，其余只列候选。 */
	Auto,
	/** 只列候选（hint.state），不写 skipIf（默认，1.8.0 起；用户 2026-11-18 定「默认不跳过，需要用户设置」）。 */
	Suggest,
	/** 不看状态。 */
	Off,
};

/** 点一下前后变了的一处：可以写成跳过条件的一个候选。 */
struct GAMETEST_API FGameTestStateCandidate
{
	/** 条件看的控件；空 = 就是这一步点的那个控件。 */
	FString Target;
	/** checked / text / visible / enabled / image / field。 */
	FString Kind;
	/** 条件字段（`{"checked": true}`、`{"textLike": "已领取"}`、`{"field": "@self.bClaimed", "equals": true}`）。 */
	TSharedPtr<FJsonObject> Cond;
	FString Before;
	FString After;
	int32 Score = 0;
};

/**
 * 录制时推断「这一步的目的」（1.6.0，设计稿第 5 节）：点 UI 前后读一遍所在界面的状态，比出点完变了什么，
 * 把变化写成跳过条件的候选（`hint.state`），把握高的直接写成 `skipIf`（`by: "auto"`）。
 *
 * **为什么录的时候做**：点完界面变成什么样，只有录制那一刻看得到；回放时再猜「这一步是为了什么」已经晚了。
 * 人只管照常点，不停下来问（停下来问会打断录制节奏，录完在步骤详情里改）。
 *
 * - 读的范围：点的控件所在的顶层 WBP（嵌套的 WBP 一起展开，最多 MaxNodes 个控件）和点的控件所在 WBP 的字段（`@self` / `@lua`）。
 * - 什么时候读：按下时一次（之前），松开后 0.15、0.5、1 倍的 `stateWindowMs`（默认 2 秒）各一次（之后）。
 *   最后一次和之前比，中途来回变的、只变了数字的（倒计时、数量）不算。
 * - 点完打开了新界面（hint.opened）用来算连带跳过（`also`），录完停止时一起算。
 *
 * 只在游戏线程上用。
 */
class GAMETEST_API FGameTestRecordState
{
public:
	static constexpr int32 AutoScore = 7;
	static constexpr int32 MinScore = 4;
	static constexpr int32 MaxCandidates = 6;
	static constexpr int32 MaxNodes = 800;
	static constexpr int32 DefaultWindowMs = 2000;

	void Begin(EGameTestInferSkip InMode, int32 InWindowMs);
	void End();

	bool IsEnabled() const
	{
		return bActive && Mode != EGameTestInferSkip::Off;
	}

	/** 按在 UI 控件上：读一遍「之前」。上一下还没看完的先按现在的样子收尾（这一下还没松开，界面还没被它改）。 */
	void OnUiPress(const TSharedPtr<SWidget>& InSlate);

	/** 松开成了点击：这一步就是要推断的那一步。 */
	void OnClick(const TSharedRef<FJsonObject>& InStep);

	/** 松开成了拖动：不推断。 */
	void OnCancel();

	/**
	 * 每帧调：到点读「之后」，最后一次读完写进步骤。自动加了 skipIf 时返回给人看的一句（工具栏提示用），否则返回空。
	 * `InStepIndex` 把步骤换成序号（提示里写第几步）。
	 */
	FString Tick(double InNow, TFunctionRef<int32(const TSharedPtr<FJsonObject>&)> InStepIndex);

	/** 停止录制：没看完的按现在收尾，再给自动加了 skipIf 的步骤算连带跳过（`also`）。 */
	void Finish(TArray<TSharedRef<FJsonObject>>& InOutSteps);

	/** 字段名、控件名里带这些词的变化更像「这一步的目的」（领取、勾选、完成……）。纯逻辑，单测用。 */
	static bool IsPurposeName(const FString& InName);

	/** 文字的变化算不算数：只变了数字和标点（倒计时、数量）、变成空、太长的不算。纯逻辑，单测用。 */
	static bool IsMeaningfulTextChange(const FString& InBefore, const FString& InAfter);

	/**
	 * 连带跳过几步（纯逻辑，单测用）：`InIndex` 这一步点完打开了 `InOpened` 这些界面，后面紧挨着的、目标在这些界面上的步骤
	 * （中间的 wait 一起算）都是为了处理这些界面——这一步跳过了，它们也不会出现。阻挡界面规则管的界面不算（回放时自动关）。
	 */
	static int32 ComputeAlso(const TArray<TSharedRef<FJsonObject>>& InSteps, int32 InIndex, const TArray<FString>& InOpened,
		const TArray<FString>& InBlockerScreens);

	/** 候选排序并挑出自动写的那个（分最高且 ≥ AutoScore；没有返回 INDEX_NONE）。纯逻辑，单测用。 */
	static int32 SortAndPick(TArray<FGameTestStateCandidate>& InOutCandidates);

private:
	/** 一个控件某一刻的样子。 */
	struct FWidgetState
	{
		TWeakObjectPtr<UWidget> Widget;
		FString Name;
		bool bVisible = true;
		bool bEnabled = true;
		/** -1 不是能勾选的控件。 */
		int8 Checked = -1;
		FString Text;
		FString Image;
	};

	struct FShot
	{
		TMap<TObjectKey<UWidget>, FWidgetState> Widgets;
		/** `@self.路径` / `@lua.路径` → 值（只要标量）。 */
		TMap<FString, TSharedPtr<FJsonValue>> Fields;
		bool bValid = false;
	};

	struct FPending
	{
		TWeakObjectPtr<UWidget> Pressed;
		TWeakObjectPtr<UUserWidget> Root;
		TWeakObjectPtr<UUserWidget> Owner;
		FShot Before;
		TArray<FShot> After;
		TArray<FString> ScreensBefore;
		TSharedPtr<FJsonObject> Step;
		double ReleasedAt = 0.0;
	};

	FShot Capture(const FPending& InPending) const;
	void Conclude(FPending& InPending, bool bInTakeNow, FString* OutNotice, TFunctionRef<int32(const TSharedPtr<FJsonObject>&)> InStepIndex);
	void Diff(const FPending& InPending, const FShot& InFinal, TArray<FGameTestStateCandidate>& OutCandidates) const;
	bool VerifyAuto(const TSharedPtr<FJsonObject>& InStep, const FGameTestStateCandidate& InCandidate) const;

	bool bActive = false;
	EGameTestInferSkip Mode = EGameTestInferSkip::Suggest;
	double WindowSeconds = 2.0;
	TOptional<FPending> Pressing;
	TOptional<FPending> Watching;
	/** 有候选的步骤，停止时算 also（写了 skipIf 的写进去，没写的记成 hint.state.also 建议值）。 */
	TArray<TWeakPtr<FJsonObject>> AutoSteps;
};
