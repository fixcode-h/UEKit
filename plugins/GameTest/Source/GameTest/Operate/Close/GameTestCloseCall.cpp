// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/Close/GameTestCloser.h"

#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Observe/Sources/GameTestReflectCall.h"
#include "Observe/Sources/GameTestReflectValue.h"

#include "UObject/Package.h"

namespace GameTestCloseCallImpl
{
	/** 推荐时认的函数名（5.2）：类上有这类名字的无参可调函数就先调它。 */
	bool LooksLikeClose(const FString& InName)
	{
		for (const TCHAR* Word : { TEXT("Close"), TEXT("Hide"), TEXT("Back"), TEXT("Exit"), TEXT("Dismiss") })
		{
			if (InName.Contains(Word, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	/** 引擎自带的类（/Script/UMG、/Script/Engine…）上的函数不推荐：只推荐项目自己写的关闭函数。 */
	bool IsProjectFunction(const UFunction& InFunction)
	{
		const UClass* Owner = InFunction.GetOwnerClass();
		const UPackage* Package = Owner != nullptr ? Owner->GetPackage() : nullptr;
		if (Package == nullptr)
		{
			return false;
		}
		const FString Name = Package->GetName();
		for (const TCHAR* Engine : { TEXT("/Script/UMG"), TEXT("/Script/Engine"), TEXT("/Script/CoreUObject"), TEXT("/Script/CommonUI"), TEXT("/Script/Slate") })
		{
			if (Name == Engine)
			{
				return false;
			}
		}
		return true;
	}

	bool HasNoInput(const UFunction& InFunction)
	{
		for (TFieldIterator<FProperty> It(&InFunction); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Parm) && !It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm))
			{
				return false;
			}
		}
		return true;
	}

	class FCloser final : public IGameTestCloser
	{
	public:
		virtual FName GetKey() const override { return TEXT("call"); }
		virtual FString GetSummary() const override { return TEXT("调对象（或 part）上的反射函数，参数规则同 obj.call"); }
		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("function"), TEXT("args"), TEXT("part") };
			return Fields;
		}
		virtual int32 GetRecommendOrder() const override { return 10; }

		virtual void Validate(const FJsonObject& InHow, FGameTestDiagSink& InSink) const override
		{
			FString Function;
			if (!InHow.TryGetStringField(TEXT("function"), Function) || Function.IsEmpty())
			{
				InSink.Error(TEXT("arg_missing"), TEXT("by=call 要写 function（函数名）"));
			}
		}

		virtual bool Close(FGameTestCloseRun& InRun) const override
		{
			UObject* Target = InRun.PartOrSelf();
			if (Target == nullptr)
			{
				return false;
			}
			FString Function;
			InRun.How.TryGetStringField(TEXT("function"), Function);
			UFunction* Found = GameTestReflectValue::FindFunction(Target->GetClass(), Function);
			if (Found == nullptr)
			{
				InRun.Message = FString::Printf(TEXT("%s 上没有函数 %s（Lua 绑的函数写 by=lua）"), *Target->GetClass()->GetName(), *Function);
				return false;
			}
			const FGameTestValue Args = GameTestRefs::EvalJson(InRun.How.TryGetField(TEXT("args")), InRun.Flow);
			if (Args.IsUnresolved())
			{
				InRun.Message = FString::Printf(TEXT("args 取不到：%s"), *Args.GetString());
				return false;
			}
			GameTestReflectCall::FArgs CallArgs;
			CallArgs.Named = Args.Type == EGameTestValueType::Map ? &Args.GetMap() : nullptr;
			CallArgs.Positional = Args.Type == EGameTestValueType::Array ? &Args.GetArray() : nullptr;
			const FGameTestValue Result = GameTestReflectCall::Call(Target, Found, CallArgs, InRun.Flow);
			if (Result.IsUnresolved())
			{
				InRun.Message = Result.GetString();
				return false;
			}
			InRun.Message = FString::Printf(TEXT("调了 %s"), *Function);
			return true;
		}

		virtual TSharedPtr<FJsonObject> Recommend(UObject* InObject) const override
		{
			for (TFieldIterator<UFunction> It(InObject->GetClass()); It; ++It)
			{
				UFunction* Function = *It;
				FString Why;
				if (Function != nullptr && LooksLikeClose(Function->GetName()) && IsProjectFunction(*Function) && HasNoInput(*Function)
					&& GameTestReflectValue::IsCallableFunction(*Function, Why))
				{
					const TSharedRef<FJsonObject> How = MakeShared<FJsonObject>();
					How->SetStringField(TEXT("by"), TEXT("call"));
					How->SetStringField(TEXT("function"), Function->GetName());
					return How;
				}
			}
			return nullptr;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCloseCall()
	{
		GameTestClosers::Get().Register(MakeShared<GameTestCloseCallImpl::FCloser>());
	}
}
