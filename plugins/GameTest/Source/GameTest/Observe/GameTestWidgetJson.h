// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FGameTestListItem;
struct FGameTestWidgetNode;

/** 控件树模型 → JSON（观察结果与截图图例共用）。 */
class GAMETEST_API GameTestWidgetJson
{
public:
	/** 节点 → JSON。节点数超过 InMaxNodes 后只记 `truncated: true`，避免返回体失控。 */
	static TSharedPtr<FJsonObject> ToJson(
		const FGameTestWidgetNode& InNode,
		bool bInIncludeGeometry,
		bool bInIncludeProps,
		int32 InMaxNodes,
		int32& InOutNodeCount);

	static TSharedPtr<FJsonObject> ItemToJson(const FGameTestListItem& InItem);
};
