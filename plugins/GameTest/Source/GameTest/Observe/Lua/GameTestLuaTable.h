// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GameTestLuaTable.generated.h"

class UWorld;

/** UELink `lua.instances` 的一项：纯 Lua 实例（不绑 UObject 的 Lua 表）。 */
struct GAMETEST_API FGameTestLuaInstance
{
	/** `luaref:<编号>`：UELink 的 lua.get / lua.call 的 path 填它就以这张表为起点。 */
	FString Ref;
	/** 它的 Lua 类（模块名）。 */
	FString Module;
	/** 显示名（Lua 设置里的显示名字段）；没有时为空。 */
	FString Name;
	/** Lua 里第一次走到它的路径（只给人看）。 */
	FString Path;
};

/**
 * 纯 Lua 实例在 GameTest 里的替身（2.2.0）。引用、取值、调函数整套都按 UObject 写：给纯 Lua 表包一个瞬态对象，
 * 候选、`@#字段`、obj.call 的 Lua 函数照旧走，GameTestLuaBridge 遇到替身时把 path 换成它的 luaref，不另写一套。
 * 替身没有反射属性，`@.X` 读不到东西；不是控件也不是 Actor，屏上高亮、准星对它不起作用。
 */
UCLASS(Transient)
class GAMETEST_API UGameTestLuaTable : public UObject
{
	GENERATED_BODY()

public:
	FGameTestLuaInstance Info;
	TWeakObjectPtr<UWorld> World;

	/** 显示名：Lua 设置里的显示名字段，没有时用「模块名最后一段#luaref 编号」。 */
	FString Label() const;

	/** 同一个世界里同一张表拿到的总是同一个替身（对象路径不变，面板拿路径再来读也找得到）。 */
	static UGameTestLuaTable* Obtain(UWorld* InWorld, const FGameTestLuaInstance& InInfo);
};
