// Copyright Epic Games, Inc. All Rights Reserved.

#include "Api/GameTestParamKind.h"

#include "AgentPanelBridgeUtil.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/FieldPathProperty.h"
#include "UObject/UnrealType.h"

namespace
{
	/** 结构体嵌套多深还往下查：项目结构体互相包含时不至于无限递归。 */
	constexpr int32 ParamMaxStructDepth = 6;

	EGameTestParamKind ClassifyAt(const FProperty* InProp, int32 InDepth, FString& OutWhy)
	{
		if (InProp == nullptr)
		{
			OutWhy = TEXT("参数不存在");
			return EGameTestParamKind::Unsupported;
		}

		// 委托、接口、字段路径：脚本里没有能表示它们的值。
		if (CastField<FDelegateProperty>(InProp) != nullptr || CastField<FMulticastDelegateProperty>(InProp) != nullptr)
		{
			OutWhy = FString::Printf(TEXT("%s 是委托，脚本里给不了"), *InProp->GetName());
			return EGameTestParamKind::Unsupported;
		}
		if (CastField<FInterfaceProperty>(InProp) != nullptr)
		{
			OutWhy = FString::Printf(TEXT("%s 是接口（TScriptInterface），脚本里给不了；改成 UObject* 参数"), *InProp->GetName());
			return EGameTestParamKind::Unsupported;
		}
		if (CastField<FFieldPathProperty>(InProp) != nullptr)
		{
			OutWhy = FString::Printf(TEXT("%s 是字段路径，脚本里给不了"), *InProp->GetName());
			return EGameTestParamKind::Unsupported;
		}

		// 类、软引用：按路径字符串给（类也可以写类名）。要先于 FObjectProperty 判断：FClassProperty 是它的子类。
		if (CastField<FClassProperty>(InProp) != nullptr || CastField<FSoftObjectProperty>(InProp) != nullptr)
		{
			return EGameTestParamKind::Value;
		}
		if (CastField<FObjectPropertyBase>(InProp) != nullptr)
		{
			return EGameTestParamKind::Object;
		}

		if (const FArrayProperty* Array = CastField<FArrayProperty>(InProp))
		{
			const EGameTestParamKind Inner = ClassifyAt(Array->Inner, InDepth + 1, OutWhy);
			return Inner == EGameTestParamKind::Value ? Inner : EGameTestParamKind::Unsupported;
		}
		if (const FSetProperty* Set = CastField<FSetProperty>(InProp))
		{
			const EGameTestParamKind Inner = ClassifyAt(Set->ElementProp, InDepth + 1, OutWhy);
			return Inner == EGameTestParamKind::Value ? Inner : EGameTestParamKind::Unsupported;
		}
		if (const FMapProperty* Map = CastField<FMapProperty>(InProp))
		{
			if (ClassifyAt(Map->KeyProp, InDepth + 1, OutWhy) != EGameTestParamKind::Value
				|| ClassifyAt(Map->ValueProp, InDepth + 1, OutWhy) != EGameTestParamKind::Value)
			{
				return EGameTestParamKind::Unsupported;
			}
			return EGameTestParamKind::Value;
		}

		if (const FStructProperty* Struct = CastField<FStructProperty>(InProp))
		{
			if (Struct->Struct == nullptr)
			{
				OutWhy = FString::Printf(TEXT("%s 的结构体类型丢了"), *InProp->GetName());
				return EGameTestParamKind::Unsupported;
			}
			if (InDepth >= ParamMaxStructDepth)
			{
				OutWhy = FString::Printf(TEXT("%s 嵌套太深"), *Struct->Struct->GetName());
				return EGameTestParamKind::Unsupported;
			}
			// 结构体的字段都能构造才算能构造：里面藏着对象引用、委托的结构体，JSON 写不全，调用时只会得到半个值。
			for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
			{
				FString FieldWhy;
				if (ClassifyAt(*It, InDepth + 1, FieldWhy) != EGameTestParamKind::Value)
				{
					OutWhy = FString::Printf(TEXT("结构体 %s 的字段 %s 不能从脚本构造（%s）"),
						*Struct->Struct->GetName(), *It->GetName(), FieldWhy.IsEmpty() ? TEXT("对象引用") : *FieldWhy);
					return EGameTestParamKind::Unsupported;
				}
			}
			return EGameTestParamKind::Value;
		}

		// 剩下的是数字、布尔、字符串、名字、文本、枚举：都能从 JSON 值构造。
		return EGameTestParamKind::Value;
	}

	/** 这个参数是不是函数的 WorldContext。编辑器里读元数据；游戏进程没有元数据，按引擎约定的参数名认。 */
	bool IsWorldContextParam(const UFunction& InFunction, const FProperty& InProp)
	{
#if WITH_EDITORONLY_DATA
		const FString& Meta = InFunction.GetMetaData(TEXT("WorldContext"));
		if (!Meta.IsEmpty())
		{
			return InProp.GetName() == Meta;
		}
#endif
		return InProp.GetName() == TEXT("WorldContextObject") || InProp.GetName() == TEXT("WorldContext");
	}
}

EGameTestParamKind GameTestParamKind::Classify(const FProperty* InProp, FString& OutWhy)
{
	return ClassifyAt(InProp, 0, OutWhy);
}

TArray<FGameTestParamInfo> GameTestParamKind::Describe(const UFunction& InFunction)
{
	TArray<FGameTestParamInfo> Out;
	for (TFieldIterator<FProperty> It(&InFunction); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		FProperty* Prop = *It;
		if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			continue;
		}
		FGameTestParamInfo Info;
		Info.Name = Prop->GetName();
		Info.Type = AgentPanel::TypeOf(Prop);
		// 非 const 引用是输出参数：脚本不给，调完从缓冲里读回来
		Info.bOut = Prop->HasAnyPropertyFlags(CPF_OutParm) && !Prop->HasAnyPropertyFlags(CPF_ConstParm);
		if (CastField<FObjectPropertyBase>(Prop) != nullptr && IsWorldContextParam(InFunction, *Prop))
		{
			Info.Kind = EGameTestParamKind::WorldContext;
		}
		else
		{
			Info.Kind = ClassifyAt(Prop, 0, Info.Why);
		}
		if (Info.Kind == EGameTestParamKind::Object)
		{
			// 对象参数要的类：软引用、类引用也是 FObjectPropertyBase，PropertyClass 是它们指向的类
			const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Prop);
			const UClass* ParamClass = nullptr;
			if (ObjectProp != nullptr)
			{
				ParamClass = ObjectProp->PropertyClass;
			}
			if (ParamClass != nullptr)
			{
				Info.ObjectClass = ParamClass->GetName();
			}
		}
		Out.Add(MoveTemp(Info));
	}
	return Out;
}

bool GameTestParamKind::IsCallable(const UFunction& InFunction, FString& OutWhy)
{
	for (const FGameTestParamInfo& Param : Describe(InFunction))
	{
		if (!Param.bOut && Param.Kind == EGameTestParamKind::Unsupported)
		{
			OutWhy = FString::Printf(TEXT("参数 %s：%s"), *Param.Name, *Param.Why);
			return false;
		}
	}
	return true;
}

FString GameTestParamKind::ReturnType(const UFunction& InFunction)
{
	FProperty* Ret = InFunction.GetReturnProperty();
	return Ret != nullptr ? AgentPanel::TypeOf(Ret) : FString();
}

TArray<TSharedPtr<FJsonValue>> GameTestParamKind::ToJson(const TArray<FGameTestParamInfo>& InParams)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FGameTestParamInfo& Param : InParams)
	{
		const TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), Param.Name);
		O->SetStringField(TEXT("type"), Param.Type);
		O->SetBoolField(TEXT("constructible"), Param.Kind == EGameTestParamKind::Value);
		if (Param.Kind == EGameTestParamKind::Object)
		{
			O->SetBoolField(TEXT("object"), true);
			if (!Param.ObjectClass.IsEmpty())
			{
				O->SetStringField(TEXT("objectClass"), Param.ObjectClass);
			}
		}
		if (Param.Kind == EGameTestParamKind::WorldContext)
		{
			O->SetStringField(TEXT("auto"), TEXT("world"));
		}
		if (Param.bOut)
		{
			O->SetBoolField(TEXT("out"), true);
		}
		if (!Param.Why.IsEmpty())
		{
			O->SetStringField(TEXT("why"), Param.Why);
		}
		Out.Add(MakeShared<FJsonValueObject>(O));
	}
	return Out;
}
