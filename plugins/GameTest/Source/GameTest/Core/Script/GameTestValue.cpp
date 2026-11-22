// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Script/GameTestValue.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"
#include "UObject/Object.h"

namespace
{
	const TArray<FGameTestValue>& EmptyArray()
	{
		static const TArray<FGameTestValue> Empty;
		return Empty;
	}

	const TMap<FString, FGameTestValue>& EmptyMap()
	{
		static const TMap<FString, FGameTestValue> Empty;
		return Empty;
	}

	FString Clip(const FString& InText, int32 InMax)
	{
		if (InMax <= 0 || InText.Len() <= InMax)
		{
			return InText;
		}
		return InText.Left(FMath::Max(1, InMax - 1)) + TEXT("…");
	}

	FString NumberText(double InNumber)
	{
		// 整数不带小数点（「3」不是「3.000000」），其余保留到够看清的位数。
		if (FMath::IsFinite(InNumber) && FMath::Abs(InNumber) < 1e15 && FMath::FloorToDouble(InNumber) == InNumber)
		{
			return FString::Printf(TEXT("%lld"), static_cast<int64>(InNumber));
		}
		return FString::SanitizeFloat(InNumber);
	}
}

FGameTestValue FGameTestValue::Null()
{
	return FGameTestValue();
}

FGameTestValue FGameTestValue::FromBool(bool bIn)
{
	FGameTestValue Value;
	Value.Type = EGameTestValueType::Bool;
	Value.bValue = bIn;
	return Value;
}

FGameTestValue FGameTestValue::FromNumber(double InNumber)
{
	FGameTestValue Value;
	Value.Type = EGameTestValueType::Number;
	Value.Number = InNumber;
	return Value;
}

FGameTestValue FGameTestValue::FromString(const FString& InText)
{
	FGameTestValue Value;
	Value.Type = EGameTestValueType::String;
	Value.Text = InText;
	return Value;
}

FGameTestValue FGameTestValue::FromArray(TArray<FGameTestValue>&& InItems)
{
	FGameTestValue Value;
	Value.Type = EGameTestValueType::Array;
	Value.Items = MakeShared<const TArray<FGameTestValue>>(MoveTemp(InItems));
	return Value;
}

FGameTestValue FGameTestValue::FromArray(const TArray<FGameTestValue>& InItems)
{
	TArray<FGameTestValue> Copy = InItems;
	return FromArray(MoveTemp(Copy));
}

FGameTestValue FGameTestValue::FromMap(TMap<FString, FGameTestValue>&& InFields)
{
	FGameTestValue Value;
	Value.Type = EGameTestValueType::Map;
	Value.Fields = MakeShared<const TMap<FString, FGameTestValue>>(MoveTemp(InFields));
	return Value;
}

FGameTestValue FGameTestValue::FromMap(const TMap<FString, FGameTestValue>& InFields)
{
	TMap<FString, FGameTestValue> Copy = InFields;
	return FromMap(MoveTemp(Copy));
}

FGameTestValue FGameTestValue::FromVector(const FVector& InVector)
{
	FGameTestValue Value;
	Value.Type = EGameTestValueType::Vector;
	Value.Vec = InVector;
	return Value;
}

FGameTestValue FGameTestValue::FromObject(UObject* InObject)
{
	if (!InObject)
	{
		return Null();
	}
	FGameTestValue Value;
	Value.Type = EGameTestValueType::Object;
	Value.Obj = InObject;
	return Value;
}

FGameTestValue FGameTestValue::Unresolved(const FString& InReason, FName InCode)
{
	FGameTestValue Value;
	Value.Type = EGameTestValueType::Unresolved;
	Value.Text = InReason;
	Value.Code = InCode.IsNone() ? FName(TEXT("unresolved")) : InCode;
	return Value;
}

UObject* FGameTestValue::GetObject() const
{
	return Type == EGameTestValueType::Object ? Obj.Get() : nullptr;
}

const TArray<FGameTestValue>& FGameTestValue::GetArray() const
{
	return (Type == EGameTestValueType::Array && Items.IsValid()) ? *Items : EmptyArray();
}

const TMap<FString, FGameTestValue>& FGameTestValue::GetMap() const
{
	return (Type == EGameTestValueType::Map && Fields.IsValid()) ? *Fields : EmptyMap();
}

bool FGameTestValue::TryGetField(const FString& InName, FGameTestValue& OutValue) const
{
	if (Type == EGameTestValueType::Map && Fields.IsValid())
	{
		if (const FGameTestValue* Exact = Fields->Find(InName))
		{
			OutValue = *Exact;
			return true;
		}
		// 属性名忽略大小写：蓝图里显示的名字和 C++ 里的写法常差个大小写，作者照着界面写不该取不到。
		for (const TPair<FString, FGameTestValue>& Pair : *Fields)
		{
			if (Pair.Key.Equals(InName, ESearchCase::IgnoreCase))
			{
				OutValue = Pair.Value;
				return true;
			}
		}
		return false;
	}
	if (Type == EGameTestValueType::Vector)
	{
		const int32 Axis = InName.Equals(TEXT("X"), ESearchCase::IgnoreCase) ? 0
			: InName.Equals(TEXT("Y"), ESearchCase::IgnoreCase) ? 1
			: InName.Equals(TEXT("Z"), ESearchCase::IgnoreCase) ? 2 : -1;
		if (Axis >= 0)
		{
			OutValue = FromNumber(Vec[Axis]);
			return true;
		}
	}
	return false;
}

int32 FGameTestValue::Len() const
{
	switch (Type)
	{
	case EGameTestValueType::Array:
		return GetArray().Num();
	case EGameTestValueType::Map:
		return GetMap().Num();
	case EGameTestValueType::String:
		return Text.Len();
	default:
		return -1;
	}
}

bool FGameTestValue::Truthy() const
{
	switch (Type)
	{
	case EGameTestValueType::Bool:
		return bValue;
	case EGameTestValueType::Number:
		return Number != 0.0 && !FMath::IsNaN(Number);
	case EGameTestValueType::String:
		return !Text.IsEmpty() && !Text.Equals(TEXT("false"), ESearchCase::IgnoreCase) && Text != TEXT("0");
	case EGameTestValueType::Array:
	case EGameTestValueType::Map:
		return Len() > 0;
	case EGameTestValueType::Vector:
		return true;
	case EGameTestValueType::Object:
		return Obj.IsValid();
	default:
		return false;
	}
}

bool FGameTestValue::TryNumber(double& OutNumber) const
{
	switch (Type)
	{
	case EGameTestValueType::Number:
		OutNumber = Number;
		return true;
	case EGameTestValueType::Bool:
		OutNumber = bValue ? 1.0 : 0.0;
		return true;
	case EGameTestValueType::String:
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || !Trimmed.IsNumeric())
		{
			return false;
		}
		OutNumber = FCString::Atod(*Trimmed);
		return true;
	}
	default:
		return false;
	}
}

FString FGameTestValue::ToDisplay(int32 InMaxChars) const
{
	switch (Type)
	{
	case EGameTestValueType::Null:
		return TEXT("null");
	case EGameTestValueType::Bool:
		return bValue ? TEXT("true") : TEXT("false");
	case EGameTestValueType::Number:
		return NumberText(Number);
	case EGameTestValueType::String:
		return Clip(Text, InMaxChars);
	case EGameTestValueType::Vector:
		return FString::Printf(TEXT("(X=%s,Y=%s,Z=%s)"), *NumberText(Vec.X), *NumberText(Vec.Y), *NumberText(Vec.Z));
	case EGameTestValueType::Object:
	{
		const UObject* Live = Obj.Get();
		return Live ? Live->GetName() : TEXT("<已销毁>");
	}
	case EGameTestValueType::Unresolved:
		return FString::Printf(TEXT("<取不到：%s>"), *Clip(Text, InMaxChars));
	case EGameTestValueType::Array:
	{
		FString Out = TEXT("[");
		for (int32 Index = 0; Index < GetArray().Num(); ++Index)
		{
			if (Index > 0)
			{
				Out += TEXT(", ");
			}
			Out += GetArray()[Index].ToDisplay(InMaxChars);
			if (InMaxChars > 0 && Out.Len() > InMaxChars)
			{
				break;
			}
		}
		return Clip(Out + TEXT("]"), InMaxChars);
	}
	case EGameTestValueType::Map:
	{
		FString Out = TEXT("{");
		bool bFirst = true;
		for (const TPair<FString, FGameTestValue>& Pair : GetMap())
		{
			Out += bFirst ? TEXT("") : TEXT(", ");
			bFirst = false;
			Out += Pair.Key + TEXT(": ") + Pair.Value.ToDisplay(InMaxChars);
			if (InMaxChars > 0 && Out.Len() > InMaxChars)
			{
				break;
			}
		}
		return Clip(Out + TEXT("}"), InMaxChars);
	}
	}
	return FString();
}

FString FGameTestValue::TypeName() const
{
	switch (Type)
	{
	case EGameTestValueType::Null: return TEXT("null");
	case EGameTestValueType::Bool: return TEXT("bool");
	case EGameTestValueType::Number: return TEXT("number");
	case EGameTestValueType::String: return TEXT("string");
	case EGameTestValueType::Array: return TEXT("array");
	case EGameTestValueType::Map: return TEXT("map");
	case EGameTestValueType::Vector: return TEXT("vector");
	case EGameTestValueType::Object: return TEXT("object");
	case EGameTestValueType::Unresolved: return TEXT("unresolved");
	}
	return TEXT("null");
}

bool FGameTestValue::LooseEquals(const FGameTestValue& InOther) const
{
	if (IsUnresolved() || InOther.IsUnresolved())
	{
		return false;
	}
	// 数字和数字字符串按数字比：控件文字读出来是 "3"，期望值写 3，不该判不等。
	double Left = 0.0;
	double Right = 0.0;
	const bool bLeftNumberish = Type == EGameTestValueType::Number || Type == EGameTestValueType::String;
	const bool bRightNumberish = InOther.Type == EGameTestValueType::Number || InOther.Type == EGameTestValueType::String;
	if (bLeftNumberish && bRightNumberish && (IsNumber() || InOther.IsNumber()) && TryNumber(Left) && InOther.TryNumber(Right))
	{
		return FMath::IsNearlyEqual(Left, Right, 1e-6);
	}
	if (Type != InOther.Type)
	{
		// 布尔和 "true" / "false" 字符串互认（Lua、配置表常把布尔写成字符串）。
		if (Type == EGameTestValueType::Bool && InOther.IsString())
		{
			return InOther.Text.Equals(bValue ? TEXT("true") : TEXT("false"), ESearchCase::IgnoreCase);
		}
		if (IsString() && InOther.Type == EGameTestValueType::Bool)
		{
			return InOther.LooseEquals(*this);
		}
		return false;
	}
	switch (Type)
	{
	case EGameTestValueType::Null:
		return true;
	case EGameTestValueType::Bool:
		return bValue == InOther.bValue;
	case EGameTestValueType::Number:
		return FMath::IsNearlyEqual(Number, InOther.Number, 1e-6);
	case EGameTestValueType::String:
		return Text.Equals(InOther.Text, ESearchCase::IgnoreCase);
	case EGameTestValueType::Vector:
		return Vec.Equals(InOther.Vec, 1e-3);
	case EGameTestValueType::Object:
		return Obj == InOther.Obj;
	case EGameTestValueType::Array:
	{
		const TArray<FGameTestValue>& A = GetArray();
		const TArray<FGameTestValue>& B = InOther.GetArray();
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (!A[Index].LooseEquals(B[Index]))
			{
				return false;
			}
		}
		return true;
	}
	case EGameTestValueType::Map:
	{
		const TMap<FString, FGameTestValue>& A = GetMap();
		if (A.Num() != InOther.GetMap().Num())
		{
			return false;
		}
		for (const TPair<FString, FGameTestValue>& Pair : A)
		{
			FGameTestValue Other;
			if (!InOther.TryGetField(Pair.Key, Other) || !Pair.Value.LooseEquals(Other))
			{
				return false;
			}
		}
		return true;
	}
	default:
		return false;
	}
}

bool FGameTestValue::Compare(const FGameTestValue& InOther, int32& OutOrder) const
{
	if (IsUnresolved() || InOther.IsUnresolved())
	{
		return false;
	}
	double Left = 0.0;
	double Right = 0.0;
	if (TryNumber(Left) && InOther.TryNumber(Right))
	{
		OutOrder = Left < Right ? -1 : (Left > Right ? 1 : 0);
		return true;
	}
	if (IsString() && InOther.IsString())
	{
		OutOrder = Text.Compare(InOther.Text, ESearchCase::IgnoreCase);
		OutOrder = OutOrder < 0 ? -1 : (OutOrder > 0 ? 1 : 0);
		return true;
	}
	return false;
}

TSharedPtr<FJsonValue> FGameTestValue::ToJson() const
{
	switch (Type)
	{
	case EGameTestValueType::Bool:
		return MakeShared<FJsonValueBoolean>(bValue);
	case EGameTestValueType::Number:
		return MakeShared<FJsonValueNumber>(Number);
	case EGameTestValueType::String:
		return MakeShared<FJsonValueString>(Text);
	case EGameTestValueType::Vector:
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("X"), Vec.X);
		Out->SetNumberField(TEXT("Y"), Vec.Y);
		Out->SetNumberField(TEXT("Z"), Vec.Z);
		return MakeShared<FJsonValueObject>(Out);
	}
	case EGameTestValueType::Object:
	{
		const UObject* Live = Obj.Get();
		return Live ? StaticCast<TSharedPtr<FJsonValue>>(MakeShared<FJsonValueString>(Live->GetPathName()))
					: StaticCast<TSharedPtr<FJsonValue>>(MakeShared<FJsonValueNull>());
	}
	case EGameTestValueType::Array:
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FGameTestValue& Item : GetArray())
		{
			Out.Add(Item.ToJson());
		}
		return MakeShared<FJsonValueArray>(Out);
	}
	case EGameTestValueType::Map:
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const TPair<FString, FGameTestValue>& Pair : GetMap())
		{
			Out->SetField(Pair.Key, Pair.Value.ToJson());
		}
		return MakeShared<FJsonValueObject>(Out);
	}
	default:
		return MakeShared<FJsonValueNull>();
	}
}

TSharedPtr<FJsonValue> FGameTestValue::ToTypedJson(int32 InMaxItems, int32 InMaxDepth) const
{
	// 和 ToJson 的区别：对象写成 {"$obj": 路径, class, name}（面板按它画成可展开的对象，再用 gt.props path 读成员），
	// 数组 / Map 超过 InMaxItems 截断并在 "$more" 里写还剩几个，超过 InMaxDepth 层写成显示文字，返回值再大也不会撑爆回包
	switch (Type)
	{
	case EGameTestValueType::Object:
	{
		const UObject* Live = Obj.Get();
		if (Live == nullptr)
		{
			return MakeShared<FJsonValueNull>();
		}
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("$obj"), Live->GetPathName());
		Out->SetStringField(TEXT("class"), Live->GetClass()->GetName());
		Out->SetStringField(TEXT("name"), Live->GetName());
		return MakeShared<FJsonValueObject>(Out);
	}
	case EGameTestValueType::Array:
	{
		if (InMaxDepth <= 0)
		{
			return MakeShared<FJsonValueString>(ToDisplay(200));
		}
		const TArray<FGameTestValue>& List = GetArray();
		TArray<TSharedPtr<FJsonValue>> Out;
		for (int32 Index = 0; Index < List.Num() && Index < InMaxItems; ++Index)
		{
			Out.Add(List[Index].ToTypedJson(InMaxItems, InMaxDepth - 1));
		}
		if (List.Num() > InMaxItems)
		{
			TSharedPtr<FJsonObject> More = MakeShared<FJsonObject>();
			More->SetNumberField(TEXT("$more"), List.Num() - InMaxItems);
			Out.Add(MakeShared<FJsonValueObject>(More));
		}
		return MakeShared<FJsonValueArray>(Out);
	}
	case EGameTestValueType::Map:
	{
		if (InMaxDepth <= 0)
		{
			return MakeShared<FJsonValueString>(ToDisplay(200));
		}
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		int32 Count = 0;
		for (const TPair<FString, FGameTestValue>& Pair : GetMap())
		{
			if (Count++ >= InMaxItems)
			{
				break;
			}
			Out->SetField(Pair.Key, Pair.Value.ToTypedJson(InMaxItems, InMaxDepth - 1));
		}
		if (GetMap().Num() > InMaxItems)
		{
			Out->SetNumberField(TEXT("$more"), GetMap().Num() - InMaxItems);
		}
		return MakeShared<FJsonValueObject>(Out);
	}
	default:
		return ToJson();
	}
}

FGameTestValue FGameTestValue::FromJson(const TSharedPtr<FJsonValue>& InJson)
{
	if (!InJson.IsValid())
	{
		return Null();
	}
	switch (InJson->Type)
	{
	case EJson::Boolean:
		return FromBool(InJson->AsBool());
	case EJson::Number:
		return FromNumber(InJson->AsNumber());
	case EJson::String:
		return FromString(InJson->AsString());
	case EJson::Array:
	{
		TArray<FGameTestValue> ArrayItems;
		for (const TSharedPtr<FJsonValue>& Item : InJson->AsArray())
		{
			ArrayItems.Add(FromJson(Item));
		}
		return FromArray(MoveTemp(ArrayItems));
	}
	case EJson::Object:
	{
		const TSharedPtr<FJsonObject> Object = InJson->AsObject();
		TMap<FString, FGameTestValue> MapFields;
		if (Object.IsValid())
		{
			for (const auto& Pair : Object->Values)
			{
				MapFields.Add(GAMETEST_JSON_KEY(Pair.Key), FromJson(Pair.Value));
			}
		}
		return FromMap(MoveTemp(MapFields));
	}
	default:
		return Null();
	}
}
