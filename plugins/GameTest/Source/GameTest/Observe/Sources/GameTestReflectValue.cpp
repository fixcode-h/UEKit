// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Sources/GameTestReflectValue.h"

#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
	/** 结构体、容器嵌套最多展开几层：防止自引用的结构把一次取值拖成几万个节点。 */
	constexpr int32 MaxDepth = 8;
	/** 一个容器最多转多少项：条件里比的是个数和少数几项，大数组全转太慢。 */
	constexpr int32 MaxItems = 2000;

	FGameTestValue Convert(const FProperty* InProp, const void* InValue, int32 InDepth);

	FGameTestValue ConvertStruct(const UScriptStruct* InStruct, const void* InValue, int32 InDepth)
	{
		const FName Name = InStruct->GetFName();
		// 向量类：成向量，能和 vec()、dist() 直接算
		if (Name == NAME_Vector)
		{
			return FGameTestValue::FromVector(*static_cast<const FVector*>(InValue));
		}
		// FGameplayTag / FGameplayTagContainer：按引擎反射读，不依赖 GameplayTags 模块（它是可选的）
		if (Name == TEXT("GameplayTag"))
		{
			if (const FNameProperty* TagName = CastField<FNameProperty>(InStruct->FindPropertyByName(TEXT("TagName"))))
			{
				return FGameTestValue::FromString(TagName->GetPropertyValue_InContainer(InValue).ToString());
			}
		}
		if (Name == TEXT("GameplayTagContainer"))
		{
			if (const FArrayProperty* Tags = CastField<FArrayProperty>(InStruct->FindPropertyByName(TEXT("GameplayTags"))))
			{
				return Convert(Tags, Tags->ContainerPtrToValuePtr<void>(InValue), InDepth);
			}
		}
		TMap<FString, FGameTestValue> Fields;
		for (TFieldIterator<FProperty> It(InStruct); It; ++It)
		{
			Fields.Add(It->GetName(), Convert(*It, It->ContainerPtrToValuePtr<void>(InValue), InDepth + 1));
		}
		return FGameTestValue::FromMap(MoveTemp(Fields));
	}

	FGameTestValue Convert(const FProperty* InProp, const void* InValue, int32 InDepth)
	{
		if (InProp == nullptr || InValue == nullptr)
		{
			return FGameTestValue::Null();
		}
		if (InDepth > MaxDepth)
		{
			return FGameTestValue::Unresolved(TEXT("嵌套太深，按更具体的路径取"));
		}
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(InProp))
		{
			return FGameTestValue::FromBool(Bool->GetPropertyValue(InValue));
		}
		if (const FEnumProperty* Enum = CastField<FEnumProperty>(InProp))
		{
			const int64 Raw = Enum->GetUnderlyingProperty()->GetSignedIntPropertyValue(InValue);
			const UEnum* Type = Enum->GetEnum();
			return Type ? FGameTestValue::FromString(Type->GetNameStringByValue(Raw)) : FGameTestValue::FromNumber((double)Raw);
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(InProp))
		{
			// 老式 TEnumAsByte：按枚举名给，和 EnumProperty 一样
			if (const UEnum* Type = Numeric->GetIntPropertyEnum())
			{
				return FGameTestValue::FromString(Type->GetNameStringByValue(Numeric->GetSignedIntPropertyValue(InValue)));
			}
			return Numeric->IsFloatingPoint() ? FGameTestValue::FromNumber(Numeric->GetFloatingPointPropertyValue(InValue))
											  : FGameTestValue::FromNumber((double)Numeric->GetSignedIntPropertyValue(InValue));
		}
		if (const FStrProperty* Str = CastField<FStrProperty>(InProp))
		{
			return FGameTestValue::FromString(Str->GetPropertyValue(InValue));
		}
		if (const FNameProperty* Name = CastField<FNameProperty>(InProp))
		{
			return FGameTestValue::FromString(Name->GetPropertyValue(InValue).ToString());
		}
		if (const FTextProperty* Text = CastField<FTextProperty>(InProp))
		{
			return FGameTestValue::FromString(Text->GetPropertyValue(InValue).ToString());
		}
		if (const FClassProperty* Class = CastField<FClassProperty>(InProp))
		{
			const UObject* Value = Class->GetObjectPropertyValue(InValue);
			return Value ? FGameTestValue::FromString(Value->GetName()) : FGameTestValue::Null();
		}
		if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(InProp))
		{
			// 软引用不加载：给路径；已经加载了就给对象，能继续往下取
			const FSoftObjectPtr& Ptr = *static_cast<const FSoftObjectPtr*>(InValue);
			if (UObject* Loaded = Ptr.Get())
			{
				return FGameTestValue::FromObject(Loaded);
			}
			return Ptr.IsNull() ? FGameTestValue::Null() : FGameTestValue::FromString(Ptr.ToSoftObjectPath().ToString());
		}
		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(InProp))
		{
			return FGameTestValue::FromObject(Object->GetObjectPropertyValue(InValue));
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(InProp))
		{
			return Struct->Struct ? ConvertStruct(Struct->Struct, InValue, InDepth) : FGameTestValue::Null();
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(InProp))
		{
			FScriptArrayHelper Helper(Array, InValue);
			TArray<FGameTestValue> Items;
			const int32 Count = FMath::Min(Helper.Num(), MaxItems);
			Items.Reserve(Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Items.Add(Convert(Array->Inner, Helper.GetRawPtr(Index), InDepth + 1));
			}
			return FGameTestValue::FromArray(MoveTemp(Items));
		}
		if (const FSetProperty* Set = CastField<FSetProperty>(InProp))
		{
			FScriptSetHelper Helper(Set, InValue);
			TArray<FGameTestValue> Items;
			for (int32 Index = 0; Index < Helper.GetMaxIndex() && Items.Num() < MaxItems; ++Index)
			{
				if (Helper.IsValidIndex(Index))
				{
					Items.Add(Convert(Set->ElementProp, Helper.GetElementPtr(Index), InDepth + 1));
				}
			}
			return FGameTestValue::FromArray(MoveTemp(Items));
		}
		if (const FMapProperty* Map = CastField<FMapProperty>(InProp))
		{
			FScriptMapHelper Helper(Map, InValue);
			TMap<FString, FGameTestValue> Fields;
			for (int32 Index = 0; Index < Helper.GetMaxIndex() && Fields.Num() < MaxItems; ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				// Map 的键一律转成文字：数字键 1001 写成 "1001"，脚本里 $m["1001"] 和 $m[1001] 都能取到（Index 按文字找）
				const FGameTestValue Key = Convert(Map->KeyProp, Helper.GetKeyPtr(Index), InDepth + 1);
				const FString KeyText = Key.IsString() ? Key.GetString() : Key.ToDisplay(200);
				Fields.Add(KeyText, Convert(Map->ValueProp, Helper.GetValuePtr(Index), InDepth + 1));
			}
			return FGameTestValue::FromMap(MoveTemp(Fields));
		}
		// 委托、字段路径之类：给类型名，不当成取不到（「这个属性在」本身有用）
		return FGameTestValue::FromString(FString::Printf(TEXT("<%s>"), *InProp->GetClass()->GetName()));
	}
}

namespace GameTestReflectValue
{
FGameTestValue FromProperty(const FProperty* InProp, const void* InValue)
{
	return Convert(InProp, InValue, 0);
}

FProperty* FindProperty(const UStruct* InStruct, const FString& InName)
{
	if (InStruct == nullptr || InName.IsEmpty())
	{
		return nullptr;
	}
	if (FProperty* Exact = InStruct->FindPropertyByName(FName(*InName)))
	{
		return Exact;
	}
	for (TFieldIterator<FProperty> It(InStruct); It; ++It)
	{
		if (It->GetName().Equals(InName, ESearchCase::IgnoreCase))
		{
			return *It;
		}
		// 蓝图里 bool 变量常显示成不带 b 的名字（「Is Open」 ↔ bIsOpen）
		if (CastField<FBoolProperty>(*It) && It->GetName().StartsWith(TEXT("b"), ESearchCase::CaseSensitive)
			&& It->GetName().RightChop(1).Equals(InName, ESearchCase::IgnoreCase))
		{
			return *It;
		}
	}
	return nullptr;
}

UFunction* FindFunction(const UClass* InClass, const FString& InName)
{
	if (InClass == nullptr || InName.IsEmpty())
	{
		return nullptr;
	}
	if (UFunction* Exact = InClass->FindFunctionByName(FName(*InName)))
	{
		return Exact;
	}
	for (TFieldIterator<UFunction> It(InClass); It; ++It)
	{
		if (It->GetName().Equals(InName, ESearchCase::IgnoreCase))
		{
			return *It;
		}
	}
	return nullptr;
}

bool IsReadOnlyFunction(const UFunction& InFunction)
{
	return InFunction.HasAnyFunctionFlags(FUNC_BlueprintPure | FUNC_Const);
}

bool IsCallableFunction(const UFunction& InFunction, FString& OutWhy)
{
	if (!InFunction.HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_Exec))
	{
		OutWhy = FString::Printf(TEXT("%s 不是 BlueprintCallable / BlueprintPure / Exec 函数，脚本调不了"), *InFunction.GetName());
		return false;
	}
	if (InFunction.HasAnyFunctionFlags(FUNC_Private))
	{
		OutWhy = FString::Printf(TEXT("%s 是私有函数"), *InFunction.GetName());
		return false;
	}
	if (InFunction.HasAnyFunctionFlags(FUNC_Delegate))
	{
		OutWhy = FString::Printf(TEXT("%s 是委托签名，不是函数"), *InFunction.GetName());
		return false;
	}
	return true;
}
}
