// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/GameTestSkipIf.h"

#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestJson.h"
#include "GameTestCompat.h"
#include "Observe/GameTestCondition.h"
#include "Observe/GameTestHintPick.h"
#include "Observe/GameTestWidgetNode.h"
#include "Operate/GameTestAutoScroll.h"

#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Widgets/SWidget.h"

namespace GameTestSkipIfDetail
{
	/** skipIf 里条件之外的字段。 */
	const TArray<FString>& ConfigKeys()
	{
		static const TArray<FString> Keys = { TEXT("target"), TEXT("also"), TEXT("settleMs"), TEXT("verify"), TEXT("verifyTimeoutMs"), TEXT("by") };
		return Keys;
	}

	/** 两次判断之间至少隔多久：每次判断要解析选择器（采控件树），不必每帧做。 */
	constexpr double EvalIntervalSeconds = 0.1;

	/** 真在屏上：有 Slate 控件、整条父链可见、有尺寸。对象池里藏着的实例往往也解析得到。 */
	bool IsOnScreen(const FGameTestWidgetNode& InNode)
	{
		TSharedPtr<SWidget> Slate = InNode.Slate.Pin();
		if (!Slate.IsValid() && InNode.Widget.IsValid())
		{
			Slate = InNode.Widget->GetCachedWidget();
		}
		if (!Slate.IsValid())
		{
			return false;
		}
		for (TSharedPtr<SWidget> Current = Slate; Current.IsValid(); Current = Current->GetParentWidget())
		{
			if (!Current->GetVisibility().IsVisible())
			{
				return false;
			}
		}
		const FVector2D Size = FVector2D(Slate->GetCachedGeometry().GetLocalSize());
		return Size.X >= 1.0 && Size.Y >= 1.0;
	}

	enum class EEval : uint8
	{
		Holds,
		NotHolds,
		/** 判断不了：反面条件所在的屏还没在屏上（找不到可能只是界面没打开）。 */
		Unknown,
	};

	struct FEval
	{
		EEval Result = EEval::Unknown;
		/** 不成立是确定的：条件看的控件（反面条件是它所在的屏）就在屏上。不确定的不成立不拿来放行。 */
		bool bDefinitive = false;
		FString Actual;
	};

	FString TargetOf(const FJsonObject& InSkipIf, const FGameTestStep& InStep)
	{
		FString Target;
		if (!GameTestJson::TryGetString(InSkipIf, TEXT("target"), Target) || Target.IsEmpty())
		{
			Target = InStep.Target;
		}
		return Target;
	}

	/**
	 * `InScroll` 给了（回放的跳过判断）时，看的控件找不到就替用户滚列表找（GameTestAutoScroll，1.7.1）：
	 * 认得出是第几个条目的直接滚过去，认不出的把界面上的列表从头翻到底，翻完还没有就原地等。
	 * 反面条件（不存在 / 不可见）找不到正是要的结果，不翻。
	 */
	FEval Evaluate(const FJsonObject& InSkipIf, const FGameTestStep& InStep, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles, GameTestAutoScroll::FState* InScroll = nullptr)
	{
		FEval Out;
		const FString Target = TargetOf(InSkipIf, InStep);
		// 看的就是这一步的目标时，带上录制线索一起认（同一个 WBP 的几个实例、顺序变了的列表），和点击认的是同一个
		const FJsonObject* Hint = Target == InStep.Target ? GameTestHintPick::GetHint(InStep.Params) : nullptr;
		TSharedPtr<FGameTestWidgetNode> Node;
		FString ResolveError;
		const bool bResolved = GameTestHandlers::TryResolveTarget(Target, InHandles, Node, ResolveError, Hint);
		const bool bOnScreen = bResolved && Node.IsValid() && IsOnScreen(*Node);
		if (InScroll != nullptr)
		{
			if (bResolved)
			{
				GameTestAutoScroll::MarkFound(*InScroll);
			}
			else if (!GameTestCondition::HasNegative(InSkipIf))
			{
				// 紧接着解析失败就取：后面再解析别的（屏）会把「哪个条目没生成」清掉
				if (GameTestAutoScroll::ScrollToUngeneratedEntry(*InScroll))
				{
					ResolveError += TEXT("（已自动把列表滚到这个条目，等它生成）");
				}
				else
				{
					GameTestAutoScroll::ScanForMissing(Target, InHandles, *InScroll, &ResolveError);
				}
			}
		}

		if (GameTestCondition::HasNegative(InSkipIf))
		{
			const FString Screen = GameTestAutoScroll::ScreenOf(Target);
			TSharedPtr<FGameTestWidgetNode> ScreenNode;
			FString ScreenError;
			if (Screen.IsEmpty() || !GameTestHandlers::TryResolveTarget(Screen + TEXT(":."), InHandles, ScreenNode, ScreenError)
				|| !ScreenNode.IsValid() || !IsOnScreen(*ScreenNode))
			{
				Out.Result = EEval::Unknown;
				Out.Actual = Screen.IsEmpty()
					? FString(TEXT("条件里有「不存在 / 不可见」，但 target 没写屏名，判断不了界面有没有打开"))
					: FString::Printf(TEXT("%s 还没在屏上"), *Screen);
				return Out;
			}
			Out.bDefinitive = true;
		}
		else if (!bOnScreen)
		{
			Out.Result = EEval::NotHolds;
			Out.Actual = bResolved ? FString::Printf(TEXT("%s 还不在屏上"), *Target) : FString::Printf(TEXT("没找到 %s：%s"), *Target, *ResolveError);
			return Out;
		}
		else
		{
			Out.bDefinitive = true;
		}

		FGameTestConditionCheck Check;
		GameTestCondition::Check(InSkipIf, bResolved ? Node.Get() : nullptr, ResolveError, Check);
		Out.Result = Check.Holds() ? EEval::Holds : EEval::NotHolds;
		Out.Actual = Check.Actuals.Num() > 0 ? FString::Join(Check.Actuals, TEXT("、")) : FString::Join(Check.Mismatches, TEXT("；"));
		return Out;
	}

	/** 这一步自己的目标出来了没有（没有目标的步骤算出来了）：没出来时不成立的结论不确定。 */
	bool StepTargetReady(const FGameTestStep& InStep, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles)
	{
		if (InStep.Target.IsEmpty())
		{
			return true;
		}
		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		return GameTestHandlers::TryResolveTarget(InStep.Target, InHandles, Node, Error, GameTestHintPick::GetHint(InStep.Params))
			&& Node.IsValid() && IsOnScreen(*Node);
	}

	bool ReadMs(const FJsonObject& InObject, const TCHAR* InField, int32 InMax, int32& OutValue, FString& OutError)
	{
		if (!InObject.HasField(InField))
		{
			return true;
		}
		int32 Value = 0;
		if (!GameTestJson::TryGetInt(InObject, InField, Value) || Value < 0 || Value > InMax)
		{
			OutError = FString::Printf(TEXT("%s 要写 0–%d 的整数（毫秒）"), InField, InMax);
			return false;
		}
		OutValue = Value;
		return true;
	}
}

TSharedPtr<FJsonObject> GameTestSkipIf::Get(const TSharedPtr<FJsonObject>& InStepParams)
{
	return InStepParams.IsValid() ? GameTestJson::TryGetObject(*InStepParams, TEXT("skipIf")) : nullptr;
}

bool GameTestSkipIf::ValidateStep(const FJsonObject& InStepJson, FString& OutError)
{
	const TSharedPtr<FJsonValue> Value = InStepJson.TryGetField(TEXT("skipIf"));
	if (!Value.IsValid() || Value->Type == EJson::Null)
	{
		return true;
	}
	const TSharedPtr<FJsonObject> SkipIf = GameTestJson::TryGetObject(InStepJson, TEXT("skipIf"));
	if (!SkipIf.IsValid())
	{
		OutError = TEXT("要写成对象，例如 {\"checked\": true}、{\"textLike\": \"已领取\"}");
		return false;
	}
	if (!GameTestCondition::Validate(*SkipIf, GameTestSkipIfDetail::ConfigKeys(), OutError))
	{
		return false;
	}
	FString StepTarget;
	GameTestJson::TryGetString(InStepJson, TEXT("target"), StepTarget);
	FString CondTarget;
	if (SkipIf->HasField(TEXT("target")) && (!GameTestJson::TryGetString(*SkipIf, TEXT("target"), CondTarget) || CondTarget.IsEmpty()))
	{
		OutError = TEXT("target 要写选择器（省略 = 这一步自己的 target）");
		return false;
	}
	if (CondTarget.IsEmpty() && StepTarget.IsEmpty())
	{
		OutError = TEXT("这一步没有 target，跳过条件要写 target 说明看哪个控件");
		return false;
	}
	if (SkipIf->HasField(TEXT("also")))
	{
		int32 Also = 0;
		if (!GameTestJson::TryGetInt(*SkipIf, TEXT("also"), Also) || Also < 0 || Also > MaxAlso)
		{
			OutError = FString::Printf(TEXT("also 要写 0–%d 的整数（连带跳过后面几步）"), MaxAlso);
			return false;
		}
	}
	bool bVerify = false;
	if (SkipIf->HasField(TEXT("verify")) && !GameTestJson::TryGetBool(*SkipIf, TEXT("verify"), bVerify))
	{
		OutError = TEXT("verify 要写 true 或 false");
		return false;
	}
	int32 Ignored = 0;
	return GameTestSkipIfDetail::ReadMs(*SkipIf, TEXT("settleMs"), MaxSettleMs, Ignored, OutError)
		&& GameTestSkipIfDetail::ReadMs(*SkipIf, TEXT("verifyTimeoutMs"), MaxVerifyTimeoutMs, Ignored, OutError);
}

bool GameTestSkipIf::ApplyOptions(const FJsonValue& InValue, FGameTestSkipOptions& InOutOptions, FString& OutError)
{
	bool bFlag = true;
	if (InValue.TryGetBool(bFlag))
	{
		InOutOptions.bEnabled = bFlag;
		return true;
	}
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!InValue.TryGetObject(Object) || Object == nullptr || !Object->IsValid())
	{
		OutError = TEXT("skipIf 要写 true / false，或 {\"enabled\", \"settleMs\", \"verify\", \"verifyTimeoutMs\"}");
		return false;
	}
	const FJsonObject& Json = **Object;
	for (const auto& Pair : Json.Values)
	{
		const FString Key = GAMETEST_JSON_KEY(Pair.Key);
		if (Key != TEXT("enabled") && Key != TEXT("settleMs") && Key != TEXT("verify") && Key != TEXT("verifyTimeoutMs"))
		{
			OutError = FString::Printf(TEXT("skipIf 不认识字段 '%s'（可用：enabled, settleMs, verify, verifyTimeoutMs）"), *Key);
			return false;
		}
	}
	if (Json.HasField(TEXT("enabled")) && !GameTestJson::TryGetBool(Json, TEXT("enabled"), InOutOptions.bEnabled))
	{
		OutError = TEXT("skipIf.enabled 要写 true 或 false");
		return false;
	}
	if (Json.HasField(TEXT("verify")) && !GameTestJson::TryGetBool(Json, TEXT("verify"), InOutOptions.bVerify))
	{
		OutError = TEXT("skipIf.verify 要写 true 或 false");
		return false;
	}
	FString Error;
	if (!GameTestSkipIfDetail::ReadMs(Json, TEXT("settleMs"), MaxSettleMs, InOutOptions.SettleMs, Error)
		|| !GameTestSkipIfDetail::ReadMs(Json, TEXT("verifyTimeoutMs"), MaxVerifyTimeoutMs, InOutOptions.VerifyTimeoutMs, Error))
	{
		OutError = FString::Printf(TEXT("skipIf.%s"), *Error);
		return false;
	}
	return true;
}

bool GameTestSkipIf::ShouldVerify(const FJsonObject& InSkipIf, const FGameTestSkipOptions& InOptions)
{
	bool bVerify = InOptions.bVerify;
	GameTestJson::TryGetBool(InSkipIf, TEXT("verify"), bVerify);
	return bVerify;
}

int32 GameTestSkipIf::VerifyTimeoutMs(const FJsonObject& InSkipIf, const FGameTestSkipOptions& InOptions)
{
	int32 Value = InOptions.VerifyTimeoutMs;
	GameTestJson::TryGetInt(InSkipIf, TEXT("verifyTimeoutMs"), Value);
	return FMath::Clamp(Value, 0, MaxVerifyTimeoutMs);
}

int32 GameTestSkipIf::Also(const FJsonObject& InSkipIf)
{
	int32 Value = 0;
	GameTestJson::TryGetInt(InSkipIf, TEXT("also"), Value);
	return FMath::Clamp(Value, 0, MaxAlso);
}

bool GameTestSkipIf::Holds(const FJsonObject& InSkipIf, const FGameTestStep& InStep, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles, FString& OutActual)
{
	const GameTestSkipIfDetail::FEval Eval = GameTestSkipIfDetail::Evaluate(InSkipIf, InStep, InHandles);
	OutActual = Eval.Actual;
	return Eval.Result == GameTestSkipIfDetail::EEval::Holds;
}

FGameTestSkipGate::FGameTestSkipGate(const FGameTestStep& InStep, const TSharedPtr<FJsonObject>& InSkipIf, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles, const FGameTestSkipOptions& InOptions, double InNow)
	: Step(InStep)
	, SkipIf(InSkipIf)
	, Handles(InHandles)
	, StartedAt(InNow)
{
	int32 SettleMs = InOptions.SettleMs;
	if (SkipIf.IsValid())
	{
		GameTestJson::TryGetInt(*SkipIf, TEXT("settleMs"), SettleMs);
		CondTarget = GameTestSkipIfDetail::TargetOf(*SkipIf, Step);
	}
	SettleSeconds = FMath::Clamp(SettleMs, 0, GameTestSkipIf::MaxSettleMs) / 1000.0;
	// 反面条件（不存在 / 不可见）多等一倍：界面打开的那一下按钮可能还没建出来
	if (SkipIf.IsValid() && GameTestCondition::HasNegative(*SkipIf))
	{
		SettleSeconds *= 2.0;
	}
	Message = FString::Printf(TEXT("判断 %s 是否已达成"), *CondTarget);
}

EGameTestSkipGate FGameTestSkipGate::Tick(double InNow)
{
	if (!SkipIf.IsValid())
	{
		return EGameTestSkipGate::Proceed;
	}
	if (LastEvalAt >= 0.0 && InNow - LastEvalAt < GameTestSkipIfDetail::EvalIntervalSeconds)
	{
		return EGameTestSkipGate::Wait;
	}
	LastEvalAt = InNow;

	const GameTestSkipIfDetail::FEval Eval = GameTestSkipIfDetail::Evaluate(*SkipIf, Step, Handles, &Scroll);
	Actual = Eval.Actual;

	if (Eval.Result == GameTestSkipIfDetail::EEval::Holds)
	{
		NotSince = -1.0;
		if (HoldSince < 0.0)
		{
			HoldSince = InNow;
		}
		if (InNow - HoldSince >= SettleSeconds)
		{
			Message = FString::Printf(TEXT("已达成，跳过：%s（此刻 %s）"), *GameTestCondition::Describe(*SkipIf), *Actual);
			return EGameTestSkipGate::Skip;
		}
		Message = FString::Printf(TEXT("条件刚成立，再看一会儿：%s"), *Actual);
		return EGameTestSkipGate::Wait;
	}

	HoldSince = -1.0;
	// 不成立要「确定」才放行：条件看的控件在屏上，或者这一步自己的目标已经出来了（界面开了，只是条件没达成）
	const bool bSure = Eval.bDefinitive || GameTestSkipIfDetail::StepTargetReady(Step, Handles);
	if (!bSure)
	{
		NotSince = -1.0;
		Message = FString::Printf(TEXT("等界面出来再判断是否已达成：%s"), *Actual);
		return EGameTestSkipGate::Wait;
	}
	if (NotSince < 0.0)
	{
		NotSince = InNow;
	}
	if (InNow - NotSince >= SettleSeconds)
	{
		Message = FString::Printf(TEXT("没达成（%s），照常做"), *Actual);
		return EGameTestSkipGate::Proceed;
	}
	Message = FString::Printf(TEXT("条件不成立，再看一会儿：%s"), *Actual);
	return EGameTestSkipGate::Wait;
}

TSharedPtr<FJsonObject> FGameTestSkipGate::MakeSkipPayload(double InNow) const
{
	const TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	if (SkipIf.IsValid())
	{
		const TSharedPtr<FJsonObject> Cond = MakeShared<FJsonObject>();
		for (const auto& Pair : SkipIf->Values)
		{
			const FString Key = GAMETEST_JSON_KEY(Pair.Key);
			if (GameTestCondition::IsKey(Key))
			{
				Cond->SetField(Key, Pair.Value);
			}
		}
		Out->SetObjectField(TEXT("cond"), Cond);
		Out->SetStringField(TEXT("say"), GameTestCondition::Describe(*SkipIf));
		const int32 Also = GameTestSkipIf::Also(*SkipIf);
		if (Also > 0)
		{
			Out->SetNumberField(TEXT("also"), Also);
		}
	}
	Out->SetStringField(TEXT("target"), CondTarget);
	Out->SetStringField(TEXT("actual"), Actual);
	if (!Scroll.Note.IsEmpty())
	{
		Out->SetStringField(TEXT("scrolled"), Scroll.Note);
	}
	Out->SetNumberField(TEXT("waitedMs"), FMath::RoundToInt((InNow - StartedAt) * 1000.0));
	return Out;
}
