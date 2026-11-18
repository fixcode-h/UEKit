// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestValue.h"

class FGameTestFlowContext;
class UFunction;
class UObject;

/**
 * 按反射调 UFUNCTION（协议第 6 节 invoke、表达式里的 `.Fn()`、项目 C++ 接口、`Object.Call`）。
 *
 * 参数打包和 ProcessEvent 复用 UELink 的 `AgentPanel::CallFunction`（`ue.call` 同一份代码），这里只做脚本这一侧：
 * 参数从 FGameTestValue 转过去、WorldContext 自动填、对象参数只认能重找的对象、返回值转回 FGameTestValue
 * （对象返回值还原成对象，能继续 `.成员`）。
 */
namespace GameTestReflectCall
{
	/** 参数：按名字（invoke 的 args 对象）或按顺序（表达式里的调用），二选一。 */
	struct GAMETEST_API FArgs
	{
		const TMap<FString, FGameTestValue>* Named = nullptr;
		const TArray<FGameTestValue>* Positional = nullptr;
	};

	/**
	 * 调用。`InTarget` 是对象（实例函数）或类默认对象（静态函数）。
	 * 返回值：没有输出参数时就是返回值本身；有输出参数时是 `{ReturnValue, 参数名…}`。
	 * 失败返回 Unresolved（fn_not_callable、arg_missing、arg_type、arg_object、expr_not_readonly）。
	 * 上下文在只读状态时只许调只读函数。往 `StepPayload` 写 `payload.invoke`。
	 */
	GAMETEST_API FGameTestValue Call(UObject* InTarget, UFunction* InFunction, const FArgs& InArgs, FGameTestFlowContext& InContext);

	/** `类名.函数`：找类上的静态函数（蓝图函数库、static UFUNCTION）。找不到返回 nullptr 并写原因。 */
	GAMETEST_API UFunction* FindStatic(const FString& InClassDotFunction, UObject*& OutTarget, FString& OutError);
}
