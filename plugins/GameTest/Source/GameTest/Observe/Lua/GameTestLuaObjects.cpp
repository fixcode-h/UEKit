// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Lua/GameTestLuaObjects.h"

#include "Observe/Lua/GameTestLuaBridge.h"

#include "AgentPanelBridge.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "UObject/UObjectGlobals.h"

bool GameTestLuaObjects::List(UWorld* InWorld, const FString& InClass, TArray<FGameTestLuaObject>& OutObjects, FString& OutError)
{
	OutObjects.Reset();
	if (!GameTestLuaBridge::IsAvailable())
	{
		OutError = TEXT("项目没有 UnLua（UELink 没编进 Lua 支持），找不了绑了 Lua 的对象");
		return false;
	}
	const TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetNumberField(TEXT("limit"), MaxObjects);
	if (InWorld != nullptr)
	{
		// 多客户端时每个世界一个 Lua 环境：按世界选
		Args->SetStringField(TEXT("world"), FAgentPanelBridge::WorldId(InWorld));
	}
	if (!InClass.IsEmpty())
	{
		Args->SetStringField(TEXT("class"), InClass);
	}
	TSharedPtr<FJsonObject> Out;
	if (!FAgentPanelBridge::Get().CallOp(TEXT("lua.objects"), Args, Out, OutError))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Out.IsValid() || !Out->TryGetArrayField(TEXT("items"), Items) || Items == nullptr)
	{
		return true;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Items)
	{
		const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Item.IsValid())
		{
			continue;
		}
		bool bIsClass = false;
		FString State;
		// 类本身不能当实例用；已标记销毁、世界已关的对象拿来点、读都不可靠
		if ((Item->TryGetBoolField(TEXT("isClass"), bIsClass) && bIsClass) || (Item->TryGetStringField(TEXT("state"), State) && !State.IsEmpty()))
		{
			continue;
		}
		FGameTestLuaObject Object;
		if (!Item->TryGetStringField(TEXT("path"), Object.Path) || Object.Path.IsEmpty())
		{
			continue;
		}
		Item->TryGetStringField(TEXT("module"), Object.Module);
		OutObjects.Add(MoveTemp(Object));
	}
	return true;
}

bool GameTestLuaObjects::ModuleChain(UWorld* InWorld, const FString& InModule, TArray<FString>& OutChain, bool& bOutLoaded, FString& OutError)
{
	OutChain.Reset();
	bOutLoaded = false;
	if (!GameTestLuaBridge::IsModuleLoaded(InWorld, InModule, bOutLoaded, OutError))
	{
		return false;
	}
	if (!bOutLoaded)
	{
		return true;
	}
	OutChain.Add(InModule);
	const TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
	if (InWorld != nullptr)
	{
		Args->SetStringField(TEXT("world"), FAgentPanelBridge::WorldId(InWorld));
	}
	const TArray<TSharedPtr<FJsonValue>> Keys = {
		MakeShared<FJsonValueString>(TEXT("package")), MakeShared<FJsonValueString>(TEXT("loaded")), MakeShared<FJsonValueString>(InModule) };
	Args->SetArrayField(TEXT("keys"), Keys);
	// 只要元表链，不要字段
	Args->SetNumberField(TEXT("limit"), 1);
	TSharedPtr<FJsonObject> Out;
	if (!FAgentPanelBridge::Get().CallOp(TEXT("lua.inspect"), Args, Out, OutError))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Chain = nullptr;
	if (!Out.IsValid() || !Out->TryGetArrayField(TEXT("chain"), Chain) || Chain == nullptr)
	{
		return true;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Chain)
	{
		const TSharedPtr<FJsonObject> Layer = Value.IsValid() ? Value->AsObject() : nullptr;
		FString Kind;
		FString Name;
		// kind 是 ue 的层是 UnLua 给 UE 类建的元表，不是 Lua 类；按 UE 类匹配写普通的 class
		if (Layer.IsValid() && Layer->TryGetStringField(TEXT("kind"), Kind) && Kind == TEXT("lua") && Layer->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
		{
			OutChain.AddUnique(Name);
		}
	}
	return true;
}

UObject* GameTestLuaObjects::FindByPath(const FString& InPath)
{
	if (InPath.IsEmpty())
	{
		return nullptr;
	}
	UObject* Object = FindObject<UObject>(nullptr, *InPath);
	return IsValid(Object) ? Object : nullptr;
}
