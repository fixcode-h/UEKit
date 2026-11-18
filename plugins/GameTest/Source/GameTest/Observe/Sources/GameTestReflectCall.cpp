// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Sources/GameTestReflectCall.h"

#include "Api/GameTestParamKind.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Sources/GameTestObjectRoots.h"
#include "Observe/Sources/GameTestReflectValue.h"

#include "AgentPanelBridgeUtil.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"

namespace
{
	/** 报告里的值截到多长（协议：值超长截到 500 字）。 */
	constexpr int32 PayloadChars = 500;

	/** UELink 返回的 JSON 转回值：对象属性的路径还原成对象，其余照 JSON。 */
	FGameTestValue FromCallJson(const FProperty* InProp, const TSharedPtr<FJsonValue>& InJson)
	{
		if (!InJson.IsValid())
		{
			return FGameTestValue::Null();
		}
		FString Path;
		if (CastField<FObjectPropertyBase>(InProp) != nullptr && CastField<FClassProperty>(InProp) == nullptr && InJson->TryGetString(Path))
		{
			UObject* Object = Path.IsEmpty() || Path == TEXT("None") ? nullptr : FindObject<UObject>(nullptr, *Path);
			return Object != nullptr ? FGameTestValue::FromObject(Object) : FGameTestValue::Null();
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(InProp))
		{
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (CastField<FObjectPropertyBase>(Array->Inner) != nullptr && InJson->TryGetArray(Items) && Items != nullptr)
			{
				TArray<FGameTestValue> Out;
				for (const TSharedPtr<FJsonValue>& Item : *Items)
				{
					Out.Add(FromCallJson(Array->Inner, Item));
				}
				return FGameTestValue::FromArray(MoveTemp(Out));
			}
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(InProp))
		{
			// 向量在 JSON 里是 {X,Y,Z}：转回向量，和取值来源读到的一样
			const TSharedPtr<FJsonObject>* Fields = nullptr;
			if (Struct->Struct != nullptr && Struct->Struct->GetFName() == NAME_Vector && InJson->TryGetObject(Fields) && Fields != nullptr)
			{
				return FGameTestValue::FromVector(FVector((*Fields)->GetNumberField(TEXT("X")), (*Fields)->GetNumberField(TEXT("Y")), (*Fields)->GetNumberField(TEXT("Z"))));
			}
		}
		return FGameTestValue::FromJson(InJson);
	}

	/** 一个参数值 → UELink CallFunction 认的 JSON；对象参数只认这次运行里能拿到的活对象。 */
	bool ArgToJson(const FGameTestParamInfo& InParam, const FGameTestValue& InValue, TSharedPtr<FJsonValue>& OutJson, FName& OutCode, FString& OutError)
	{
		if (InValue.IsUnresolved())
		{
			OutCode = TEXT("arg_type");
			OutError = FString::Printf(TEXT("参数 %s 取不到：%s"), *InParam.Name, *InValue.GetString());
			return false;
		}
		if (InParam.Kind == EGameTestParamKind::Object)
		{
			if (InValue.IsNull())
			{
				OutJson = MakeShared<FJsonValueNull>();
				return true;
			}
			UObject* Object = InValue.GetObject();
			if (Object == nullptr)
			{
				// 对象参数写成了文字（资产路径除外）：不能按名字乱找，要能重找的对象（协议第 6 节）
				FString Path = InValue.IsString() ? InValue.GetString() : FString();
				if (Path.StartsWith(TEXT("/")))
				{
					OutJson = MakeShared<FJsonValueString>(Path);
					return true;
				}
				OutCode = TEXT("arg_object");
				OutError = FString::Printf(TEXT("参数 %s 要对象（%s），给的是 %s：写 {\"$obj\": \"名字\"}、$变量、{\"target\": \"界面\"} 或 @来源"),
					*InParam.Name, *InParam.Type, *InValue.ToDisplay(60));
				return false;
			}
			OutJson = MakeShared<FJsonValueString>(Object->GetPathName());
			return true;
		}
		if (InParam.Kind == EGameTestParamKind::Unsupported)
		{
			OutCode = TEXT("fn_not_callable");
			OutError = FString::Printf(TEXT("参数 %s：%s"), *InParam.Name, *InParam.Why);
			return false;
		}
		OutJson = InValue.ToJson();
		return true;
	}

	void WritePayload(FGameTestFlowContext& InContext, const UObject* InTarget, const UFunction& InFunction, const TSharedPtr<FJsonObject>& InArgs, const FGameTestValue& InReturned, double InMs)
	{
		if (!InContext.StepPayload.IsValid())
		{
			return;
		}
		const TSharedPtr<FJsonObject> Invoke = MakeShared<FJsonObject>();
		Invoke->SetStringField(TEXT("object"), InTarget != nullptr && InTarget->HasAnyFlags(RF_ClassDefaultObject)
			? InTarget->GetClass()->GetName() : GameTestObjectRoots::DisplayName(InTarget));
		Invoke->SetStringField(TEXT("function"), InFunction.GetName());
		if (InArgs.IsValid())
		{
			Invoke->SetObjectField(TEXT("args"), InArgs);
		}
		Invoke->SetStringField(TEXT("returned"), InReturned.ToDisplay(PayloadChars));
		Invoke->SetNumberField(TEXT("ms"), InMs);
		InContext.StepPayload->SetObjectField(TEXT("invoke"), Invoke);
	}
}

namespace GameTestReflectCall
{
FGameTestValue Call(UObject* InTarget, UFunction* InFunction, const FArgs& InArgs, FGameTestFlowContext& InContext)
{
	if (InTarget == nullptr || InFunction == nullptr)
	{
		return FGameTestValue::Unresolved(TEXT("要调的对象或函数不在了"), TEXT("fn_missing"));
	}
	FString Why;
	if (!GameTestReflectValue::IsCallableFunction(*InFunction, Why))
	{
		return FGameTestValue::Unresolved(Why, TEXT("fn_not_callable"));
	}
	if (InContext.IsReadOnly() && !GameTestReflectValue::IsReadOnlyFunction(*InFunction))
	{
		return FGameTestValue::Unresolved(FString::Printf(TEXT("%s 不是只读函数（BlueprintPure 或 const），条件和表达式里不能调；改成 invoke 步骤"),
			*InFunction->GetName()), TEXT("expr_not_readonly"));
	}

	const TArray<FGameTestParamInfo> Params = GameTestParamKind::Describe(*InFunction);
	const TSharedPtr<FJsonObject> ArgsJson = MakeShared<FJsonObject>();
	int32 Position = 0;
	int32 Consumed = 0;
	for (const FGameTestParamInfo& Param : Params)
	{
		if (Param.Kind == EGameTestParamKind::WorldContext)
		{
			// 自动填当前世界：引擎的 WorldContext 参数认任何能 GetWorld 的对象，世界本身最直接
			if (InContext.World != nullptr)
			{
				ArgsJson->SetStringField(Param.Name, InContext.World->GetPathName());
			}
			continue;
		}
		const FGameTestValue* Given = nullptr;
		if (InArgs.Named != nullptr)
		{
			for (const TPair<FString, FGameTestValue>& Pair : *InArgs.Named)
			{
				if (Pair.Key.Equals(Param.Name, ESearchCase::IgnoreCase))
				{
					Given = &Pair.Value;
					++Consumed;
					break;
				}
			}
		}
		else if (InArgs.Positional != nullptr && !Param.bOut)
		{
			if (InArgs.Positional->IsValidIndex(Position))
			{
				Given = &(*InArgs.Positional)[Position];
				++Consumed;
			}
			++Position;
		}
		if (Given == nullptr)
		{
			// 输出参数不用给；普通参数没给时用默认值（零值），和蓝图里不连线一样
			continue;
		}
		TSharedPtr<FJsonValue> Json;
		FName Code;
		FString Error;
		if (!ArgToJson(Param, *Given, Json, Code, Error))
		{
			return FGameTestValue::Unresolved(Error, Code);
		}
		ArgsJson->SetField(Param.Name, Json);
	}
	const int32 GivenCount = InArgs.Named != nullptr ? InArgs.Named->Num() : InArgs.Positional != nullptr ? InArgs.Positional->Num() : 0;
	if (Consumed < GivenCount)
	{
		TArray<FString> Names;
		for (const FGameTestParamInfo& Param : Params)
		{
			if (Param.Kind != EGameTestParamKind::WorldContext && !Param.bOut)
			{
				Names.Add(Param.Name);
			}
		}
		return FGameTestValue::Unresolved(FString::Printf(TEXT("%s 多给了参数（它的参数：%s）"), *InFunction->GetName(),
			Names.Num() > 0 ? *FString::Join(Names, TEXT("、")) : TEXT("没有")), TEXT("unknown_field"));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	FString Error;
	if (!AgentPanel::CallFunction(InTarget, InFunction, MakeShared<FJsonValueObject>(ArgsJson), Out, Error))
	{
		return FGameTestValue::Unresolved(FString::Printf(TEXT("调 %s 失败：%s"), *InFunction->GetName(), *Error), TEXT("arg_type"));
	}

	// 返回值：只有返回值时就是它；有输出参数时拼成 {ReturnValue, 参数名…}
	FProperty* RetProp = InFunction->GetReturnProperty();
	const FGameTestValue Ret = RetProp != nullptr ? FromCallJson(RetProp, Out->TryGetField(TEXT("ret"))) : FGameTestValue::Null();
	TMap<FString, FGameTestValue> Outs;
	const TSharedPtr<FJsonObject>* OutParams = nullptr;
	if (Out->TryGetObjectField(TEXT("out"), OutParams) && OutParams != nullptr && (*OutParams).IsValid())
	{
		for (const FGameTestParamInfo& Param : Params)
		{
			if (Param.bOut && (*OutParams)->HasField(Param.Name))
			{
				Outs.Add(Param.Name, FromCallJson(InFunction->FindPropertyByName(FName(*Param.Name)), (*OutParams)->TryGetField(Param.Name)));
			}
		}
	}
	FGameTestValue Result = Ret;
	if (Outs.Num() > 0)
	{
		if (RetProp != nullptr)
		{
			Outs.Add(TEXT("ReturnValue"), Ret);
		}
		Result = FGameTestValue::FromMap(MoveTemp(Outs));
	}
	double Ms = 0;
	Out->TryGetNumberField(TEXT("ms"), Ms);
	WritePayload(InContext, InTarget, *InFunction, ArgsJson, Result, Ms);
	return Result;
}

UFunction* FindStatic(const FString& InClassDotFunction, UObject*& OutTarget, FString& OutError)
{
	FString ClassName;
	FString FunctionName;
	if (!InClassDotFunction.Split(TEXT("."), &ClassName, &FunctionName, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
		|| ClassName.IsEmpty() || FunctionName.IsEmpty())
	{
		OutError = FString::Printf(TEXT("静态函数写成 类名.函数（%s）"), *InClassDotFunction);
		return nullptr;
	}
	UClass* Class = GameTestObjectRoots::FindClass(ClassName);
	if (Class == nullptr)
	{
		OutError = FString::Printf(TEXT("没有类 %s（类名写错了，或这个类所在的模块没加载）"), *ClassName);
		return nullptr;
	}
	UFunction* Function = GameTestReflectValue::FindFunction(Class, FunctionName);
	if (Function == nullptr)
	{
		OutError = FString::Printf(TEXT("%s 上没有函数 %s"), *Class->GetName(), *FunctionName);
		return nullptr;
	}
	if (!Function->HasAnyFunctionFlags(FUNC_Static))
	{
		OutError = FString::Printf(TEXT("%s.%s 不是静态函数：要写 object（在哪个对象上调）"), *Class->GetName(), *FunctionName);
		return nullptr;
	}
	OutTarget = Class->GetDefaultObject();
	return Function;
}
}
