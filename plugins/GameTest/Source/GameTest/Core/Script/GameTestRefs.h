// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestValue.h"

class FGameTestDiagSink;
class FGameTestFlowContext;
class FJsonValue;
struct FGameTestExprScope;

/**
 * 步骤里「填值的 JSON」求值（协议第 1.1 节）：块、接口参数、invoke 参数、定位器 key、比较的期望值都走这一个入口。
 *
 * | 写法 | 结果 |
 * | --- | --- |
 * | `"$名字"`、`"$名字.a[0]"`（整个值） | 变量的值，保持原类型 |
 * | `"=表达式"` | 表达式的值 |
 * | `"\\=..."` | 文字 `=...`（转义） |
 * | `"第 $i 页"`（嵌在字符串里） | 按文字替换，`\$` 是字面的 `$` |
 * | `{"$obj": "名字"}` | `objects` 里的对象（来源 `@obj`） |
 * | 数组、对象 | 逐项求 |
 * | 其他 | 原样 |
 */
namespace GameTestRefs
{
	GAMETEST_API FGameTestValue EvalJson(const TSharedPtr<FJsonValue>& InJson, FGameTestFlowContext& InContext);

	/** 只要文字（选择器、控制台指令、消息）：求值后转成文字；取不到返回 false 和原因。 */
	GAMETEST_API bool EvalText(const FString& InText, FGameTestFlowContext& InContext, FString& OutText, FString& OutError);

	/**
	 * 选择器里的 `$变量`：变量是控件（`loop each` 的 `$item`、`set` 存下的控件）时换成临时句柄 `@__v_<名字>`，
	 * 后面接的 `>子路径` 从这个控件自己的控件树开始走；不是控件时按文字替换。
	 */
	GAMETEST_API bool EvalSelector(const FString& InSelector, FGameTestFlowContext& InContext, FString& OutSelector, FString& OutError);

	/** 这段 JSON 里有没有引用（`$`、`=`、`{"$obj"}`）：没有的话可以直接当字面量，不用每次求值。 */
	GAMETEST_API bool HasRefs(const TSharedPtr<FJsonValue>& InJson);

	/** 静态检查：表达式能不能解析、用到的变量有没有定义。 */
	GAMETEST_API void Validate(const TSharedPtr<FJsonValue>& InJson, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink);

	/** 收集用到的 `$名字`。 */
	GAMETEST_API void CollectVars(const TSharedPtr<FJsonValue>& InJson, TSet<FString>& OutVars);
}
