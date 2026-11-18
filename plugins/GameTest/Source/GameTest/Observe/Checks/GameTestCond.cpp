// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Checks/GameTestCond.h"

#include "Core/Expr/GameTestExpr.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"
#include "Observe/Checks/GameTestCheck.h"

namespace
{
	const TCHAR* StateText(EGameTestCondState InState)
	{
		return InState == EGameTestCondState::Holds ? TEXT("成立") : InState == EGameTestCondState::Unknown ? TEXT("还不能判断") : TEXT("不成立");
	}

	bool IsTreeNode(const FJsonObject& InCond)
	{
		return InCond.HasField(TEXT("all")) || InCond.HasField(TEXT("any")) || InCond.HasField(TEXT("not"));
	}

	/**
	 * 1.6 的旧条件对象里 `any: [条件对象…]` 是「控件条件的任一成立」，和新的 `any` 同义，
	 * 但旧对象可能同时写了 `target` + `any`（any 里的项共用外层的 target）。这种按一条控件判断交给 widget 处理。
	 */
	bool IsLegacyWidgetAny(const FJsonObject& InCond)
	{
		return InCond.HasField(TEXT("any")) && InCond.HasField(TEXT("target"));
	}

	EGameTestCondState Combine(const TArray<EGameTestCondState>& InStates, bool bAll)
	{
		bool bUnknown = false;
		for (const EGameTestCondState State : InStates)
		{
			if (State == (bAll ? EGameTestCondState::NotHolds : EGameTestCondState::Holds))
			{
				return State;
			}
			bUnknown = bUnknown || State == EGameTestCondState::Unknown;
		}
		if (bUnknown)
		{
			return EGameTestCondState::Unknown;
		}
		return bAll ? EGameTestCondState::Holds : EGameTestCondState::NotHolds;
	}

	EGameTestCondState EvalNode(const FJsonObject& InCond, FGameTestFlowContext& InContext, const FString& InPath, FGameTestCondResult& OutResult);

	EGameTestCondState EvalList(const FJsonObject& InCond, const TCHAR* InKey, FGameTestFlowContext& InContext, const FString& InPath, FGameTestCondResult& OutResult)
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		TArray<EGameTestCondState> States;
		if (InCond.TryGetArrayField(InKey, Items) && Items)
		{
			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Child = GameTestCond::AsObject((*Items)[Index]);
				const FString ChildPath = FString::Printf(TEXT("%s%s%s.%d"), *InPath, InPath.IsEmpty() ? TEXT("") : TEXT("."), InKey, Index);
				States.Add(Child.IsValid() ? EvalNode(*Child, InContext, ChildPath, OutResult) : EGameTestCondState::NotHolds);
			}
		}
		return Combine(States, FCString::Strcmp(InKey, TEXT("all")) == 0);
	}

	EGameTestCondState EvalCheck(const FJsonObject& InCond, FGameTestFlowContext& InContext, const FString& InPath, FGameTestCondResult& OutResult)
	{
		FGameTestCondPart& Part = OutResult.Parts.AddDefaulted_GetRef();
		Part.Path = InPath;
		const IGameTestCheck* Check = GameTestChecks::FindFor(InCond);
		if (!Check)
		{
			Part.State = EGameTestCondState::NotHolds;
			Part.Unresolved = TEXT("认不出这条判断（要有 target / value / api / expr / actorNear / alias 之一）");
			return Part.State;
		}
		FGameTestCheckResult Result;
		Check->Evaluate(InCond, InContext, Result);
		Part.Actual = Result.Actual;
		Part.Unresolved = Result.Unresolved;
		Part.Say = Result.Say;
		Part.State = Result.bHolds ? EGameTestCondState::Holds
			: (Result.bScreenAbsent && Check->IsNegative(InCond)) ? EGameTestCondState::Unknown
			: EGameTestCondState::NotHolds;
		return Part.State;
	}

	EGameTestCondState EvalNode(const FJsonObject& InCond, FGameTestFlowContext& InContext, const FString& InPath, FGameTestCondResult& OutResult)
	{
		if (!IsTreeNode(InCond) || IsLegacyWidgetAny(InCond))
		{
			return EvalCheck(InCond, InContext, InPath, OutResult);
		}
		if (InCond.HasField(TEXT("all")))
		{
			return EvalList(InCond, TEXT("all"), InContext, InPath, OutResult);
		}
		if (InCond.HasField(TEXT("any")))
		{
			return EvalList(InCond, TEXT("any"), InContext, InPath, OutResult);
		}
		const TSharedPtr<FJsonObject> Inner = GameTestCond::AsObject(InCond.TryGetField(TEXT("not")));
		if (!Inner.IsValid())
		{
			return EGameTestCondState::NotHolds;
		}
		const EGameTestCondState State = EvalNode(*Inner, InContext, InPath.IsEmpty() ? TEXT("not") : InPath + TEXT(".not"), OutResult);
		return State == EGameTestCondState::Unknown ? State
			: State == EGameTestCondState::Holds ? EGameTestCondState::NotHolds : EGameTestCondState::Holds;
	}

	void ValidateCondNode(const TSharedPtr<FJsonValue>& InCond, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink, const TArray<FString>& InExtraAllowed)
	{
		FString Text;
		if (InCond.IsValid() && InCond->TryGetString(Text))
		{
			FGameTestDiagSink::FScope Scope(InSink, TEXT("expr"));
			GameTestRefs::Validate(MakeShared<FJsonValueString>(Text.StartsWith(TEXT("=")) ? Text : TEXT("=") + Text), InScope, InSink);
			return;
		}
		const TSharedPtr<FJsonObject> Cond = GameTestCond::AsObject(InCond);
		if (!Cond.IsValid())
		{
			InSink.Error(TEXT("bad_type"), TEXT("条件要写成对象（或表达式字符串）"));
			return;
		}
		if (IsTreeNode(*Cond) && !IsLegacyWidgetAny(*Cond))
		{
			for (const auto& Pair : Cond->Values)
			{
				const FString Key = GAMETEST_JSON_KEY(Pair.Key);
				if (!GameTestCond::TreeKeys().Contains(Key) && !InExtraAllowed.Contains(Key))
				{
					InSink.Error(TEXT("unknown_field"), FString::Printf(TEXT("all / any / not 节点上不能写 %s：判断放进数组里"), *Key));
				}
			}
			for (const TCHAR* Key : { TEXT("all"), TEXT("any") })
			{
				const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
				if (!Cond->HasField(Key))
				{
					continue;
				}
				if (!Cond->TryGetArrayField(Key, Items) || !Items || Items->Num() == 0)
				{
					InSink.Error(TEXT("bad_type"), FString::Printf(TEXT("%s 要写成非空数组"), Key));
					continue;
				}
				for (int32 Index = 0; Index < Items->Num(); ++Index)
				{
					FGameTestDiagSink::FScope Scope(InSink, FString::Printf(TEXT("%s.%d"), Key, Index));
					ValidateCondNode((*Items)[Index], InScope, InSink, TArray<FString>());
				}
			}
			if (Cond->HasField(TEXT("not")))
			{
				FGameTestDiagSink::FScope Scope(InSink, TEXT("not"));
				ValidateCondNode(Cond->TryGetField(TEXT("not")), InScope, InSink, TArray<FString>());
			}
			return;
		}
		const IGameTestCheck* Check = GameTestChecks::FindFor(*Cond);
		if (!Check)
		{
			InSink.Error(TEXT("bad_type"), TEXT("认不出这条判断：要有 target / value / api / expr / actorNear / alias 之一"));
			return;
		}
		for (const auto& Pair : Cond->Values)
		{
			const FString Key = GAMETEST_JSON_KEY(Pair.Key);
			if (!Check->GetFields().Contains(Key) && !GameTestCompare::Keys().Contains(Key) && !GameTestCond::TreeKeys().Contains(Key)
				&& !InExtraAllowed.Contains(Key))
			{
				InSink.Error(TEXT("unknown_field"), FString::Printf(TEXT("判断（%s）不认识字段 %s，可用：%s"), *Check->GetKey().ToString(), *Key,
					*FString::Join(Check->GetFields(), TEXT(", "))));
			}
		}
		GameTestCompare::Validate(*Cond, InSink);
		for (const FString& Key : GameTestCompare::Keys())
		{
			if (const TSharedPtr<FJsonValue> Expected = Cond->TryGetField(Key))
			{
				GameTestRefs::Validate(Expected, InScope, InSink);
			}
		}
		Check->Validate(*Cond, InSink);
	}

	FString DescribeNode(const FJsonObject& InCond)
	{
		if (!IsTreeNode(InCond) || IsLegacyWidgetAny(InCond))
		{
			const IGameTestCheck* Check = GameTestChecks::FindFor(InCond);
			return Check ? Check->Describe(InCond) : TEXT("（写错的判断）");
		}
		if (InCond.HasField(TEXT("not")))
		{
			const TSharedPtr<FJsonObject> Inner = GameTestCond::AsObject(InCond.TryGetField(TEXT("not")));
			return Inner.IsValid() ? FString::Printf(TEXT("不是（%s）"), *DescribeNode(*Inner)) : TEXT("不是（?）");
		}
		const bool bAll = InCond.HasField(TEXT("all"));
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		TArray<FString> Parts;
		if (InCond.TryGetArrayField(bAll ? TEXT("all") : TEXT("any"), Items) && Items)
		{
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				const TSharedPtr<FJsonObject> Child = GameTestCond::AsObject(Item);
				if (Child.IsValid())
				{
					const bool bNested = IsTreeNode(*Child) && !IsLegacyWidgetAny(*Child);
					Parts.Add(bNested ? TEXT("（") + DescribeNode(*Child) + TEXT("）") : DescribeNode(*Child));
				}
			}
		}
		return FString::Join(Parts, bAll ? TEXT(" 且 ") : TEXT(" 或 "));
	}
}

TSharedPtr<FJsonObject> FGameTestCondPart::ToJson() const
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("path"), Path);
	Out->SetBoolField(TEXT("holds"), State == EGameTestCondState::Holds);
	if (State == EGameTestCondState::Unknown)
	{
		Out->SetBoolField(TEXT("unknown"), true);
	}
	if (!Actual.IsNull() && !Actual.IsUnresolved())
	{
		Out->SetStringField(TEXT("actual"), Actual.ToDisplay(200));
	}
	if (!Unresolved.IsEmpty())
	{
		Out->SetStringField(TEXT("unresolved"), Unresolved);
	}
	if (!Say.IsEmpty())
	{
		Out->SetStringField(TEXT("say"), Say);
	}
	return Out;
}

FString FGameTestCondResult::Summary() const
{
	TArray<FString> Lines;
	for (const FGameTestCondPart& Part : Parts)
	{
		const bool bInteresting = State == EGameTestCondState::Holds ? true : Part.State != EGameTestCondState::Holds;
		if (!bInteresting)
		{
			continue;
		}
		const FString What = !Part.Unresolved.IsEmpty() ? FString::Printf(TEXT("读不到：%s"), *Part.Unresolved)
			: !Part.Say.IsEmpty() ? Part.Say
			: Part.Actual.ToDisplay(80);
		Lines.Add(Part.Path.IsEmpty() ? What : FString::Printf(TEXT("%s：%s"), *Part.Path, *What));
	}
	return FString::Printf(TEXT("%s（%s）"), StateText(State), *FString::Join(Lines, TEXT("；")));
}

TSharedPtr<FJsonObject> FGameTestCondResult::ToJson() const
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("holds"), Holds());
	if (State == EGameTestCondState::Unknown)
	{
		Out->SetBoolField(TEXT("unknown"), true);
	}
	TArray<TSharedPtr<FJsonValue>> PartsJson;
	for (const FGameTestCondPart& Part : Parts)
	{
		PartsJson.Add(MakeShared<FJsonValueObject>(Part.ToJson()));
	}
	Out->SetArrayField(TEXT("parts"), PartsJson);
	return Out;
}

namespace GameTestCond
{
	const TArray<FString>& TreeKeys()
	{
		static const TArray<FString> Keys = { TEXT("all"), TEXT("any"), TEXT("not"), TEXT("settleMs"), TEXT("elseSettleMs"), TEXT("client"), TEXT("note") };
		return Keys;
	}

	const TCHAR* SubjectVar()
	{
		return TEXT("__subject");
	}

	TSharedPtr<FJsonObject> AsObject(const TSharedPtr<FJsonValue>& InCond)
	{
		if (!InCond.IsValid())
		{
			return nullptr;
		}
		FString Text;
		if (InCond->TryGetString(Text))
		{
			TSharedPtr<FJsonObject> Wrapped = MakeShared<FJsonObject>();
			Wrapped->SetStringField(TEXT("expr"), Text.StartsWith(TEXT("=")) ? Text.RightChop(1) : Text);
			return Wrapped;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		return InCond->TryGetObject(Object) && Object ? *Object : nullptr;
	}

	void Validate(const TSharedPtr<FJsonValue>& InCond, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink, const TArray<FString>& InExtraAllowed)
	{
		ValidateCondNode(InCond, InScope, InSink, InExtraAllowed);
	}

	FGameTestCondResult Evaluate(const FJsonObject& InCond, FGameTestFlowContext& InContext, const FGameTestValue* InSubject)
	{
		FGameTestFlowContext::FReadOnlyScope ReadOnly(InContext);
		if (InSubject)
		{
			InContext.PushLocals();
			InContext.SetLocal(SubjectVar(), *InSubject);
		}
		FGameTestCondResult Result;
		Result.State = EvalNode(InCond, InContext, FString(), Result);
		if (InSubject)
		{
			InContext.PopLocals();
		}
		return Result;
	}

	int32 SettleMs(const FJsonObject& InCond, const FGameTestFlowContext& InContext)
	{
		int32 Value = InContext.Options.SettleMs;
		InCond.TryGetNumberField(TEXT("settleMs"), Value);
		return FMath::Max(0, Value);
	}

	int32 ElseSettleMs(const FJsonObject& InCond, const FGameTestFlowContext& InContext)
	{
		int32 Value = InContext.Options.ElseSettleMs;
		InCond.TryGetNumberField(TEXT("elseSettleMs"), Value);
		return FMath::Max(0, Value);
	}

	FString Describe(const FJsonObject& InCond)
	{
		return DescribeNode(InCond);
	}

	FGameTestValue ResolveExpected(const TSharedPtr<FJsonValue>& InValue, FGameTestFlowContext& InContext)
	{
		return GameTestRefs::EvalJson(InValue, InContext);
	}
}

void FGameTestCondWatch::Start(const TSharedPtr<FJsonObject>& InCond, FGameTestFlowContext& InContext, bool bNeedNegativeSettle)
{
	Cond = InCond;
	Last = FGameTestCondResult();
	HoldSince = -1.0;
	NotSince = -1.0;
	bNegativeSettle = bNeedNegativeSettle;
	Settle = Cond.IsValid() ? GameTestCond::SettleMs(*Cond, InContext) : 0;
	ElseSettle = Cond.IsValid() ? GameTestCond::ElseSettleMs(*Cond, InContext) : 0;
}

FGameTestCondWatch::EVerdict FGameTestCondWatch::Tick(FGameTestFlowContext& InContext, double InNowSeconds)
{
	if (!Cond.IsValid())
	{
		return EVerdict::Holds;
	}
	Last = GameTestCond::Evaluate(*Cond, InContext);
	switch (Last.State)
	{
	case EGameTestCondState::Holds:
		NotSince = -1.0;
		HoldSince = HoldSince < 0.0 ? InNowSeconds : HoldSince;
		return (InNowSeconds - HoldSince) * 1000.0 >= Settle ? EVerdict::Holds : EVerdict::Pending;
	case EGameTestCondState::NotHolds:
		HoldSince = -1.0;
		if (!bNegativeSettle)
		{
			return EVerdict::NotHolds;
		}
		NotSince = NotSince < 0.0 ? InNowSeconds : NotSince;
		return (InNowSeconds - NotSince) * 1000.0 >= ElseSettle ? EVerdict::NotHolds : EVerdict::Pending;
	default:
		// 还不能判断（反面判断所在的屏没出来）：两边都重新计时。
		HoldSince = -1.0;
		NotSince = -1.0;
		return EVerdict::Pending;
	}
}
