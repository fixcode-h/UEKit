// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 主模块自带的扩展点实现（块、判断、来源、成员访问、表达式函数、接口）的登记入口。
 *
 * 为什么不用「每个 .cpp 里放一个静态对象自己登记」：单体打包（monolithic）链接时，没有被别处引用的目标文件
 * 可能整个被丢掉，静态对象就不会构造——编辑器里好好的，打出来的游戏里接口全没了，而且没有任何报错。
 * 所以每个实现文件提供一个 `Register…()` 函数，在 GameTestBuiltins.cpp 里按顺序显式调一遍。
 * 加一个实现 = 加一个文件 + 在 GameTestBuiltins.cpp 的列表里加一行，执行器和各主流程不用动。
 *
 * 第一次取任何一张注册表时调用（每张表的 `Get()` 里都调），重复调用无害。只在游戏线程上用。
 */
namespace GameTestBuiltins
{
	GAMETEST_API void EnsureRegistered();
}
