// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/GameTestSkipIf.h"

#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestJson.h"
#include "GameTestCompat.h"
#include "Core/Expr/GameTestExpr.h"
#include "Core/GameTestWorld.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Checks/GameTestCond.h"
#include "Observe/GameTestWidgetNode.h"
#include "Observe/Ref/GameTestRefAmbient.h"

#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Widgets/SWidget.h"

namespace GameTestSkipIfDetail
{
	/** skipIf 里 cond 之外的字段（v4：条件只写在 cond 里）。 */
	const TArray<FString>& ConfigKeys()
	{
		static const TArray<FString> Keys = { TEXT("cond"), TEXT("also"), TEXT("settleMs"), TEXT("verify"), TEXT("verifyTimeoutMs") };
		return Keys;
	}

	/** 两次判断之间至少隔多久：每次判断要解析引用（采界面树），不必每帧做。 */
	constexpr double EvalIntervalSeconds = 0.1;

	enum class EEval : uint8
	{
		Holds,
		NotHolds,
		/** 判断不了：反面判断的对象本身还没出来（找不到可能只是界面没打开，game-test-script 4.2）。 */
		Unknown,
	};

	struct FEval
	{
		EEval Result = EEval::Unknown;
		/** 不成立是确定的：每条判断都读到了值（对象都在）。不确定的不成立不拿来放行。 */
		bool bDefinitive = false;
		FString Actual;
	};

	/** 此刻的脚本上下文（执行器 Tick 里设的）；没有时用临时的（gt.resolve checks、录制核对）。 */
	FEval Evaluate(const FJsonObject& InSkipIf, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles)
	{
		FEval Out;
		const TSharedPtr<FJsonObject> Cond = GameTestCond::AsObject(InSkipIf.TryGetField(TEXT("cond")));
		if (!Cond.IsValid())
		{
			Out.Actual = TEXT("skipIf 没写 cond");
			return Out;
		}
		FGameTestFlowContext Temp;
		FGameTestFlowContext* Flow = GameTestRefAmbient::Get();
		if (Flow == nullptr)
		{
			Temp.Handles = InHandles;
			Flow = &Temp;
		}
		if (Flow->World == nullptr)
		{
			Flow->World = GameTestWorld::FindGameWorld();
		}
		FGameTestCondResult Result;
		{
			FGameTestFlowContext::FReadOnlyScope ReadOnly(*Flow);
			Result = GameTestCond::Evaluate(*Cond, *Flow);
		}
		Out.Actual = Result.Summary();
		if (Result.State == EGameTestCondState::Unknown)
		{
			Out.Result = EEval::Unknown;
			return Out;
		}
		Out.Result = Result.Holds() ? EEval::Holds : EEval::NotHolds;
		Out.bDefinitive = true;
		for (const FGameTestCondPart& Part : Result.Parts)
		{
			if (!Part.Unresolved.IsEmpty())
			{
				Out.bDefinitive = false;
			}
		}
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
		return GameTestHandlers::TryResolveTarget(InStep.Target, InHandles, Node, Error) && Node.IsValid();
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
		OutError = TEXT("要写成对象 {\"cond\": 条件, \"also\"?, \"verify\"?}");
		return false;
	}
	if (!SkipIf->HasField(TEXT("cond")))
	{
		OutError = TEXT("cond_removed：v4 的 skipIf 写 {\"cond\": 条件, \"also\"?, \"verify\"?}，条件写进 cond（对象判断 {obj, prop, op, value}）；按控件写的 checked / textLike 这类字段删掉了");
		return false;
	}
	for (const auto& Pair : SkipIf->Values)
	{
		const FString Key = GAMETEST_JSON_KEY(Pair.Key);
		if (!GameTestSkipIfDetail::ConfigKeys().Contains(Key))
		{
			OutError = FString::Printf(TEXT("skipIf 不认识字段 %s（可用：%s）"), *Key, *FString::Join(GameTestSkipIfDetail::ConfigKeys(), TEXT(", ")));
			return false;
		}
	}
	{
		FGameTestDiagSink Sink;
		FGameTestExprScope Scope;
		GameTestCond::Validate(SkipIf->TryGetField(TEXT("cond")), Scope, Sink);
		if (Sink.HasErrors())
		{
			TArray<FString> Lines;
			Sink.AppendErrorLines(Lines);
			OutError = FString::Printf(TEXT("cond：%s"), *FString::Join(Lines, TEXT("；")));
			return false;
		}
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
	(void)InStep;
	const GameTestSkipIfDetail::FEval Eval = GameTestSkipIfDetail::Evaluate(InSkipIf, InHandles);
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
		if (const TSharedPtr<FJsonObject> Cond = GameTestCond::AsObject(SkipIf->TryGetField(TEXT("cond"))))
		{
			CondTarget = GameTestCond::Describe(*Cond);
		}
	}
	SettleSeconds = FMath::Clamp(SettleMs, 0, GameTestSkipIf::MaxSettleMs) / 1000.0;
	Message = FString::Printf(TEXT("判断「%s」是否已达成"), *CondTarget);
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

	const GameTestSkipIfDetail::FEval Eval = GameTestSkipIfDetail::Evaluate(*SkipIf, Handles);
	Actual = Eval.Actual;

	if (Eval.Result == GameTestSkipIfDetail::EEval::Unknown)
	{
		HoldSince = -1.0;
		NotSince = -1.0;
		Message = FString::Printf(TEXT("等界面出来再判断是否已达成：%s"), *Actual);
		return EGameTestSkipGate::Wait;
	}
	if (Eval.Result == GameTestSkipIfDetail::EEval::Holds)
	{
		NotSince = -1.0;
		if (HoldSince < 0.0)
		{
			HoldSince = InNow;
		}
		if (InNow - HoldSince >= SettleSeconds)
		{
			Message = FString::Printf(TEXT("已达成，跳过：%s（此刻 %s）"), *CondTarget, *Actual);
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
		if (const TSharedPtr<FJsonObject> Cond = GameTestCond::AsObject(SkipIf->TryGetField(TEXT("cond"))))
		{
			Out->SetObjectField(TEXT("cond"), Cond);
		}
		Out->SetStringField(TEXT("say"), CondTarget);
		const int32 Also = GameTestSkipIf::Also(*SkipIf);
		if (Also > 0)
		{
			Out->SetNumberField(TEXT("also"), Also);
		}
	}
	Out->SetStringField(TEXT("actual"), Actual);
	Out->SetNumberField(TEXT("waitedMs"), FMath::RoundToInt((InNow - StartedAt) * 1000.0));
	return Out;
}

FString GameTestSkipIf::Describe(const FJsonObject& InSkipIf)
{
	const TSharedPtr<FJsonObject> Cond = GameTestCond::AsObject(InSkipIf.TryGetField(TEXT("cond")));
	return Cond.IsValid() ? GameTestCond::Describe(*Cond) : FString(TEXT("（skipIf 没写 cond）"));
}
