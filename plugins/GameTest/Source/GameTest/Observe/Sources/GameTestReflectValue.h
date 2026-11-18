// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestValue.h"

class FProperty;
class UFunction;
class UStruct;

/**
 * 反射值 ↔ FGameTestValue（协议第 4.3 节「UObject 走反射」）。
 *
 * 取值来源、反射成员访问器、invoke 的返回值都走这一份：结构体、数组、Set、Map、对象引用、软引用（不加载）、
 * 枚举（写成名字）、FGameplayTag（写成 `A.B.C`）在三处的样子一样，条件比较才可靠。
 */
namespace GameTestReflectValue
{
	/** 属性值 → 值。对象引用保留成对象（能继续 `.成员`）；软引用只给路径，不加载。 */
	GAMETEST_API FGameTestValue FromProperty(const FProperty* InProp, const void* InValue);

	/** 按名字找属性，忽略大小写（作者照蓝图里看到的写，大小写常不一致）。 */
	GAMETEST_API FProperty* FindProperty(const UStruct* InStruct, const FString& InName);

	/** 按名字找函数，忽略大小写；先找原名（快），再挨个比。 */
	GAMETEST_API UFunction* FindFunction(const UClass* InClass, const FString& InName);

	/** 只读函数：BlueprintPure 或 const（协议第 3 节，条件和表达式里只许调它）。 */
	GAMETEST_API bool IsReadOnlyFunction(const UFunction& InFunction);

	/** 脚本能调的函数：BlueprintCallable / BlueprintPure / Exec；不能调时 OutWhy 写原因（fn_not_callable）。 */
	GAMETEST_API bool IsCallableFunction(const UFunction& InFunction, FString& OutWhy);
}
