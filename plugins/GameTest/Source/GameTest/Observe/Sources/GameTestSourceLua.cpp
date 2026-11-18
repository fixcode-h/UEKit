// Copyright Epic Games, Inc. All Rights Reserved.

// 取值来源：@luaMod.<require 路径> 和 @luaG.<名字>（协议第 4.3 节）。
// 模块只看 package.loaded（没 require 的不主动加载：读值不该改变游戏状态）。
// 路径整段一次交给 UELink 读（深层的表也能取到），遇到 .Fn(参数) 时调函数，调完剩下的段交回通用走法。

#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Lua/GameTestLuaBridge.h"
#include "Observe/Sources/GameTestValueSource.h"

#include "Dom/JsonValue.h"

namespace
{
	TSharedPtr<FJsonValue> KeyOf(const FGameTestPathSegment& InSegment)
	{
		if (InSegment.Kind == FGameTestPathSegment::EKind::Index)
		{
			double Number = 0;
			return InSegment.Index.TryNumber(Number) && !InSegment.Index.IsString()
				? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNumber>(Number))
				: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(InSegment.Index.IsString() ? InSegment.Index.GetString() : InSegment.Index.ToDisplay()));
		}
		return MakeShared<FJsonValueString>(InSegment.Name);
	}

	/**
	 * 从 InStart 开始读到第一个调用为止（含调用）：读就是读，有调用就按 Module / 全局调。
	 * 返回值和吃掉的段数（相对 InStart）。
	 */
	FGameTestValue ReadOrCall(const FString& InModule, TArrayView<const FGameTestPathSegment> InRest, int32 InStart, int32& OutConsumed,
		FGameTestFlowContext& InContext)
	{
		TArray<TSharedPtr<FJsonValue>> Keys;
		int32 Index = InStart;
		for (; Index < InRest.Num() && InRest[Index].Kind != FGameTestPathSegment::EKind::Call; ++Index)
		{
			Keys.Add(KeyOf(InRest[Index]));
		}
		FString Error;
		if (Index >= InRest.Num())
		{
			OutConsumed = Index;
			FGameTestLuaValue Value;
			const bool bOk = InModule.IsEmpty() ? GameTestLuaBridge::Read(InContext.World, nullptr, Keys, Value, Error)
												: GameTestLuaBridge::ReadModule(InContext.World, InModule, Keys, Value, Error);
			if (!bOk)
			{
				return FGameTestValue::Unresolved(Error);
			}
			return Value.IsNil() ? FGameTestValue::Unresolved(TEXT("是 nil")) : Value.ToValue();
		}

		// 调用：函数路径 = 前面的键 + 这个调用的名字
		const FGameTestPathSegment& CallSegment = InRest[Index];
		OutConsumed = Index + 1;
		if (InContext.IsReadOnly() && !InContext.Options.bAllowLuaInCond)
		{
			return FGameTestValue::Unresolved(FString::Printf(TEXT("条件、表达式里不能调 Lua 函数 %s（_settings.json 的 allowLuaInCond 打开后才行）"),
				*CallSegment.Name), TEXT("expr_not_readonly"));
		}
		if (!InContext.Options.bAllowLuaInvoke)
		{
			return FGameTestValue::Unresolved(TEXT("_settings.json 关掉了 allowLuaInvoke，不能调 Lua 函数"), TEXT("lua_disabled"));
		}
		Keys.Add(MakeShared<FJsonValueString>(CallSegment.Name));
		TArray<TSharedPtr<FJsonValue>> Args;
		for (const FGameTestValue& Arg : CallSegment.Args)
		{
			Args.Add(Arg.ToJson());
		}
		TArray<FGameTestLuaValue> Results;
		if (!GameTestLuaBridge::Call(InContext.World, nullptr, InModule, Keys, Args, Results, Error))
		{
			return FGameTestValue::Unresolved(FString::Printf(TEXT("调 Lua 函数 %s 出错：%s"), *CallSegment.Name, *Error));
		}
		return Results.Num() > 0 ? Results[0].ToValue() : FGameTestValue::Null();
	}

	class FLuaModuleSource final : public IGameTestValueSource
	{
	public:
		virtual FName GetKey() const override { return TEXT("luaMod"); }
		virtual FString GetSummary() const override { return TEXT("Lua 模块表（require 过的；没加载不主动 require）"); }
		virtual FString GetSyntax() const override { return TEXT("@luaMod.<require 路径>.<字段>（如 @luaMod.Game.Quest.QuestMgr.curId）"); }
		virtual bool IsLua() const override { return true; }

		virtual FGameTestValue Resolve(const FString& InSelector, TArrayView<const FGameTestPathSegment> InRest, int32& OutConsumed,
			FGameTestFlowContext& InContext) const override
		{
			OutConsumed = 0;
			if (!GameTestLuaBridge::IsAvailable())
			{
				return FGameTestValue::Unresolved(TEXT("项目没有 UnLua，读不了 Lua 模块"), TEXT("lua_disabled"));
			}
			// require 路径本身带点：从最长的前缀往短里试，第一个已加载的就是模块（Game.Quest.QuestMgr 优先于 Game.Quest）
			int32 Leading = 0;
			while (Leading < InRest.Num() && InRest[Leading].Kind == FGameTestPathSegment::EKind::Member)
			{
				++Leading;
			}
			for (int32 Take = Leading; Take >= 1; --Take)
			{
				TArray<FString> Parts;
				for (int32 Index = 0; Index < Take; ++Index)
				{
					Parts.Add(InRest[Index].Name);
				}
				const FString Module = FString::Join(Parts, TEXT("."));
				bool bLoaded = false;
				FString Error;
				if (!GameTestLuaBridge::IsModuleLoaded(InContext.World, Module, bLoaded, Error))
				{
					return FGameTestValue::Unresolved(Error);
				}
				if (!bLoaded)
				{
					continue;
				}
				int32 Consumed = 0;
				const FGameTestValue Value = ReadOrCall(Module, InRest, Take, Consumed, InContext);
				OutConsumed = Consumed;
				return Value;
			}
			return FGameTestValue::Unresolved(TEXT("没有已加载的 Lua 模块对得上这个路径（模块还没 require？GameTest 读值不主动加载）"), TEXT("path_missing"));
		}
	};

	class FLuaGlobalSource final : public IGameTestValueSource
	{
	public:
		virtual FName GetKey() const override { return TEXT("luaG"); }
		virtual FString GetSummary() const override { return TEXT("Lua 全局变量"); }
		virtual FString GetSyntax() const override { return TEXT("@luaG.<名字>.<字段>"); }
		virtual bool IsLua() const override { return true; }

		virtual FGameTestValue Resolve(const FString& InSelector, TArrayView<const FGameTestPathSegment> InRest, int32& OutConsumed,
			FGameTestFlowContext& InContext) const override
		{
			OutConsumed = 0;
			if (!GameTestLuaBridge::IsAvailable())
			{
				return FGameTestValue::Unresolved(TEXT("项目没有 UnLua，读不了 Lua 全局变量"), TEXT("lua_disabled"));
			}
			if (InRest.Num() == 0)
			{
				return FGameTestValue::Unresolved(TEXT("@luaG 后面要写全局变量名"), TEXT("expr_syntax"));
			}
			return ReadOrCall(FString(), InRest, 0, OutConsumed, InContext);
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterSourceLua()
	{
		GameTestValueSources::Get().Register(MakeShared<FLuaModuleSource>());
		GameTestValueSources::Get().Register(MakeShared<FLuaGlobalSource>());
	}
}
