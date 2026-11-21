// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UObject;
class UWorld;

/** `lua.objects` 的一项（只留引用解析要的）。 */
struct GAMETEST_API FGameTestLuaObject
{
	/** UObject 路径（FindObject 用）。 */
	FString Path;
	/** 绑定的 Lua 模块名。 */
	FString Module;
};

/**
 * 绑了 UnLua 的对象和 Lua 类继承链（docs/game-test-ref.md 第 1 节 `in: "lua"`、`class: "lua:模块名"`）。
 *
 * 和 GameTestLuaBridge 一样只经 UELink 的操作（`lua.objects`、`lua.inspect`，FAgentPanelBridge::CallOp）：
 * UELink 已经按项目的 UnLua 选好了接口，GameTest 不碰 lua_State，也不 include 可选模块的头。
 */
namespace GameTestLuaObjects
{
	/** 一次最多列多少个对象（UELink lua.objects 的上限）。 */
	constexpr int32 MaxObjects = 1000;

	/**
	 * 列出这个世界里绑了 Lua 的活对象（跳过类本身、已标记销毁和世界已关的）。`InClass` 非空时按 UE 类过滤（含子类）。
	 * 没有 UnLua 时返回 false 并写原因。
	 */
	GAMETEST_API bool List(UWorld* InWorld, const FString& InClass, TArray<FGameTestLuaObject>& OutObjects, FString& OutError);

	/**
	 * Lua 模块的继承链：它自己 + 沿元表 `__index` 往上的每一层 Lua 模块名（`lua.inspect` 的 chain 里 kind 是 lua 的层）。
	 * 模块没 require 过时 bOutLoaded = false（调用方报 lua_class_missing）。
	 */
	GAMETEST_API bool ModuleChain(UWorld* InWorld, const FString& InModule, TArray<FString>& OutChain, bool& bOutLoaded, FString& OutError);

	/** 按路径找活对象（lua.objects 给的是完整对象路径）。找不到返回空。 */
	GAMETEST_API UObject* FindByPath(const FString& InPath);
}
