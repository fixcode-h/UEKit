// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/GameTestTypes.h"

class FGameTestDiagSink;
class FJsonObject;
class UObject;
struct FGameTestExprScope;
struct FGameTestStepContext;

/**
 * 对象动作（docs/game-test-ref.md 5.2、5.3：obj.find / obj.wait / obj.set / obj.call / obj.close）共用的东西：
 * 写法检查（加载、gt.validate）和「等对象出现再做」。每个动作的实现各在自己的文件里（Operate/Obj/、Operate/Close/）。
 */
namespace GameTestObjActions
{
	/** 加载时查对象动作的字段（obj 引用、part、prop、how……）；不是对象动作时什么都不做。 */
	GAMETEST_API void Validate(const FJsonObject& InJson, FName InAction, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink);

	/** 找到对象（和 part）之后要做的事：返回 Succeeded / Failed，消息写进 InContext。不能返回 Pending。 */
	using FDo = TFunction<EGameTestStepStatus(FGameTestStepContext& /*Context*/, UObject* /*Object*/)>;

	/**
	 * 解析步骤的 `obj`（+ `part`）：找到就马上 InDo；还没出现（ref_not_found / part_missing）就每帧再找，
	 * 受步骤 timeoutMs（默认 UI 等待上限）；歧义、类写错这类等也没用的直接失败。
	 * `InWhat` 是等待时的进度说明（「调 Close」）。
	 */
	GAMETEST_API EGameTestStepStatus RunWhenFound(FGameTestStepContext& InContext, const FString& InWhat, FDo InDo);

	/** 没有脚本上下文（gt.step 也会给，这里只防万一）。 */
	EGameTestStepStatus NoFlow(FGameTestStepContext& InContext, const TCHAR* InAction);
}
