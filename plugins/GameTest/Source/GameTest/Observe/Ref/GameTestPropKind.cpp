// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestPropKind.h"

#include "Components/SlateWrapperTypes.h"
#include "Core/Script/GameTestValue.h"
#include "GameFramework/Actor.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

namespace GameTestPropKindImpl
{
	/**
	 * 是不是资源类（贴图、材质、声音、数据资产…）：资源引用归 `asset`（按名字或路径比），别的对象引用归 `object`。
	 * 按类名沿继承链认，不 include 各模块的头（Niagara、Paper2D 不一定开）。
	 */
	bool IsAssetClass(const UClass* InClass)
	{
		static const TSet<FName> AssetBases = {
			TEXT("Texture"), TEXT("MaterialInterface"), TEXT("SoundBase"), TEXT("StreamableRenderAsset"), TEXT("DataAsset"),
			TEXT("DataTable"), TEXT("CurveBase"), TEXT("Font"), TEXT("FontFace"), TEXT("AnimationAsset"), TEXT("ParticleSystem"),
			TEXT("NiagaraSystem"), TEXT("PaperSprite"), TEXT("SlateWidgetStyleAsset"),
		};
		for (const UClass* Class = InClass; Class != nullptr; Class = Class->GetSuperClass())
		{
			if (AssetBases.Contains(Class->GetFName()))
			{
				return true;
			}
		}
		return false;
	}

	EGameTestKind OfEnum(const UEnum* InEnum)
	{
		return InEnum != nullptr && InEnum == StaticEnum<ESlateVisibility>() ? EGameTestKind::Visibility : EGameTestKind::Enum;
	}

	EGameTestKind OfStruct(const UScriptStruct* InStruct, FString* OutWhy)
	{
		const FName Name = InStruct != nullptr ? InStruct->GetFName() : NAME_None;
		if (Name == NAME_Vector || Name == TEXT("Vector3f") || Name == TEXT("IntVector"))
		{
			return EGameTestKind::Vector;
		}
		if (Name == NAME_Rotator || Name == TEXT("Rotator3f"))
		{
			return EGameTestKind::Rotator;
		}
		if (Name == NAME_Vector2D || Name == TEXT("Vector2f") || Name == TEXT("IntPoint"))
		{
			return EGameTestKind::Vector2;
		}
		if (Name == NAME_LinearColor || Name == NAME_Color || Name == TEXT("SlateColor"))
		{
			return EGameTestKind::Color;
		}
		if (Name == TEXT("GameplayTag"))
		{
			return EGameTestKind::String;
		}
		if (Name == TEXT("GameplayTagContainer"))
		{
			return EGameTestKind::Array;
		}
		if (OutWhy != nullptr)
		{
			*OutWhy = TEXT("结构体不能直接比，展开它的成员选（@X.成员）");
		}
		return EGameTestKind::Struct;
	}
}

EGameTestKind FGameTestPropKind::Of(const FProperty* InProp, FString* OutWhy)
{
	using namespace GameTestPropKindImpl;
	if (InProp == nullptr)
	{
		if (OutWhy != nullptr)
		{
			*OutWhy = TEXT("没有这个属性");
		}
		return EGameTestKind::None;
	}
	if (InProp->ArrayDim > 1)
	{
		return EGameTestKind::Array;
	}
	if (CastField<FBoolProperty>(InProp))
	{
		return EGameTestKind::Bool;
	}
	if (const FEnumProperty* Enum = CastField<FEnumProperty>(InProp))
	{
		return OfEnum(Enum->GetEnum());
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(InProp))
	{
		if (const UEnum* Enum = Numeric->GetIntPropertyEnum())
		{
			return OfEnum(Enum);
		}
		return Numeric->IsFloatingPoint() ? EGameTestKind::Float : EGameTestKind::Int;
	}
	if (CastField<FStrProperty>(InProp) || CastField<FNameProperty>(InProp))
	{
		return EGameTestKind::String;
	}
	if (CastField<FTextProperty>(InProp))
	{
		return EGameTestKind::Text;
	}
	if (CastField<FClassProperty>(InProp) || CastField<FSoftClassProperty>(InProp))
	{
		return EGameTestKind::Object;
	}
	if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(InProp))
	{
		// 软引用多半指资源；指 Actor 的（关卡里的软 Actor 引用）按对象比
		return Soft->PropertyClass != nullptr && Soft->PropertyClass->IsChildOf(AActor::StaticClass()) ? EGameTestKind::Object : EGameTestKind::Asset;
	}
	if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(InProp))
	{
		return IsAssetClass(Object->PropertyClass) ? EGameTestKind::Asset : EGameTestKind::Object;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(InProp))
	{
		return OfStruct(Struct->Struct, OutWhy);
	}
	if (CastField<FArrayProperty>(InProp) || CastField<FSetProperty>(InProp) || CastField<FMapProperty>(InProp))
	{
		return EGameTestKind::Array;
	}
	if (OutWhy != nullptr)
	{
		*OutWhy = CastField<FDelegateProperty>(InProp) || CastField<FMulticastDelegateProperty>(InProp) ? TEXT("委托不能当标识也不能判断")
			: CastField<FInterfaceProperty>(InProp) ? TEXT("接口引用不能当标识也不能判断")
			: FString::Printf(TEXT("%s 类型不能当标识也不能判断"), *InProp->GetClass()->GetName());
	}
	return EGameTestKind::None;
}

EGameTestKind FGameTestPropKind::OfValue(const FGameTestValue& InValue)
{
	switch (InValue.Type)
	{
	case EGameTestValueType::Bool:
		return EGameTestKind::Bool;
	case EGameTestValueType::Number:
		return EGameTestKind::Float;
	case EGameTestValueType::String:
		return EGameTestKind::String;
	case EGameTestValueType::Array:
	case EGameTestValueType::Map:
		return EGameTestKind::Array;
	case EGameTestValueType::Vector:
		return EGameTestKind::Vector;
	case EGameTestValueType::Object:
		return EGameTestKind::Object;
	default:
		return EGameTestKind::None;
	}
}

const TCHAR* FGameTestPropKind::Name(EGameTestKind InKind)
{
	switch (InKind)
	{
	case EGameTestKind::Bool: return TEXT("bool");
	case EGameTestKind::Int: return TEXT("int");
	case EGameTestKind::Float: return TEXT("float");
	case EGameTestKind::String: return TEXT("string");
	case EGameTestKind::Text: return TEXT("text");
	case EGameTestKind::Enum: return TEXT("enum");
	case EGameTestKind::Visibility: return TEXT("visibility");
	case EGameTestKind::Object: return TEXT("object");
	case EGameTestKind::Asset: return TEXT("asset");
	case EGameTestKind::Vector: return TEXT("vector");
	case EGameTestKind::Rotator: return TEXT("rotator");
	case EGameTestKind::Vector2: return TEXT("vector2");
	case EGameTestKind::Color: return TEXT("color");
	case EGameTestKind::Array: return TEXT("array");
	case EGameTestKind::Struct: return TEXT("struct");
	default: return TEXT("none");
	}
}

bool FGameTestPropKind::FromName(const FString& InName, EGameTestKind& OutKind)
{
	for (uint8 Raw = 0; Raw <= static_cast<uint8>(EGameTestKind::Struct); ++Raw)
	{
		const EGameTestKind Kind = static_cast<EGameTestKind>(Raw);
		if (InName.Equals(Name(Kind), ESearchCase::IgnoreCase))
		{
			OutKind = Kind;
			return true;
		}
	}
	return false;
}

bool FGameTestPropKind::IsComparable(EGameTestKind InKind)
{
	return InKind != EGameTestKind::None && InKind != EGameTestKind::Struct;
}

FString FGameTestPropKind::TypeName(const FProperty* InProp)
{
	if (InProp == nullptr)
	{
		return FString();
	}
	FString Extended;
	const FString Base = InProp->GetCPPType(&Extended);
	return Base + Extended;
}

bool FGameTestPropKind::Accepts(EGameTestKind InOpKind, EGameTestKind InValueKind)
{
	if (InOpKind == InValueKind)
	{
		return true;
	}
	// int / float 互收（Lua 数字整数值也收 int 的 op）；visibility 是 enum 的特例，enum 的 op 它都收
	const bool bNumbers = (InOpKind == EGameTestKind::Int || InOpKind == EGameTestKind::Float)
		&& (InValueKind == EGameTestKind::Int || InValueKind == EGameTestKind::Float);
	return bNumbers || (InOpKind == EGameTestKind::Enum && InValueKind == EGameTestKind::Visibility);
}
