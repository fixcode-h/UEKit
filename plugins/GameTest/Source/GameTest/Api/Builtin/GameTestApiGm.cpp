// Copyright Epic Games, Inc. All Rights Reserved.

// 自带接口：执行 Agent 面板的 GM（2.3.0，协议第 8.1 节 Gm.*）。面板把「执行 GM」节点展开成这两个接口的 api 步骤：
// C++ GM（Exec 函数、UAgentPanelGMLibrary 的函数）走 UELink 的 gm.run，Lua GM 由面板按扫描结果拼好调用代码、走 lua.exec。
// 和 GM 页、ue_gm 执行的是同一条路，所以测试里跑的结果和在 GM 页点一下一样。

#include "Api/Builtin/GameTestApiArgs.h"
#include "Api/GameTestApi.h"
#include "AgentPanelBridge.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"

namespace GA = GameTestApiArgs;
using GameTestApiArgs::Opt;
using GameTestApiArgs::Req;

namespace
{
	using FCall = FGameTestApiCall;
	using FCtx = FGameTestFlowContext;
	using EState = EGameTestCallState;

	/** Lua GM 的时限（毫秒）：GM 应该马上返回，死循环由 UELink 的超时钩子中止。 */
	constexpr int32 LuaGmTimeoutMs = 10000;

	/** UELink 的回包（output、results）原样当返回值：saveAs 存下来，附带 GM 的 $ret 也是它。 */
	FGameTestValue ResultOf(const TSharedPtr<FJsonObject>& InOut)
	{
		return InOut.IsValid() ? FGameTestValue::FromJson(MakeShared<FJsonValueObject>(InOut)) : FGameTestValue::Null();
	}

	EState GmRun(FCall& C, FCtx& X)
	{
		const FString Name = GA::Text(C.Arg(0));
		if (Name.IsEmpty())
		{
			GA::ArgError(C, 0, TEXT("name"), TEXT("要写 GM 名（C++ 的函数名）"));
			return EState::Failed;
		}
		if (X.World == nullptr)
		{
			return C.Fail(TEXT("engine_unavailable"), TEXT("没有在跑的游戏世界，执行不了 GM：先开 PIE"));
		}
		TArray<TSharedPtr<FJsonValue>> Args;
		const FGameTestValue& Given = C.Arg(1);
		if (Given.Type == EGameTestValueType::Array)
		{
			for (const FGameTestValue& Item : Given.GetArray())
			{
				Args.Add(Item.ToJson());
			}
		}
		else if (!Given.IsNull())
		{
			Args.Add(Given.ToJson());
		}
		const TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
		Op->SetStringField(TEXT("world"), FAgentPanelBridge::WorldId(X.World));
		Op->SetStringField(TEXT("name"), Name);
		const FString Owner = GA::Text(C.Arg(2));
		if (!Owner.IsEmpty())
		{
			Op->SetStringField(TEXT("owner"), Owner);
		}
		Op->SetArrayField(TEXT("args"), Args);
		TSharedPtr<FJsonObject> Out;
		FString Error;
		if (!FAgentPanelBridge::Get().CallOp(TEXT("gm.run"), Op, Out, Error))
		{
			return C.Fail(TEXT("gm_failed"), FString::Printf(TEXT("GM %s 执行失败：%s"), *Name, *Error));
		}
		return C.Done(ResultOf(Out));
	}

	EState GmLua(FCall& C, FCtx& X)
	{
		if (!X.Options.bAllowLuaInvoke)
		{
			return C.Fail(TEXT("lua_disabled"), TEXT("运行配置关掉了步骤调 Lua（_settings.json 的 allowLuaInvoke），Lua GM 跑不了"));
		}
		const FString Code = GA::Text(C.Arg(0));
		if (Code.TrimStartAndEnd().IsEmpty())
		{
			GA::ArgError(C, 0, TEXT("code"), TEXT("要写 Lua 代码（面板按 Lua GM 拼好的调用）"));
			return EState::Failed;
		}
		const TSharedRef<FJsonObject> Op = MakeShared<FJsonObject>();
		if (X.World != nullptr)
		{
			// 多客户端时每个世界一个 Lua 环境：按这一步对着的世界选
			Op->SetStringField(TEXT("world"), FAgentPanelBridge::WorldId(X.World));
		}
		Op->SetStringField(TEXT("code"), Code);
		Op->SetNumberField(TEXT("timeoutMs"), LuaGmTimeoutMs);
		TSharedPtr<FJsonObject> Out;
		FString Error;
		if (!FAgentPanelBridge::Get().CallOp(TEXT("lua.exec"), Op, Out, Error))
		{
			const FString Name = GA::Text(C.Arg(1));
			return C.Fail(TEXT("gm_failed"), FString::Printf(TEXT("Lua GM %s 执行失败：%s"), Name.IsEmpty() ? *Code.Left(80) : *Name, *Error));
		}
		return C.Done(ResultOf(Out));
	}
}

namespace GameTestBuiltin
{
	void RegisterApiGm()
	{
		const TCHAR* Cat = TEXT("GM");
		GameTestApis::Add(TEXT("Gm.Run"), Cat, TEXT("执行一条 C++ GM（Exec 函数或 GM 库函数，和面板 GM 页同一条路）；返回 {output, …}"),
			{ Req(TEXT("name"), TEXT("string")), Opt(TEXT("args"), TEXT("any"), TEXT("参数数组，按顺序")), Opt(TEXT("owner"), TEXT("string"), TEXT("同名多处时指定类")) },
			TEXT("{output}"), false, &GmRun);
		GameTestApis::Add(TEXT("Gm.Lua"), Cat, TEXT("执行一条 Lua GM（面板按扫描结果拼好的调用代码）；返回 {results, output}。受 allowLuaInvoke"),
			{ Req(TEXT("code"), TEXT("string")), Opt(TEXT("name"), TEXT("string"), TEXT("GM 名，只用在报错里")) },
			TEXT("{results, output}"), false, &GmLua);
	}
}
