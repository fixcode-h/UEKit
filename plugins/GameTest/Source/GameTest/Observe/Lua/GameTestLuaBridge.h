// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestValue.h"

class FJsonObject;
class FJsonValue;
class UObject;
class UWorld;

/** 读到的一个 Lua 值：UELink `lua.get` 的描述（t、v、n、obj、class、module…）+ 是表时的第一层字段。 */
struct GAMETEST_API FGameTestLuaValue
{
	/** `{t: "number", v: 3}`、`{t: "table", n: 5}`、`{t: "nil"}`…，见 UELink lua.get 的 value。 */
	TSharedPtr<FJsonObject> Desc;
	/** 是表时：`[{k, kt, t, v…}]`（按键排序，最多一页）。 */
	TArray<TSharedPtr<FJsonValue>> Fields;
	/** 表里的项比列出来的多。 */
	bool bMore = false;

	FString Type() const;
	bool IsNil() const { return Type() == TEXT("nil") || Type().IsEmpty(); }

	/**
	 * 转成普通 JSON 值：数字、字符串、布尔照原样，nil 是 null，表转成对象（数组式的表转成数组），
	 * 表里再套的表只有一层描述（`{"$table": 项数}`），UObject 是对象路径。取值、条件比较用。
	 */
	TSharedPtr<FJsonValue> ToPlain() const;

	/** 转成脚本的值：绑了 UObject 的（UObject 代理、实例表）是对象，nil 是 Null，其余同 ToPlain。 */
	FGameTestValue ToValue() const;
};

/**
 * GameTest 读写 Lua 的唯一入口（协议 4.3 的 @luaMod / @luaG / @lua，接口库的 Lua.*）。
 *
 * 不自己碰 lua_State：经 UELink 的 lua.get / lua.call / lua.errors（`FAgentPanelBridge::CallOp`，UELink 2.12 起），
 * UELink 已经按项目的 UnLua 头选好了接口（项目的 UnLua 是二次开发版，不能按官方的假设），GameTest 不另写一份。
 * 没编进 Lua 支持（项目没有 UnLua）时这些操作不存在，IsAvailable 返回 false，调用方当作「读不到」。
 */
class GAMETEST_API GameTestLuaBridge
{
public:
	static bool IsAvailable();

	/**
	 * 读 Lua 值。Self 非空时从这个对象的实例表开始（`@lua.x`），否则从全局表开始（`@luaG.x`）。
	 * Keys 是一段段的键（字符串或数字）。路径中途是 nil、不是表时返回 false 并写原因。
	 */
	static bool Read(UWorld* InWorld, UObject* InSelf, const TArray<TSharedPtr<FJsonValue>>& InKeys, FGameTestLuaValue& OutValue, FString& OutError,
		bool bInFunctions = false);

	/** 读模块表（只看 package.loaded，不主动 require：读值不该触发加载）。没加载时返回 false，原因写「还没 require」。 */
	static bool ReadModule(UWorld* InWorld, const FString& InModule, const TArray<TSharedPtr<FJsonValue>>& InKeys, FGameTestLuaValue& OutValue, FString& OutError);

	/** 模块是不是已经 require 过。 */
	static bool IsModuleLoaded(UWorld* InWorld, const FString& InModule, bool& bOutLoaded, FString& OutError);

	/**
	 * 写 Lua 字段（UELink `lua.set`）：`InSelf` 不为空时从它绑的实例表开始，否则 `InKeys` 从全局开始
	 * （模块字段写 `["package","loaded",模块,…]`）。值是表时整个换掉。
	 */
	static bool Write(UWorld* InWorld, UObject* InSelf, const TArray<TSharedPtr<FJsonValue>>& InKeys, const TSharedPtr<FJsonValue>& InValue,
		FString& OutError);

	/**
	 * 调 Lua 函数。Self 非空时函数在实例表上找、以方法调（obj:Fn(...)）；Module 非空时在模块表上找（按点调 M.Fn(...)）；
	 * 都空时从全局表找。InFunctionKeys 是函数的路径（一般就一段函数名）。Args 按顺序（JSON 对象、数组会转成表）。
	 * 返回值最多 20 个，按顺序写进 OutResults。
	 */
	static bool Call(UWorld* InWorld, UObject* InSelf, const FString& InModule, const TArray<TSharedPtr<FJsonValue>>& InFunctionKeys,
		const TArray<TSharedPtr<FJsonValue>>& InArgs, TArray<FGameTestLuaValue>& OutResults, FString& OutError);

	/** Lua 报错的当前序号（标记「从这之后」用）。 */
	static uint64 ErrorSeq();

	/** 序号大于 Since 的 Lua 报错（UELink lua.errors 的条目：message、where、stack、count…），新的在前。 */
	static bool Errors(uint64 InSince, int32 InLimit, TArray<TSharedPtr<FJsonValue>>& OutItems, FString& OutError);

	/** 把 `A.B[2].C` 这样的路径拆成 Lua 键（数字下标转成数字键）。 */
	static TArray<TSharedPtr<FJsonValue>> SplitPath(const FString& InPath);
};
