// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UObject;
class UWorld;

/**
 * 引用的 `class`（docs/game-test-ref.md 第 1 节）：UE 类名 / 资源路径按 UE 继承匹配；`lua:模块名` 按 Lua 继承链匹配
 * （对象绑的模块或它往上的某个 Lua 父类是这个模块）。
 *
 * `lua:` 的匹配要问 UELink（lua.objects + lua.inspect），一次解析建一个、只问一遍，别每个候选问一次。
 */
struct GAMETEST_API FGameTestClassFilter
{
	/** 原文。 */
	FString Class;
	/** `lua:` 后面的模块名；不是 Lua 类时为空。 */
	FString LuaModule;
	/** Lua 类时：继承链上有这个模块的对象路径。 */
	TSet<FString> LuaPaths;

	bool IsLua() const { return !LuaModule.IsEmpty(); }

	/** 写的是不是 `lua:模块名`（不问 UELink；写法检查用）。OutModule 是模块名。 */
	static bool ParseLua(const FString& InClass, FString& OutModule);

	/**
	 * 准备：UE 类只记下名字；`lua:` 时列出绑了 Lua 的对象、算出谁的继承链上有这个模块。
	 * 模块没加载（不认识）时返回 false，OutCode = lua_class_missing；没有 UnLua 也返回 false。
	 */
	bool Build(const FString& InClass, UWorld* InWorld, FString& OutError, FName& OutCode);

	bool Matches(const UObject* InObject) const;
};
