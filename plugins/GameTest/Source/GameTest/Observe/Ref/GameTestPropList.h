// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Observe/Ref/GameTestIdentSuggest.h"

class FGameTestFlowContext;
class UObject;

/**
 * gt.props 的属性树要列「看得见、调得动」的属性（docs/game-test-ref.md 7.3）：
 * 挑标识的 Collect 只要项目自己声明的字段（UMG 基类上百个属性会盖住真正的数据字段），
 * 但用户在属性树里要看到可见性、文字、颜色、勾选这些控件自带的属性，所以这里另外补上引擎类上编辑器 / 蓝图可见的反射属性。
 */
namespace GameTestPropList
{
	/** 补对象上 Collect 没列的反射属性（只要编辑器或蓝图看得见、读得到、能比较的），排在项目字段后面，常用的（可见性、文字、勾选…）在前。 */
	GAMETEST_API void AddReflected(UObject* InObject, FGameTestFlowContext& InContext, TArray<FGameTestIdentProp>& InOutProps);

	/** 按部件读出来的路径（`@.X`、`@:状态`、`@#X`、`@/Txt:text`）改成从外层对象写的路径（`@/部件.X`、`@/部件:状态`…）；InPart 是 `Header.TxtTitle` 写法。 */
	GAMETEST_API FString Rebase(const FString& InProp, const FString& InPart);
}
