// Copyright Epic Games, Inc. All Rights Reserved.

// 自带接口：日志（协议第 8.1 节 Log.*）。读 GameTestLogTap 的环形缓冲（最近 1000 行），不读日志文件。

#include "Api/Builtin/GameTestApiArgs.h"
#include "Api/GameTestApi.h"
#include "Core/GameTestLogTap.h"
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

	/** 一行日志拆成类别、级别、内容（LogTap 写成「类别: 级别: 内容」，普通级别省略级别）。 */
	struct FLine
	{
		FString Category;
		FString Verbosity;
		FString Message;
	};

	FLine Split(const FString& InLine)
	{
		FLine Out;
		FString Rest;
		if (!InLine.Split(TEXT(": "), &Out.Category, &Rest))
		{
			Out.Message = InLine;
			return Out;
		}
		static const TCHAR* Levels[] = { TEXT("Fatal"), TEXT("Error"), TEXT("Warning"), TEXT("Verbose"), TEXT("VeryVerbose") };
		for (const TCHAR* Level : Levels)
		{
			if (Rest.StartsWith(FString(Level) + TEXT(": ")))
			{
				Out.Verbosity = Level;
				Out.Message = Rest.Mid(FCString::Strlen(Level) + 2);
				return Out;
			}
		}
		Out.Verbosity = TEXT("Log");
		Out.Message = Rest;
		return Out;
	}

	/** `since`：省略 = 本次运行开始；Log.Mark() 的返回 {log, lua}；或直接给序号。 */
	bool ReadSince(const FGameTestValue& InArg, const FCtx& X, uint64& OutLog, uint64& OutLua)
	{
		OutLog = X.RunLogSeq;
		OutLua = X.RunLuaErrorSeq;
		if (InArg.IsNull())
		{
			return true;
		}
		double Number = 0.0;
		FGameTestValue Field;
		if (InArg.TryGetField(TEXT("log"), Field) && Field.TryNumber(Number))
		{
			OutLog = static_cast<uint64>(Number);
			if (InArg.TryGetField(TEXT("lua"), Field) && Field.TryNumber(Number))
			{
				OutLua = static_cast<uint64>(Number);
			}
			return true;
		}
		if (InArg.TryNumber(Number))
		{
			OutLog = static_cast<uint64>(Number);
			return true;
		}
		return false;
	}

	/** 给定类别、起点范围内的行（Contains / Count 共用）。 */
	bool Lines(FCall& C, FCtx& X, const FString& InCategory, int32 InSinceArg, TArray<FLine>& OutLines)
	{
		if (X.LogTap == nullptr)
		{
			C.Fail(TEXT("engine_unavailable"), TEXT("日志没接上（不在运行中）"));
			return false;
		}
		uint64 SinceLog = 0, SinceLua = 0;
		if (!ReadSince(C.Arg(InSinceArg), X, SinceLog, SinceLua))
		{
			GA::ArgError(C, InSinceArg, TEXT("since"), TEXT("写 Log.Mark() 的返回或序号"));
			return false;
		}
		const FString& Category = InCategory;
		for (const FString& Raw : X.LogTap->Range(SinceLog, X.LogTap->GetSeq()))
		{
			FLine Line = Split(Raw);
			// 类别写 Quest 也认 LogQuest：项目里大多带 Log 前缀，脚本里常省
			if (Category.IsEmpty() || Line.Category.Equals(Category, ESearchCase::IgnoreCase)
				|| Line.Category.Equals(TEXT("Log") + Category, ESearchCase::IgnoreCase))
			{
				OutLines.Add(MoveTemp(Line));
			}
		}
		return true;
	}

	int32 CountMatches(const TArray<FLine>& InLines, const FString& InText)
	{
		int32 Count = 0;
		for (const FLine& Line : InLines)
		{
			Count += Line.Message.Contains(InText) ? 1 : 0;
		}
		return Count;
	}

	EState LogErrors(FCall& C, FCtx& X)
	{
		TArray<FLine> All;
		if (!Lines(C, X, FString(), 0, All))
		{
			return EState::Failed;
		}
		TArray<FGameTestValue> Out;
		for (const FLine& Line : All)
		{
			const bool bEnsure = Line.Message.Contains(TEXT("Ensure condition failed"));
			if (Line.Verbosity != TEXT("Error") && Line.Verbosity != TEXT("Fatal") && !bEnsure)
			{
				continue;
			}
			TMap<FString, FGameTestValue> Item;
			Item.Add(TEXT("category"), FGameTestValue::FromString(Line.Category));
			Item.Add(TEXT("verbosity"), FGameTestValue::FromString(bEnsure ? TEXT("Ensure") : *Line.Verbosity));
			Item.Add(TEXT("message"), FGameTestValue::FromString(Line.Message.Left(1000)));
			Out.Add(FGameTestValue::FromMap(MoveTemp(Item)));
		}
		// Lua 报错（被 pcall 接住的不一定进 Error 级别的日志）也算
		uint64 SinceLog = 0, SinceLua = 0;
		ReadSince(C.Arg(0), X, SinceLog, SinceLua);
		TArray<TSharedPtr<FJsonValue>> LuaItems;
		FString Ignore;
		if (GameTestLuaBridge::IsAvailable() && GameTestLuaBridge::Errors(SinceLua, 50, LuaItems, Ignore))
		{
			for (const TSharedPtr<FJsonValue>& LuaItem : LuaItems)
			{
				const FGameTestValue Value = FGameTestValue::FromJson(LuaItem);
				FGameTestValue Message;
				Value.TryGetField(TEXT("message"), Message);
				TMap<FString, FGameTestValue> Item;
				Item.Add(TEXT("category"), FGameTestValue::FromString(TEXT("Lua")));
				Item.Add(TEXT("verbosity"), FGameTestValue::FromString(TEXT("Error")));
				Item.Add(TEXT("message"), FGameTestValue::FromString(GA::Text(Message).Left(1000)));
				Out.Add(FGameTestValue::FromMap(MoveTemp(Item)));
			}
		}
		return C.Done(FGameTestValue::FromArray(MoveTemp(Out)));
	}
}

namespace GameTestBuiltin
{
	void RegisterApiLog()
	{
		const TCHAR* Cat = TEXT("日志");
		GameTestApis::Add(TEXT("Log.Mark"), Cat, TEXT("记下日志和 Lua 报错的当前位置，给后面的 since 用"), {}, TEXT("{log, lua}"), false,
			[](FCall& C, FCtx& X)
			{
				TMap<FString, FGameTestValue> Mark;
				Mark.Add(TEXT("log"), FGameTestValue::FromNumber(static_cast<double>(X.LogTap ? X.LogTap->GetSeq() : 0)));
				Mark.Add(TEXT("lua"), FGameTestValue::FromNumber(static_cast<double>(GameTestLuaBridge::ErrorSeq())));
				return C.Done(FGameTestValue::FromMap(MoveTemp(Mark)));
			});
		GameTestApis::Add(TEXT("Log.Contains"), Cat, TEXT("从 since 起有没有含这段文字的日志（只看最近 1000 行）"),
			{ Req(TEXT("text"), TEXT("string")), Opt(TEXT("category"), TEXT("string"), TEXT("LogQuest，Log 前缀可省")), Opt(TEXT("since"), TEXT("any")) },
			TEXT("bool"), true,
			[](FCall& C, FCtx& X)
			{
				TArray<FLine> Found;
				return Lines(C, X, GA::Text(C.Arg(1)), 2, Found) ? C.Done(FGameTestValue::FromBool(CountMatches(Found, GA::Text(C.Arg(0))) > 0)) : EState::Failed;
			});
		GameTestApis::Add(TEXT("Log.Count"), Cat, TEXT("从 since 起含这段文字的日志有几行"),
			{ Req(TEXT("text"), TEXT("string")), Opt(TEXT("category"), TEXT("string")), Opt(TEXT("since"), TEXT("any")) }, TEXT("int"), true,
			[](FCall& C, FCtx& X)
			{
				TArray<FLine> Found;
				return Lines(C, X, GA::Text(C.Arg(1)), 2, Found) ? C.Done(FGameTestValue::FromNumber(CountMatches(Found, GA::Text(C.Arg(0))))) : EState::Failed;
			});
		GameTestApis::Add(TEXT("Log.Errors"), Cat, TEXT("从 since 起的错误：Error / Fatal 级别、Ensure、Lua 报错"),
			{ Opt(TEXT("since"), TEXT("any")) }, TEXT("[{category, verbosity, message}]"), true, &LogErrors);
	}
}
