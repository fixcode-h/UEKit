// Copyright Epic Games, Inc. All Rights Reserved.

// gt.props 的 `class: "lua:模块"`（docs/game-test-ref.md 7.3）：Lua 类没有反射，读模块表本身（UELink lua.inspect）：
// 自己的字段（当默认值看）和自己 + 继承来的函数。路径写成对象上的 Lua 字段 `@#字段`，挑标识、写判断时直接用。

#include "Integration/Ref/GameTestRpcRef.h"

#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Observe/Lua/GameTestLuaBridge.h"

#include "AgentPanelBridge.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"

namespace GameTestRpcPropsLuaImpl
{
	/** 一次最多列多少个字段 / 函数：模块表一般不大，回包还要在 12000 字以内。 */
	constexpr int32 MaxEntries = 200;

	/** Lua 值的类型 → 3.4 的 kind（和 gt.props 反射属性同一套名字）。 */
	FString KindOfLua(const FString& InType)
	{
		if (InType == TEXT("number"))
		{
			return TEXT("float");
		}
		if (InType == TEXT("string"))
		{
			return TEXT("string");
		}
		if (InType == TEXT("boolean"))
		{
			return TEXT("bool");
		}
		if (InType == TEXT("table"))
		{
			return TEXT("array");
		}
		return TEXT("none");
	}

	TSharedRef<FJsonObject> MakeGroup(const TCHAR* InId, const TCHAR* InTitle, const TArray<TSharedPtr<FJsonValue>>& InItems)
	{
		const TSharedRef<FJsonObject> Group = MakeShared<FJsonObject>();
		Group->SetStringField(TEXT("id"), InId);
		Group->SetStringField(TEXT("title"), InTitle);
		Group->SetArrayField(TEXT("items"), InItems);
		return Group;
	}
}

bool GameTestRpcRef::PropsLua(const FString& InModule, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
{
	namespace Impl = GameTestRpcPropsLuaImpl;
	if (InModule.IsEmpty())
	{
		OutError = TEXT("class 写了 lua: 但没写模块名（例：lua:UI.ShopPanel）");
		return false;
	}
	FGameTestFlowContext Flow;
	MakeContext(Flow);
	UWorld* World = GameTestWidgetProbe::WorldOf(Flow);
	bool bLoaded = false;
	if (!GameTestLuaBridge::IsModuleLoaded(World, InModule, bLoaded, OutError))
	{
		return false;
	}
	if (!bLoaded)
	{
		OutError = FString::Printf(TEXT("lua_class_missing：Lua 模块 %s 没有加载（package.loaded 里没有）：检查模块名，或等游戏 require 它之后再看"), *InModule);
		return false;
	}
	const TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
	if (World != nullptr)
	{
		Args->SetStringField(TEXT("world"), FAgentPanelBridge::WorldId(World));
	}
	const TArray<TSharedPtr<FJsonValue>> Keys = {
		MakeShared<FJsonValueString>(TEXT("package")), MakeShared<FJsonValueString>(TEXT("loaded")), MakeShared<FJsonValueString>(InModule) };
	Args->SetArrayField(TEXT("keys"), Keys);
	Args->SetNumberField(TEXT("limit"), Impl::MaxEntries);
	TSharedPtr<FJsonObject> Out;
	if (!FAgentPanelBridge::Get().CallOp(TEXT("lua.inspect"), Args, Out, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> FieldItems;
	const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
	if (Out.IsValid() && Out->TryGetArrayField(TEXT("fields"), Fields) && Fields != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Fields)
		{
			const TSharedPtr<FJsonObject> Field = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Key;
			FString KeyType;
			// 只列字符串键（数字键是数组项，不是字段）
			if (!Field.IsValid() || !Field->TryGetStringField(TEXT("k"), Key) || Key.IsEmpty() || (Field->TryGetStringField(TEXT("kt"), KeyType) && KeyType != TEXT("string")))
			{
				continue;
			}
			FString Type;
			Field->TryGetStringField(TEXT("t"), Type);
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("prop"), TEXT("@#") + Key);
			Item->SetStringField(TEXT("name"), Key);
			Item->SetStringField(TEXT("src"), TEXT("lua"));
			Item->SetStringField(TEXT("type"), Type);
			Item->SetStringField(TEXT("kind"), Impl::KindOfLua(Type));
			if (const TSharedPtr<FJsonValue> Default = Field->TryGetField(TEXT("v")))
			{
				// 模块表上的值是类的默认值，实例上可能不同
				Item->SetField(TEXT("value"), Default);
			}
			FieldItems.Add(MakeShared<FJsonValueObject>(Item));
		}
	}

	TArray<TSharedPtr<FJsonValue>> FunctionItems;
	const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
	if (Out.IsValid() && Out->TryGetArrayField(TEXT("functions"), Functions) && Functions != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Functions)
		{
			const TSharedPtr<FJsonObject> Function = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Name;
			if (!Function.IsValid() || !Function->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				continue;
			}
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Name);
			Item->SetStringField(TEXT("src"), TEXT("lua"));
			const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
			if (Function->TryGetArrayField(TEXT("params"), Params) && Params != nullptr)
			{
				Item->SetArrayField(TEXT("params"), *Params);
			}
			double From = 0.0;
			if (Function->TryGetNumberField(TEXT("from"), From) && From > 0.0)
			{
				// 第几层父类继承来的（0 = 自己的，不写）
				Item->SetNumberField(TEXT("from"), From);
			}
			FunctionItems.Add(MakeShared<FJsonValueObject>(Item));
		}
	}

	TArray<TSharedPtr<FJsonValue>> Groups;
	Groups.Add(MakeShared<FJsonValueObject>(Impl::MakeGroup(TEXT("lua"), TEXT("Lua 字段"), FieldItems)));
	Groups.Add(MakeShared<FJsonValueObject>(Impl::MakeGroup(TEXT("functions"), TEXT("Lua 函数"), FunctionItems)));
	OutResult = MakeShared<FJsonObject>();
	OutResult->SetStringField(TEXT("class"), TEXT("lua:") + InModule);
	OutResult->SetStringField(TEXT("luaModule"), InModule);
	const TArray<TSharedPtr<FJsonValue>>* Chain = nullptr;
	if (Out.IsValid() && Out->TryGetArrayField(TEXT("chain"), Chain) && Chain != nullptr)
	{
		OutResult->SetArrayField(TEXT("chain"), *Chain);
	}
	OutResult->SetArrayField(TEXT("groups"), Groups);
	OutResult->SetNumberField(TEXT("total"), FieldItems.Num() + FunctionItems.Num());
	bool bMore = false;
	OutResult->SetBoolField(TEXT("hasMore"), Out.IsValid() && Out->TryGetBoolField(TEXT("more"), bMore) && bMore);
	OutResult->SetBoolField(TEXT("truncated"), false);
	return true;
}
