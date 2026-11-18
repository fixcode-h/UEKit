// Copyright Epic Games, Inc. All Rights Reserved.

// 成员访问：UObject 绑的 UnLua 实例表（协议第 4.3 节「Lua 表走 UnLua」）。优先级比反射低：同名时 C++ / 蓝图属性优先，
// 和 UnLua 自己的查找顺序一致。读写都经 UELink（GameTestLuaBridge），项目的 UnLua 是二次开发版，不在这里假设它的接口。

#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Lua/GameTestLuaBridge.h"
#include "Observe/Sources/GameTestValueSource.h"

#include "Dom/JsonValue.h"

namespace
{
	class FLuaMemberAccess final : public IGameTestMemberAccess
	{
	public:
		virtual FName GetKey() const override { return TEXT("lua"); }
		virtual int32 GetPriority() const override { return -10; }

		virtual bool Handles(const FGameTestValue& InBase) const override
		{
			return InBase.GetObject() != nullptr && GameTestLuaBridge::IsAvailable();
		}

		virtual bool ReadMember(const FGameTestValue& InBase, const FString& InName, FGameTestFlowContext& InContext,
			FGameTestValue& OutValue) const override
		{
			UObject* Object = InBase.GetObject();
			FGameTestLuaValue Lua;
			FString Error;
			const TArray<TSharedPtr<FJsonValue>> Keys = { MakeShared<FJsonValueString>(InName) };
			// 读不了（没绑 Lua、还没进过 Lua）和读到 nil 都算「我这里没有」：交回去报「没有这个成员」
			if (!GameTestLuaBridge::Read(InContext.World, Object, Keys, Lua, Error) || Lua.IsNil() || Lua.Type() == TEXT("function"))
			{
				return false;
			}
			OutValue = Lua.ToValue();
			return true;
		}

		virtual bool CallMethod(const FGameTestValue& InBase, const FString& InName, const TArray<FGameTestValue>& InArgs,
			FGameTestFlowContext& InContext, FGameTestValue& OutValue) const override
		{
			UObject* Object = InBase.GetObject();
			FGameTestLuaValue Probe;
			FString Error;
			const TArray<TSharedPtr<FJsonValue>> Keys = { MakeShared<FJsonValueString>(InName) };
			if (!GameTestLuaBridge::Read(InContext.World, Object, Keys, Probe, Error) || Probe.Type() != TEXT("function"))
			{
				return false;
			}
			// Lua 函数有没有副作用看不出来：条件、表达式里默认不许调（allowLuaInCond），步骤里受 allowLuaInvoke 管
			if (InContext.IsReadOnly() && !InContext.Options.bAllowLuaInCond)
			{
				OutValue = FGameTestValue::Unresolved(FString::Printf(TEXT("条件、表达式里不能调 Lua 函数 %s（_settings.json 的 allowLuaInCond 打开后才行）"), *InName),
					TEXT("expr_not_readonly"));
				return true;
			}
			if (!InContext.Options.bAllowLuaInvoke)
			{
				OutValue = FGameTestValue::Unresolved(TEXT("_settings.json 关掉了 allowLuaInvoke，不能调 Lua 函数"), TEXT("lua_disabled"));
				return true;
			}
			TArray<TSharedPtr<FJsonValue>> Args;
			for (const FGameTestValue& Arg : InArgs)
			{
				Args.Add(Arg.ToJson());
			}
			TArray<FGameTestLuaValue> Results;
			if (!GameTestLuaBridge::Call(InContext.World, Object, FString(), Keys, Args, Results, Error))
			{
				OutValue = FGameTestValue::Unresolved(FString::Printf(TEXT("调 Lua 函数 %s 出错：%s"), *InName, *Error));
				return true;
			}
			OutValue = Results.Num() > 0 ? Results[0].ToValue() : FGameTestValue::Null();
			return true;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterMemberLua()
	{
		GameTestMemberAccess::Register(MakeShared<FLuaMemberAccess>());
	}
}
