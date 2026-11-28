// Copyright Epic Games, Inc. All Rights Reserved.

// Lua 代码节点 `lua` 和脚本函数节点 `luafn`（2.9.0）：Lua 写在测试脚本的节点里或面板的函数库里，不落成 .lua 文件、不进游戏项目。
// 代码由 GameTestLuaChunk 包一层外壳，走 UELink 的 lua.exec（和 GM 页、Lua GM 同一条路）；返回值存 saveAs、用 expect 判断（和 api 步骤同一套）。

#include "AgentPanelBridge.h"
#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestRunner.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Core/Script/GameTestValue.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameTestCompat.h"
#include "Operate/GameTestLuaChunk.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace GameTestHandlersLuaLocal
{
	/** 默认 2 秒：节点里的 Lua 应该马上返回，要等就接「等到」节点；最多 10 秒。 */
	constexpr int32 DefaultTimeoutMs = 2000;
	constexpr int32 MaxTimeoutMs = 10000;

	EGameTestStepStatus Fail(FGameTestStepContext& InContext, const TCHAR* InCode, const FString& InMessage)
	{
		InContext.Message = InMessage;
		InContext.Payload->SetStringField(TEXT("code"), InCode);
		return GameTestHandlers::StepFailed;
	}

	/** 外壳返回的是 JSON 文字（lua.exec 的 results 只给显示用的描述，长字符串会被截断）。 */
	bool ReadResult(const TSharedPtr<FJsonObject>& InOut, FGameTestValue& OutValue, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (!InOut.IsValid() || !InOut->TryGetArrayField(TEXT("results"), Results) || Results == nullptr || Results->Num() == 0)
		{
			OutValue = FGameTestValue::Null();
			return true;
		}
		const TSharedPtr<FJsonObject> First = (*Results)[0].IsValid() ? (*Results)[0]->AsObject() : nullptr;
		FString Text;
		if (!First.IsValid() || !First->TryGetStringField(TEXT("v"), Text))
		{
			OutValue = FGameTestValue::Null();
			return true;
		}
		if (First->HasField(TEXT("len")))
		{
			OutError = TEXT("返回值太大，被截断了：只返回需要检查的那几个值（例如 return #list, list[1].id）");
			return false;
		}
		TSharedPtr<FJsonValue> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(TEXT("[") + Text + TEXT("]"));
		TArray<TSharedPtr<FJsonValue>> Wrapped;
		if (!FJsonSerializer::Deserialize(Reader, Wrapped) || Wrapped.Num() != 1)
		{
			OutValue = FGameTestValue::FromString(Text);
			return true;
		}
		OutValue = FGameTestValue::FromJson(Wrapped[0]);
		return true;
	}

	EGameTestStepStatus Run(FGameTestStepContext& InContext, bool bFn)
	{
		const TCHAR* Action = bFn ? TEXT("luafn") : TEXT("lua");
		if (InContext.Flow == nullptr)
		{
			return Fail(InContext, TEXT("unresolved"), FString::Printf(TEXT("这一步没有脚本上下文（%s 只能在脚本里用）"), Action));
		}
		FGameTestFlowContext& Flow = *InContext.Flow;
		Flow.World = InContext.World ? InContext.World : Flow.World;
		if (!Flow.Options.bAllowLuaInvoke)
		{
			return Fail(InContext, TEXT("lua_disabled"), TEXT("运行配置关掉了步骤调 Lua（_settings.json 的 allowLuaInvoke），Lua 代码节点跑不了"));
		}
		if (Flow.World == nullptr)
		{
			return Fail(InContext, TEXT("engine_unavailable"), TEXT("没有在跑的游戏世界，Lua 代码跑不了：先开 PIE"));
		}
		const FJsonObject& Params = *InContext.Step->Params;
		const TSharedPtr<FJsonObject>* LibPtr = nullptr;
		const TSharedPtr<FJsonObject> Lib = Params.TryGetObjectField(TEXT("lib"), LibPtr) && LibPtr != nullptr ? *LibPtr : nullptr;

		FString Name;
		FString Code;
		if (bFn)
		{
			Params.TryGetStringField(TEXT("fn"), Name);
			if (Name.IsEmpty())
			{
				return Fail(InContext, TEXT("bad_type"), TEXT("luafn 要写 fn（函数库里的函数名）"));
			}
			FString Error;
			Code = GameTestLuaChunk::BuildFnCall(Name, Lib, Error);
			if (Code.IsEmpty())
			{
				return Fail(InContext, TEXT("luafn_missing"), Error);
			}
		}
		else
		{
			Params.TryGetStringField(TEXT("code"), Code);
			if (Code.TrimStartAndEnd().IsEmpty())
			{
				return Fail(InContext, TEXT("bad_type"), TEXT("Lua 代码节点没写代码"));
			}
			Name = InContext.Step->Id.IsEmpty() ? FString(TEXT("Lua 代码")) : FString::Printf(TEXT("Lua 代码 #%s"), *InContext.Step->Id);
		}

		// 参数在外层求值（$变量、= 表达式、对象引用都行），代码里直接当变量用
		TMap<FString, TSharedPtr<FJsonValue>> Args;
		const TSharedPtr<FJsonObject>* ArgsObject = nullptr;
		if (Params.TryGetObjectField(TEXT("args"), ArgsObject) && ArgsObject != nullptr && ArgsObject->IsValid())
		{
			for (const auto& Pair : (*ArgsObject)->Values)
			{
				const FString ArgName = GAMETEST_JSON_KEY(Pair.Key);
				const FGameTestValue Value = GameTestRefs::EvalJson(Pair.Value, Flow);
				if (Value.IsUnresolved())
				{
					return Fail(InContext, *Value.GetCode().ToString(), FString::Printf(TEXT("参数 %s 取不到值：%s"), *ArgName, *Value.GetString()));
				}
				Args.Add(ArgName, Value.ToJson());
			}
		}

		int32 TimeoutMs = DefaultTimeoutMs;
		double TimeoutNumber = 0;
		if (Params.TryGetNumberField(TEXT("timeoutMs"), TimeoutNumber) && TimeoutNumber > 0)
		{
			TimeoutMs = FMath::Clamp((int32)TimeoutNumber, 100, MaxTimeoutMs);
		}
		const FString Chunk = bFn ? FString::Printf(TEXT("=GameTest:库.%s"), *Name)
			: FString::Printf(TEXT("=GameTest:#%s"), InContext.Step->Id.IsEmpty() ? TEXT("lua") : *InContext.Step->Id);

		const TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("world"), FAgentPanelBridge::WorldId(Flow.World));
		Op->SetStringField(TEXT("code"), GameTestLuaChunk::BuildWrapper(Code, Chunk, Args, Lib));
		Op->SetNumberField(TEXT("timeoutMs"), TimeoutMs);
		Op->SetNumberField(TEXT("depth"), 1);
		TSharedPtr<FJsonObject> Out;
		FString Error;
		if (!FAgentPanelBridge::Get().CallOp(TEXT("lua.exec"), Op, Out, Error))
		{
			InContext.Payload->SetStringField(TEXT("luaError"), Error);
			return Fail(InContext, TEXT("lua_error"), FString::Printf(TEXT("%s 出错：%s"), *Name, *Error));
		}
		const TArray<TSharedPtr<FJsonValue>>* Output = nullptr;
		if (Out.IsValid() && Out->TryGetArrayField(TEXT("output"), Output) && Output != nullptr && Output->Num() > 0)
		{
			InContext.Payload->SetArrayField(TEXT("output"), *Output);
		}
		FGameTestValue Result;
		if (!ReadResult(Out, Result, Error))
		{
			return Fail(InContext, TEXT("lua_result_too_big"), FString::Printf(TEXT("%s：%s"), *Name, *Error));
		}
		return GameTestHandlers::FinishScriptResult(InContext, Name, Result);
	}
}

namespace GameTestHandlers
{
	EGameTestStepStatus HandleLua(FGameTestStepContext& InContext)
	{
		return GameTestHandlersLuaLocal::Run(InContext, false);
	}

	EGameTestStepStatus HandleLuaFn(FGameTestStepContext& InContext)
	{
		return GameTestHandlersLuaLocal::Run(InContext, true);
	}
}
