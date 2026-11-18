// Copyright Epic Games, Inc. All Rights Reserved.

// 自带接口：Lua（协议第 8.1 节 Lua.*）。经 UELink 的 lua.* 操作读写，GameTest 自己不依赖 UnLua。
// 模块只看 package.loaded，不主动 require：读值不该触发加载（加载可能有副作用）。

#include "Api/Builtin/GameTestApiArgs.h"
#include "Api/GameTestApi.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonValue.h"
#include "Observe/Lua/GameTestLuaBridge.h"

namespace GA = GameTestApiArgs;
using GameTestApiArgs::Opt;
using GameTestApiArgs::Req;

namespace
{
	using FCall = FGameTestApiCall;
	using FCtx = FGameTestFlowContext;
	using EState = EGameTestCallState;

	EState Finish(FCall& C, bool bOk, const FGameTestLuaValue& InValue, const FString& InError)
	{
		return bOk ? C.Done(InValue.ToValue()) : C.Fail(TEXT("path_missing"), InError);
	}

	EState Call(FCall& C, FCtx& X)
	{
		if (!X.Options.bAllowLuaInvoke)
		{
			return C.Fail(TEXT("lua_disabled"), TEXT("运行配置关掉了步骤调 Lua（_settings.json 的 allowLuaInvoke）"));
		}
		TArray<TSharedPtr<FJsonValue>> Args;
		const FGameTestValue& Given = C.Arg(2);
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
		TArray<FGameTestLuaValue> Results;
		FString Error;
		if (!GameTestLuaBridge::Call(X.World, nullptr, GA::Text(C.Arg(0)), GameTestLuaBridge::SplitPath(GA::Text(C.Arg(1))), Args, Results, Error))
		{
			return C.Fail(TEXT("fn_missing"), Error);
		}
		if (Results.Num() <= 1)
		{
			return C.Done(Results.Num() == 1 ? Results[0].ToValue() : FGameTestValue::Null());
		}
		// 多返回值（Lua 常见的 ok, err）：给数组，脚本里 $r[0]、$r[1] 取
		TArray<FGameTestValue> Items;
		for (const FGameTestLuaValue& Result : Results)
		{
			Items.Add(Result.ToValue());
		}
		return C.Done(FGameTestValue::FromArray(MoveTemp(Items)));
	}

	EState LuaErrors(FCall& C, FCtx& X)
	{
		double Since = static_cast<double>(X.RunLuaErrorSeq);
		const FGameTestValue& Given = C.Arg(0);
		FGameTestValue LuaSeq;
		// since 可以是 Log.Mark() 的返回（{log, lua}），也可以直接是序号
		if (Given.TryGetField(TEXT("lua"), LuaSeq))
		{
			LuaSeq.TryNumber(Since);
		}
		else if (!Given.IsNull() && !Given.TryNumber(Since))
		{
			GA::ArgError(C, 0, TEXT("since"), TEXT("写 Log.Mark() 的返回或序号"));
			return EState::Failed;
		}
		TArray<TSharedPtr<FJsonValue>> Items;
		FString Error;
		if (!GameTestLuaBridge::Errors(static_cast<uint64>(Since), 50, Items, Error))
		{
			return C.Fail(TEXT("engine_unavailable"), Error);
		}
		TArray<FGameTestValue> Out;
		for (const TSharedPtr<FJsonValue>& Item : Items)
		{
			Out.Add(FGameTestValue::FromJson(Item));
		}
		return C.Done(FGameTestValue::FromArray(MoveTemp(Out)));
	}
}

namespace GameTestBuiltin
{
	void RegisterApiLua()
	{
		const TCHAR* Cat = TEXT("Lua");
		GameTestApis::Add(TEXT("Lua.Get"), Cat, TEXT("读 Lua 模块里的值（模块按 require 路径；没加载的不主动加载）"),
			{ Req(TEXT("module"), TEXT("string"), TEXT("Game.Quest.QuestMgr")), Req(TEXT("path"), TEXT("string"), TEXT("Active.count")) },
			TEXT("any"), true,
			[](FCall& C, FCtx& X)
			{
				FGameTestLuaValue Value;
				FString Error;
				const bool bOk = GameTestLuaBridge::ReadModule(X.World, GA::Text(C.Arg(0)), GameTestLuaBridge::SplitPath(GA::Text(C.Arg(1))), Value, Error);
				return Finish(C, bOk, Value, Error);
			});
		GameTestApis::Add(TEXT("Lua.Global"), Cat, TEXT("读 Lua 全局变量（可以带路径：G_Config.Debug）"),
			{ Req(TEXT("name"), TEXT("string")) }, TEXT("any"), true,
			[](FCall& C, FCtx& X)
			{
				FGameTestLuaValue Value;
				FString Error;
				const bool bOk = GameTestLuaBridge::Read(X.World, nullptr, GameTestLuaBridge::SplitPath(GA::Text(C.Arg(0))), Value, Error);
				return Finish(C, bOk, Value, Error);
			});
		GameTestApis::Add(TEXT("Lua.Loaded"), Cat, TEXT("模块 require 过没有"), { Req(TEXT("module"), TEXT("string")) }, TEXT("bool"), true,
			[](FCall& C, FCtx& X)
			{
				bool bLoaded = false;
				FString Error;
				if (!GameTestLuaBridge::IsModuleLoaded(X.World, GA::Text(C.Arg(0)), bLoaded, Error))
				{
					return C.Fail(TEXT("engine_unavailable"), Error);
				}
				return C.Done(FGameTestValue::FromBool(bLoaded));
			});
		GameTestApis::Add(TEXT("Lua.Call"), Cat, TEXT("调 Lua 模块上的函数（M.Fn(...)，参数按顺序）；多个返回值给数组。受 allowLuaInvoke"),
			{ Req(TEXT("module"), TEXT("string")), Req(TEXT("fn"), TEXT("string")), Opt(TEXT("args"), TEXT("any"), TEXT("参数数组")) },
			TEXT("any"), false, &Call);
		GameTestApis::Add(TEXT("Lua.Errors"), Cat, TEXT("Lua 报错（含被 pcall 接住但打了日志的）；since 省略 = 本次运行开始"),
			{ Opt(TEXT("since"), TEXT("any"), TEXT("Log.Mark() 的返回")) }, TEXT("[{message, traceback, at}]"), true, &LuaErrors);
	}
}
