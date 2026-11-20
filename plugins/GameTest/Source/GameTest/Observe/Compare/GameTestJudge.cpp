// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Compare/GameTestJudge.h"

#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Core/Script/GameTestScriptOptions.h"
#include "Observe/Compare/GameTestCompareRegistry.h"
#include "Observe/Ref/GameTestRef.h"

double GameTestJudge::DefaultTol(EGameTestKind InKind, const FGameTestScriptOptions& InOptions)
{
	switch (InKind)
	{
	case EGameTestKind::Vector:
	case EGameTestKind::Vector2:
		// vector2 是视口归一化坐标（:pos），厘米的容差太大：用 color 那一档（0.02 ≈ 屏幕的 2%）
		return InKind == EGameTestKind::Vector ? InOptions.TolVector : InOptions.TolColor;
	case EGameTestKind::Rotator:
		return InOptions.TolRotator;
	case EGameTestKind::Color:
		return InOptions.TolColor;
	case EGameTestKind::Int:
		return 0.0001;
	default:
		return InOptions.TolFloat;
	}
}

FString GameTestJudge::TypeMismatch(const FString& InOp, EGameTestKind InKind)
{
	const IGameTestCompare* Compare = FGameTestCompareRegistry::Find(InOp);
	if (Compare == nullptr || InKind == EGameTestKind::None || Compare->Applies(InKind))
	{
		return FString();
	}
	if (!FGameTestPropKind::IsComparable(InKind))
	{
		return FString::Printf(TEXT("这个属性是 %s，不能直接比较（展开到它的成员再选）"), FGameTestPropKind::Name(InKind));
	}
	return FString::Printf(TEXT("%s 类型不能用 %s（能用：%s）"), FGameTestPropKind::Name(InKind), *InOp, *FString::Join(FGameTestCompareRegistry::OpsFor(InKind), TEXT("、")));
}

FGameTestJudgeResult GameTestJudge::TestRead(const FGameTestPropRead& InRead, const FGameTestIdentItem& InItem, FGameTestFlowContext& InContext)
{
	FGameTestJudgeResult Result;
	Result.Read = InRead;
	if (!InRead.IsResolved())
	{
		Result.bUnresolved = true;
		Result.Why = InRead.Value.GetString();
		Result.Code = InRead.Value.GetCode();
		return Result;
	}
	const EGameTestKind Kind = InRead.Kind == EGameTestKind::None ? FGameTestPropKind::OfValue(InRead.Value) : InRead.Kind;
	Result.Op = InItem.Op.IsEmpty() ? FGameTestCompareRegistry::DefaultOp(Kind) : InItem.Op;
	const IGameTestCompare* Compare = FGameTestCompareRegistry::Find(Result.Op);
	if (Compare == nullptr)
	{
		Result.bUnresolved = true;
		Result.Code = TEXT("op_unknown");
		Result.Why = FString::Printf(TEXT("没有比较 %s"), *Result.Op);
		return Result;
	}
	const FString Mismatch = TypeMismatch(Result.Op, Kind);
	if (!Mismatch.IsEmpty())
	{
		Result.bUnresolved = true;
		Result.Code = TEXT("op_type");
		Result.Why = FString::Printf(TEXT("%s：%s"), *InItem.Prop, *Mismatch);
		return Result;
	}
	FGameTestCompareInput Input(InRead, InItem);
	Input.Expected = InItem.Value.IsValid() ? GameTestRefs::EvalJson(InItem.Value, InContext) : FGameTestValue::Null();
	if (Input.Expected.IsUnresolved())
	{
		Result.bUnresolved = true;
		Result.Code = Input.Expected.GetCode();
		Result.Why = FString::Printf(TEXT("期望值算不出：%s"), *Input.Expected.GetString());
		return Result;
	}
	Input.bIgnoreCase = InItem.IgnoreCase.IsSet() ? InItem.IgnoreCase.GetValue() : InContext.Options.bRefIgnoreCase;
	Input.Tol = InItem.Tol.IsSet() ? InItem.Tol.GetValue() : DefaultTol(Kind, InContext.Options);
	// 比较按 kind 走：Lua 值、状态的 kind 由读的那边给；省略时补上推出来的
	FGameTestPropRead Typed = InRead;
	Typed.Kind = Kind;
	FGameTestCompareInput TypedInput(Typed, InItem);
	TypedInput.Expected = Input.Expected;
	TypedInput.bIgnoreCase = Input.bIgnoreCase;
	TypedInput.Tol = Input.Tol;
	const FGameTestCompareResult Compared = Compare->Test(TypedInput);
	Result.bHolds = Compared.bHolds;
	Result.Why = Compared.bHolds ? FString() : FString::Printf(TEXT("%s：%s"), InItem.Prop.IsEmpty() ? TEXT(":exists") : *InItem.Prop, *Compared.Why);
	return Result;
}

FGameTestJudgeResult GameTestJudge::Test(UObject* InObject, const FGameTestIdentItem& InItem, FGameTestFlowContext& InContext, UObject* InListItem)
{
	const FString Prop = InItem.Prop.IsEmpty() ? FString(TEXT(":exists")) : InItem.Prop;
	const FGameTestPropRead Read = GameTestPropRead::ReadText(InObject, Prop, InContext, InListItem);
	return TestRead(Read, InItem, InContext);
}
