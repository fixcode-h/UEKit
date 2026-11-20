// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/Close/GameTestCloser.h"

#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Lua/GameTestLuaBridge.h"

namespace GameTestCloseLuaImpl
{
	class FCloser final : public IGameTestCloser
	{
	public:
		virtual FName GetKey() const override { return TEXT("lua"); }
		virtual FString GetSummary() const override { return TEXT("调对象绑的 Lua 表上的函数（运行配置 allowLuaInvoke）"); }
		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("function"), TEXT("args"), TEXT("part") };
			return Fields;
		}

		virtual void Validate(const FJsonObject& InHow, FGameTestDiagSink& InSink) const override
		{
			FString Function;
			if (!InHow.TryGetStringField(TEXT("function"), Function) || Function.IsEmpty())
			{
				InSink.Error(TEXT("arg_missing"), TEXT("by=lua 要写 function（Lua 表上的函数名）"));
			}
			const TSharedPtr<FJsonValue> Args = InHow.TryGetField(TEXT("args"));
			if (Args.IsValid() && Args->Type != EJson::Array && Args->Type != EJson::Null)
			{
				InSink.Error(TEXT("bad_type"), TEXT("by=lua 的 args 要写数组（Lua 函数没有参数名，按顺序传）"));
			}
		}

		virtual bool Close(FGameTestCloseRun& InRun) const override
		{
			if (!InRun.Flow.Options.bAllowLuaInvoke)
			{
				InRun.Message = TEXT("运行配置关掉了步骤调 Lua（_settings.json 的 allowLuaInvoke）");
				return false;
			}
			UObject* Target = InRun.PartOrSelf();
			if (Target == nullptr)
			{
				return false;
			}
			FString Function;
			InRun.How.TryGetStringField(TEXT("function"), Function);
			TArray<TSharedPtr<FJsonValue>> Args;
			const TArray<TSharedPtr<FJsonValue>>* Given = nullptr;
			if (InRun.How.TryGetArrayField(TEXT("args"), Given) && Given != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Arg : *Given)
				{
					const FGameTestValue Value = GameTestRefs::EvalJson(Arg, InRun.Flow);
					if (Value.IsUnresolved())
					{
						InRun.Message = FString::Printf(TEXT("args 取不到：%s"), *Value.GetString());
						return false;
					}
					Args.Add(Value.ToJson());
				}
			}
			TArray<FGameTestLuaValue> Results;
			FString Error;
			if (!GameTestLuaBridge::Call(InRun.Flow.World, Target, FString(), GameTestLuaBridge::SplitPath(Function), Args, Results, Error))
			{
				InRun.Message = Error;
				return false;
			}
			InRun.Message = FString::Printf(TEXT("调了 Lua 的 %s"), *Function);
			return true;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCloseLua()
	{
		GameTestClosers::Get().Register(MakeShared<GameTestCloseLuaImpl::FCloser>());
	}
}
