// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UWidgetBlueprint;

/** gt.widgets 的查询参数。 */
struct FGameTestWidgetTreeQuery
{
	/** WBP 资产路径（`/Game/UI/WBP_Bag`）；和 Class 二选一。 */
	FString Asset;
	/** 类名（`WBP_Bag`、`WBP_Bag_C`）。 */
	FString Class;
	/** 只要名字或类名里含这段文字的节点（忽略大小写）。 */
	FString Filter;
	int32 Offset = 0;
	int32 Limit = 200;
};

/**
 * 读 WBP 资产的设计器控件树（协议第 10 节 `gt.widgets`）：每个控件的名字、类、能直接用的选择器、
 * 深度、是不是变量、默认文字、可见性；列表类容器给出条目类。不开游戏也能用：
 * Agent 不录制、从代码写回放时靠它知道界面里有哪些控件、选择器怎么写。
 */
class GameTestWidgetTreeReader
{
public:
	/** 按资产路径或类名找 WBP 资产；找不到时 OutError 写清楚，并给几个名字相近的。 */
	static UWidgetBlueprint* FindWidgetBlueprint(const FGameTestWidgetTreeQuery& InQuery, FString& OutError);

	/** 读控件树写进 Out（asset、class、parent、luaModule?、nodes、total、truncated?）。 */
	static bool Read(const FGameTestWidgetTreeQuery& InQuery, TSharedPtr<FJsonObject>& Out, FString& OutError);
};
