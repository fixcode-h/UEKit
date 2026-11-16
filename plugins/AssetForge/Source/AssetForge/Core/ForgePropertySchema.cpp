// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ForgePropertySchema.h"

#include "Core/ForgeClassResolver.h"
#include "Core/ForgePropertyCodec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/EnumProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace ForgePropertySchema
{
namespace
{
	// 枚举取值和结构体字段名最多列这么多：返回整体要在 12000 字以内（读结构的上限，面板 ADR 0067）
	constexpr int32 MaxEnumValues = 24;
	constexpr int32 MaxStructFields = 16;
	constexpr int32 MaxLimit = 150;

	TArray<TSharedPtr<FJsonValue>> EnumNames(const UEnum* InEnum)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		// 最后一项常是自动生成的 _MAX，不能写进配置
		const int32 Count = InEnum->NumEnums() - (InEnum->ContainsExistingMax() ? 1 : 0);
		for (int32 Index = 0; Index < Count && Out.Num() < MaxEnumValues; ++Index)
		{
#if WITH_METADATA
			if (InEnum->HasMetaData(TEXT("Hidden"), Index))
			{
				continue;
			}
#endif
			// FJsonObjectConverter 写枚举用的是去掉 `EFoo::` 前缀的名字
			Out.Add(MakeShared<FJsonValueString>(InEnum->GetNameStringByIndex(Index)));
		}
		return Out;
	}

	void SetEnum(const TSharedRef<FJsonObject>& Out, const UEnum* InEnum)
	{
		TArray<TSharedPtr<FJsonValue>> Names = EnumNames(InEnum);
		const int32 Total = InEnum->NumEnums() - (InEnum->ContainsExistingMax() ? 1 : 0);
		if (Names.Num() >= MaxEnumValues && Total > Names.Num())
		{
			// 超长的枚举不列全，标出类型名让人去查
			Out->SetStringField(TEXT("description"), FString::Printf(TEXT("%s 共 %d 个取值，只列了前 %d 个"), *InEnum->GetName(), Total, Names.Num()));
		}
		Out->SetArrayField(TEXT("enum"), Names);
	}

	/** 一个属性 → JSON Schema 片段。数组 / 集合只展开一层元素类型，结构体只列字段名，不递归。 */
	TSharedRef<FJsonObject> PropertyToSchema(const FProperty* InProperty)
	{
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();

		if (CastField<FBoolProperty>(InProperty))
		{
			Out->SetStringField(TEXT("type"), TEXT("boolean"));
		}
		else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(InProperty))
		{
			Out->SetStringField(TEXT("type"), TEXT("string"));
			SetEnum(Out, EnumProperty->GetEnum());
		}
		else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(InProperty); ByteProperty && ByteProperty->Enum)
		{
			Out->SetStringField(TEXT("type"), TEXT("string"));
			SetEnum(Out, ByteProperty->Enum);
		}
		else if (const FNumericProperty* Numeric = CastField<FNumericProperty>(InProperty))
		{
			Out->SetStringField(TEXT("type"), Numeric->IsInteger() ? TEXT("integer") : TEXT("number"));
		}
		else if (CastField<FStrProperty>(InProperty) || CastField<FNameProperty>(InProperty) || CastField<FTextProperty>(InProperty))
		{
			Out->SetStringField(TEXT("type"), TEXT("string"));
		}
		else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(InProperty))
		{
			// 对象引用写资产路径；类引用写类路径（/Script/... 或蓝图的 _C）
			Out->SetStringField(TEXT("type"), TEXT("string"));
			const UClass* Target = ObjectProperty->PropertyClass;
			if (const FClassProperty* ClassProperty = CastField<FClassProperty>(InProperty))
			{
				Target = ClassProperty->MetaClass;
			}
			else if (const FSoftClassProperty* SoftClass = CastField<FSoftClassProperty>(InProperty))
			{
				Target = SoftClass->MetaClass;
			}
			if (Target != nullptr)
			{
				Out->SetStringField(TEXT("description"), FString::Printf(TEXT("%s 路径"), *Target->GetName()));
			}
		}
		else if (const FStructProperty* StructProperty = CastField<FStructProperty>(InProperty))
		{
			Out->SetStringField(TEXT("type"), TEXT("object"));
			TArray<FString> Fields;
			int32 Total = 0;
			for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
			{
				if (++Total <= MaxStructFields)
				{
					Fields.Add(It->GetName());
				}
			}
			FString Description = FString::Printf(TEXT("%s：%s"), *StructProperty->Struct->GetName(), *FString::Join(Fields, TEXT(", ")));
			if (Total > MaxStructFields)
			{
				Description += FString::Printf(TEXT(" …共 %d 个字段"), Total);
			}
			Out->SetStringField(TEXT("description"), Description);
		}
		else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(InProperty))
		{
			Out->SetStringField(TEXT("type"), TEXT("array"));
			Out->SetObjectField(TEXT("items"), PropertyToSchema(ArrayProperty->Inner));
		}
		else if (const FSetProperty* SetProperty = CastField<FSetProperty>(InProperty))
		{
			Out->SetStringField(TEXT("type"), TEXT("array"));
			Out->SetObjectField(TEXT("items"), PropertyToSchema(SetProperty->ElementProp));
		}
		else if (const FMapProperty* MapProperty = CastField<FMapProperty>(InProperty))
		{
			Out->SetStringField(TEXT("type"), TEXT("object"));
			Out->SetObjectField(TEXT("additionalProperties"), PropertyToSchema(MapProperty->ValueProp));
		}

		Out->SetStringField(TEXT("x-ue"), InProperty->GetCPPType());
		return Out;
	}

	/** 结构体：全路径直接加载；短名（可带 F 前缀）在已加载的结构体里找。 */
	UScriptStruct* ResolveStruct(const FString& InName)
	{
		if (InName.StartsWith(TEXT("/")))
		{
			return LoadObject<UScriptStruct>(nullptr, *InName, nullptr, LOAD_Quiet | LOAD_NoWarn);
		}
		FString Name = InName;
		if (UScriptStruct* Found = FindFirstObject<UScriptStruct>(*Name, EFindFirstObjectOptions::None))
		{
			return Found;
		}
		if (Name.Len() > 1 && Name[0] == TEXT('F'))
		{
			Name.RightChopInline(1);
			return FindFirstObject<UScriptStruct>(*Name, EFindFirstObjectOptions::None);
		}
		return nullptr;
	}
}

bool Describe(const FQuery& InQuery, const TSharedPtr<FJsonObject>& OutJson, FString& OutError)
{
	const FString Type = InQuery.Type.TrimStartAndEnd();
	if (Type.IsEmpty())
	{
		OutError = TEXT("缺少 class：要看哪个类或结构体的属性（如 /Script/Engine.WorldSettings、TextBlock）");
		return false;
	}

	const UStruct* Struct = nullptr;
	FString ClassError;
	if (UClass* Class = FForgeClassResolver::Resolve(Type, nullptr, ClassError))
	{
		Struct = Class;
	}
	else if (UScriptStruct* ScriptStruct = ResolveStruct(Type))
	{
		Struct = ScriptStruct;
	}
	if (Struct == nullptr)
	{
		OutError = FString::Printf(TEXT("%s；也不是已加载的结构体。写全路径试试（/Script/<模块>.<类名>）"), *ClassError);
		return false;
	}

	const FString Filter = InQuery.Filter.TrimStartAndEnd();
	TArray<const FProperty*> Matched;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		if (ForgePropertyCodec::IsCodecProperty(*It) && (Filter.IsEmpty() || It->GetName().Contains(Filter)))
		{
			Matched.Add(*It);
		}
	}

	const int32 Offset = FMath::Clamp(InQuery.Offset, 0, Matched.Num());
	const int32 Limit = FMath::Clamp(InQuery.Limit, 1, MaxLimit);
	const int32 End = FMath::Min(Matched.Num(), Offset + Limit);
	const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
	for (int32 Index = Offset; Index < End; ++Index)
	{
		Properties->SetObjectField(Matched[Index]->GetName(), PropertyToSchema(Matched[Index]));
	}

	OutJson->SetStringField(TEXT("class"), Struct->GetPathName());
	OutJson->SetBoolField(TEXT("isStruct"), Struct->IsA<UScriptStruct>());
	OutJson->SetObjectField(TEXT("properties"), Properties);
	OutJson->SetNumberField(TEXT("total"), Matched.Num());
	OutJson->SetNumberField(TEXT("offset"), Offset);
	if (End < Matched.Num())
	{
		OutJson->SetBoolField(TEXT("truncated"), true);
		OutJson->SetStringField(TEXT("next"), FString::Printf(TEXT("还有 %d 个：offset=%d，或用 filter 按名字筛"), Matched.Num() - End, End));
	}
	return true;
}
}
