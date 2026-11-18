// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
struct FGameTestListItem;

/**
 * 列表数据项的匹配与描述（纯逻辑，可脱离引擎单测）。
 *
 * 抽出来的理由：这是"按 Id==1 找到那一条"的全部判据所在，而它的边界情形
 * （类型不同、大小写、属性名拼法）正是最容易写错、也最适合用单测钉住的地方。
 */
class GAMETEST_API GameTestItemMatch
{
public:
	/** 值比较：同类型按类型比；类型不同退化到文本比（`"1"` 与 `1` 等价，字符串忽略大小写）。 */
	static bool ValuesEqual(const FJsonValue& InLeft, const FJsonValue& InRight);

	/** 按名找属性，大小写不敏感（AI 写的常是记忆里的拼法）。 */
	static TSharedPtr<FJsonValue> FindProperty(const FJsonObject& InProperties, const FString& InPropertyName);

	/** 返回第一个 `InProperty` 等于 `InExpected` 的条目下标；没有命中返回 INDEX_NONE。 */
	static int32 FindIndexByProperty(const TArray<TSharedPtr<FGameTestListItem>>& InItems, const FString& InProperty, const FJsonValue& InExpected);

	/** 失败消息里的"实际值"清单——AI 一次就能改对，不用来回猜。 */
	static FString DescribeProperty(const TArray<TSharedPtr<FGameTestListItem>>& InItems, const FString& InPropertyName, int32 InLimit);
};
