// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Compare/GameTestCompareRegistry.h"

#include "Core/Script/GameTestBuiltins.h"
#include "Core/Script/GameTestDiag.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRef.h"
#include "UObject/Object.h"

//~ FGameTestCompareFn -----------------------------------------------------------

FGameTestCompareFn::FGameTestCompareFn(FName InKey, const TArray<EGameTestKind>& InKinds, const FString& InVerb, const FString& InSummary, bool bInNeedsValue, FTestFn InTest, FValidateFn InValidate)
	: Key(InKey)
	, Kinds(InKinds)
	, VerbText(InVerb)
	, Summary(InSummary)
	, bNeedsValue(bInNeedsValue)
	, TestFn(MoveTemp(InTest))
	, ValidateFn(MoveTemp(InValidate))
{
}

void FGameTestCompareFn::ValidateValue(const TSharedPtr<FJsonValue>& InValue, FGameTestDiagSink& InSink) const
{
	if (ValidateFn)
	{
		ValidateFn(InValue, InSink);
	}
}

FGameTestCompareResult FGameTestCompareFn::Test(const FGameTestCompareInput& InInput) const
{
	return TestFn ? TestFn(InInput) : FGameTestCompareResult();
}

bool IGameTestCompare::Applies(EGameTestKind InKind) const
{
	for (const EGameTestKind Kind : GetKinds())
	{
		if (FGameTestPropKind::Accepts(Kind, InKind))
		{
			return true;
		}
	}
	return false;
}

//~ 注册表 ---------------------------------------------------------------------------

TGameTestRegistry<IGameTestCompare>& FGameTestCompareRegistry::Get()
{
	static TGameTestRegistry<IGameTestCompare> Registry;
	GameTestBuiltins::EnsureRegistered();
	return Registry;
}

const IGameTestCompare* FGameTestCompareRegistry::Find(const FString& InOp)
{
	return InOp.IsEmpty() ? nullptr : Get().Find(FName(*InOp));
}

TArray<FString> FGameTestCompareRegistry::OpsFor(EGameTestKind InKind)
{
	TArray<FString> Ops;
	for (const TSharedRef<IGameTestCompare>& Compare : Get().GetAll())
	{
		if (Compare->Applies(InKind))
		{
			Ops.Add(Compare->GetKey().ToString());
		}
	}
	return Ops;
}

FString FGameTestCompareRegistry::DefaultOp(EGameTestKind InKind)
{
	// 协议 3.3：bool 按真值（is true），其余 eq；eq 不适用的类型取它最自然的那个
	switch (InKind)
	{
	case EGameTestKind::Bool:
	case EGameTestKind::Enum:
	case EGameTestKind::Visibility:
		return TEXT("is");
	case EGameTestKind::Object:
		return TEXT("notNull");
	case EGameTestKind::Array:
		return TEXT("notEmpty");
	case EGameTestKind::Color:
		return TEXT("near");
	default:
		return TEXT("eq");
	}
}

TSharedRef<FJsonObject> FGameTestCompareRegistry::Describe()
{
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const TSharedRef<IGameTestCompare>& Compare : Get().GetAll())
	{
		const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("op"), Compare->GetKey().ToString());
		TArray<TSharedPtr<FJsonValue>> Kinds;
		for (const EGameTestKind Kind : Compare->GetKinds())
		{
			Kinds.Add(MakeShared<FJsonValueString>(FGameTestPropKind::Name(Kind)));
		}
		Item->SetArrayField(TEXT("kinds"), Kinds);
		Item->SetStringField(TEXT("verb"), Compare->Verb());
		Item->SetStringField(TEXT("summary"), Compare->GetSummary());
		if (!Compare->NeedsValue())
		{
			Item->SetBoolField(TEXT("noValue"), true);
		}
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	Out->SetArrayField(TEXT("compares"), Items);
	return Out;
}

//~ 共用小工具 -------------------------------------------------------------------------

namespace GameTestCompareRegistryImpl
{
	/** 枚举名去掉 `EX::` 前缀。 */
	FString ShortEnum(const FString& InName)
	{
		int32 Colons = INDEX_NONE;
		return InName.FindLastChar(TEXT(':'), Colons) ? InName.Mid(Colons + 1) : InName;
	}

	/** 资源路径的几种写法统一成「包路径」和「短名」。 */
	void AssetForms(const FString& InText, FString& OutPath, FString& OutName)
	{
		OutPath = InText.TrimStartAndEnd();
		// `Texture2D'/Game/UI/T_Icon.T_Icon'` → `/Game/UI/T_Icon.T_Icon`
		int32 Quote = INDEX_NONE;
		if (OutPath.FindChar(TEXT('\''), Quote) && OutPath.EndsWith(TEXT("'")))
		{
			OutPath = OutPath.Mid(Quote + 1, OutPath.Len() - Quote - 2);
		}
		int32 Dot = INDEX_NONE;
		if (OutPath.FindLastChar(TEXT('.'), Dot) && OutPath.StartsWith(TEXT("/")))
		{
			OutPath = OutPath.Left(Dot);
		}
		int32 Slash = INDEX_NONE;
		OutName = OutPath.FindLastChar(TEXT('/'), Slash) ? OutPath.Mid(Slash + 1) : OutPath;
	}

	bool NumberOf(const FGameTestValue& InValue, const TCHAR* InField, double& OutNumber)
	{
		FGameTestValue Field;
		return InValue.TryGetField(InField, Field) && Field.TryNumber(OutNumber);
	}
}

FString GameTestCompareUtil::TextOf(const FGameTestValue& InValue)
{
	if (InValue.IsString())
	{
		return InValue.GetString();
	}
	if (const UObject* Object = InValue.GetObject())
	{
		return Object->GetPathName();
	}
	return InValue.ToDisplay(4000);
}

bool GameTestCompareUtil::TextEquals(const FString& InA, const FString& InB, bool bInIgnoreCase)
{
	return InA.Equals(InB, bInIgnoreCase ? ESearchCase::IgnoreCase : ESearchCase::CaseSensitive);
}

bool GameTestCompareUtil::EnumEquals(const FGameTestValue& InActual, const FGameTestValue& InExpected)
{
	using namespace GameTestCompareRegistryImpl;
	double ActualNumber = 0.0;
	double ExpectedNumber = 0.0;
	if (InActual.TryNumber(ActualNumber) && InExpected.TryNumber(ExpectedNumber))
	{
		return FMath::IsNearlyEqual(ActualNumber, ExpectedNumber);
	}
	if (InExpected.Type == EGameTestValueType::Bool)
	{
		// `:checked is true` 这种写法：Checked 当真
		const FString Name = ShortEnum(TextOf(InActual));
		return (Name.Equals(TEXT("Checked"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("true"), ESearchCase::IgnoreCase)) == InExpected.GetBool();
	}
	return ShortEnum(TextOf(InActual)).Equals(ShortEnum(TextOf(InExpected)), ESearchCase::IgnoreCase);
}

bool GameTestCompareUtil::AssetEquals(const FString& InActual, const FString& InExpected)
{
	FString ActualPath;
	FString ActualName;
	FString ExpectedPath;
	FString ExpectedName;
	GameTestCompareRegistryImpl::AssetForms(InActual, ActualPath, ActualName);
	GameTestCompareRegistryImpl::AssetForms(InExpected, ExpectedPath, ExpectedName);
	if (ExpectedPath.StartsWith(TEXT("/")))
	{
		return ActualPath.Equals(ExpectedPath, ESearchCase::IgnoreCase);
	}
	return ActualName.Equals(ExpectedName, ESearchCase::IgnoreCase);
}

bool GameTestCompareUtil::Components(const FGameTestValue& InValue, EGameTestKind InKind, TArray<double>& OutComponents)
{
	using namespace GameTestCompareRegistryImpl;
	OutComponents.Reset();
	if (InValue.Type == EGameTestValueType::Vector)
	{
		const FVector& Vector = InValue.GetVector();
		OutComponents = { Vector.X, Vector.Y, Vector.Z };
	}
	else if (InValue.Type == EGameTestValueType::Array)
	{
		for (const FGameTestValue& Item : InValue.GetArray())
		{
			double Number = 0.0;
			if (!Item.TryNumber(Number))
			{
				return false;
			}
			OutComponents.Add(Number);
		}
	}
	else if (InValue.IsString() && InKind == EGameTestKind::Color && InValue.GetString().StartsWith(TEXT("#")))
	{
		// 十六进制按 0–255 原样换成 0–1，不做 sRGB 转换（和属性里存的 FColor 比）
		const FLinearColor Color = FColor::FromHex(InValue.GetString()).ReinterpretAsLinear();
		OutComponents = { Color.R, Color.G, Color.B, Color.A };
		return true;
	}
	else if (InValue.Type == EGameTestValueType::Map)
	{
		FGameTestValue Specified;
		if (InKind == EGameTestKind::Color && InValue.TryGetField(TEXT("SpecifiedColor"), Specified))
		{
			return Components(Specified, InKind, OutComponents);
		}
		const TArray<const TCHAR*> Names = InKind == EGameTestKind::Rotator ? TArray<const TCHAR*>{ TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll") }
			: InKind == EGameTestKind::Vector2 ? TArray<const TCHAR*>{ TEXT("X"), TEXT("Y") }
			: InKind == EGameTestKind::Color ? TArray<const TCHAR*>{ TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") }
			: TArray<const TCHAR*>{ TEXT("X"), TEXT("Y"), TEXT("Z") };
		for (const TCHAR* Name : Names)
		{
			double Number = 0.0;
			if (!NumberOf(InValue, Name, Number))
			{
				// 颜色的 A 可以不写
				if (InKind == EGameTestKind::Color && FCString::Strcmp(Name, TEXT("A")) == 0)
				{
					OutComponents.Add(1.0);
					continue;
				}
				return false;
			}
			OutComponents.Add(Number);
		}
	}
	else
	{
		return false;
	}
	// FColor 是 0–255：有一个分量超过 1 就当 0–255 换算，和 FLinearColor 比
	if (InKind == EGameTestKind::Color && OutComponents.ContainsByPredicate([](double V) { return V > 1.0; }))
	{
		for (double& Value : OutComponents)
		{
			Value /= 255.0;
		}
	}
	return OutComponents.Num() > 0;
}

bool GameTestCompareUtil::ComponentsNear(const FGameTestValue& InActual, const FGameTestValue& InExpected, EGameTestKind InKind, double InTol)
{
	TArray<double> Actual;
	TArray<double> Expected;
	if (!Components(InActual, InKind, Actual) || !Components(InExpected, InKind, Expected))
	{
		return false;
	}
	// 期望少写分量（颜色不写 A、二维当三维的前两项）：只比写了的
	const int32 Count = FMath::Min(Actual.Num(), Expected.Num());
	if (Count == 0)
	{
		return false;
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		double Diff = Actual[Index] - Expected[Index];
		if (InKind == EGameTestKind::Rotator)
		{
			Diff = FRotator::NormalizeAxis(Diff);
		}
		if (FMath::Abs(Diff) > InTol)
		{
			return false;
		}
	}
	return true;
}

bool GameTestCompareUtil::ValueEquals(const FGameTestValue& InActual, const FGameTestValue& InExpected, EGameTestKind InKind, bool bInIgnoreCase, double InTol)
{
	switch (InKind)
	{
	case EGameTestKind::Int:
	case EGameTestKind::Float:
	{
		double Actual = 0.0;
		double Expected = 0.0;
		if (InActual.TryNumber(Actual) && InExpected.TryNumber(Expected))
		{
			return FMath::Abs(Actual - Expected) <= InTol;
		}
		return false;
	}
	case EGameTestKind::Enum:
	case EGameTestKind::Visibility:
		return EnumEquals(InActual, InExpected);
	case EGameTestKind::Asset:
		return AssetEquals(TextOf(InActual), TextOf(InExpected));
	case EGameTestKind::Vector:
	case EGameTestKind::Rotator:
	case EGameTestKind::Vector2:
	case EGameTestKind::Color:
		return ComponentsNear(InActual, InExpected, InKind, InTol);
	case EGameTestKind::Bool:
		return InActual.Truthy() == InExpected.Truthy();
	case EGameTestKind::String:
	case EGameTestKind::Text:
		return TextEquals(TextOf(InActual), TextOf(InExpected), bInIgnoreCase);
	default:
		return InActual.LooseEquals(InExpected);
	}
}

FGameTestCompareResult GameTestCompareUtil::Result(bool bInHolds, const FGameTestCompareInput& InInput, const FString& InVerb)
{
	FGameTestCompareResult Result;
	Result.bHolds = bInHolds;
	if (!bInHolds)
	{
		Result.Why = InInput.Expected.IsNull()
			? FString::Printf(TEXT("期望%s，实际 %s"), *InVerb, *InInput.Actual.Value.ToDisplay(80))
			: FString::Printf(TEXT("期望%s %s，实际 %s"), *InVerb, *InInput.Expected.ToDisplay(80), *InInput.Actual.Value.ToDisplay(80));
	}
	return Result;
}

void GameTestCompareUtil::ExpectArray(const TSharedPtr<FJsonValue>& InValue, int32 InExactCount, FGameTestDiagSink& InSink)
{
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!InValue.IsValid() || !InValue->TryGetArray(Items) || Items == nullptr)
	{
		InSink.Error(TEXT("bad_type"), InExactCount > 0 ? FString::Printf(TEXT("value 要写成 %d 项的数组"), InExactCount) : FString(TEXT("value 要写成数组")));
		return;
	}
	if (InExactCount > 0 && Items->Num() != InExactCount)
	{
		InSink.Error(TEXT("bad_type"), FString::Printf(TEXT("value 要写成 %d 项的数组（现在 %d 项）"), InExactCount, Items->Num()));
	}
}

void GameTestCompareUtil::ExpectNumber(const TSharedPtr<FJsonValue>& InValue, FGameTestDiagSink& InSink)
{
	double Number = 0.0;
	if (!InValue.IsValid() || !InValue->TryGetNumber(Number))
	{
		InSink.Error(TEXT("bad_type"), TEXT("value 要写数字"));
	}
}

void GameTestCompareUtil::ExpectString(const TSharedPtr<FJsonValue>& InValue, FGameTestDiagSink& InSink)
{
	if (!InValue.IsValid() || InValue->Type != EJson::String)
	{
		InSink.Error(TEXT("bad_type"), TEXT("value 要写文字"));
	}
}
