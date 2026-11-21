// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestClassFilter.h"

#include "Observe/Lua/GameTestLuaObjects.h"
#include "Observe/Sources/GameTestObjectRoots.h"

#include "UObject/Object.h"

bool FGameTestClassFilter::ParseLua(const FString& InClass, FString& OutModule)
{
	const FString Trimmed = InClass.TrimStartAndEnd();
	if (!Trimmed.StartsWith(TEXT("lua:"), ESearchCase::IgnoreCase))
	{
		return false;
	}
	OutModule = Trimmed.Mid(4).TrimStartAndEnd();
	return true;
}

bool FGameTestClassFilter::Build(const FString& InClass, UWorld* InWorld, FString& OutError, FName& OutCode)
{
	Class = InClass;
	LuaModule.Reset();
	LuaPaths.Reset();
	LuaTables.Reset();
	LuaRefs.Reset();
	FString Module;
	if (!ParseLua(InClass, Module))
	{
		return true;
	}
	if (Module.IsEmpty())
	{
		OutError = TEXT("class 写了 lua: 但没写模块名（例：lua:UI.ShopPanel）");
		OutCode = TEXT("lua_class_missing");
		return false;
	}
	LuaModule = Module;

	TArray<FString> TargetChain;
	bool bLoaded = false;
	if (!GameTestLuaObjects::ModuleChain(InWorld, Module, TargetChain, bLoaded, OutError))
	{
		OutCode = TEXT("lua_class_missing");
		return false;
	}
	// 纯 Lua 实例（2.2.0）：Lua 框架自己 new 的表，UnLua 的对象表里没有；Lua 设置配了类名字段时模块没加载也能按字段认
	FString TableError;
	if (!GameTestLuaObjects::ListTables(InWorld, Module, LuaTables, TableError))
	{
		LuaTables.Reset();
	}
	for (const FGameTestLuaInstance& Table : LuaTables)
	{
		LuaRefs.Add(Table.Ref);
	}
	if (!bLoaded && LuaTables.Num() > 0)
	{
		return true;
	}
	if (!bLoaded)
	{
		OutError = FString::Printf(TEXT("Lua 模块 %s 没有加载（package.loaded 里没有）：检查模块名，或等游戏 require 它之后再找"), *Module);
		OutCode = TEXT("lua_class_missing");
		return false;
	}

	TArray<FGameTestLuaObject> Objects;
	if (!GameTestLuaObjects::List(InWorld, FString(), Objects, OutError))
	{
		OutCode = TEXT("lua_class_missing");
		return false;
	}
	// 同一个模块的对象往往很多：每个模块的链只问一次
	TMap<FString, bool> ModuleHits;
	ModuleHits.Add(Module, true);
	for (const FGameTestLuaObject& Object : Objects)
	{
		if (Object.Module.IsEmpty())
		{
			continue;
		}
		const bool* Known = ModuleHits.Find(Object.Module);
		bool bHit = Known != nullptr && *Known;
		if (Known == nullptr)
		{
			TArray<FString> Chain;
			bool bChainLoaded = false;
			FString ChainError;
			bHit = GameTestLuaObjects::ModuleChain(InWorld, Object.Module, Chain, bChainLoaded, ChainError) && Chain.Contains(Module);
			ModuleHits.Add(Object.Module, bHit);
		}
		if (bHit)
		{
			LuaPaths.Add(Object.Path);
		}
	}
	return true;
}

bool FGameTestClassFilter::Matches(const UObject* InObject) const
{
	if (InObject == nullptr)
	{
		return false;
	}
	if (IsLua())
	{
		if (const UGameTestLuaTable* Table = Cast<UGameTestLuaTable>(InObject))
		{
			return LuaRefs.Contains(Table->Info.Ref);
		}
		return LuaPaths.Contains(InObject->GetPathName());
	}
	return GameTestObjectRoots::ClassMatches(InObject->GetClass(), Class);
}
