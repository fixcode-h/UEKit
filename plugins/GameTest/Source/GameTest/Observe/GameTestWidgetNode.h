// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class SWidget;
class UWidget;

/** 列表条目的数据项（ListView 的 `GetListItems()` 一项）。 */
struct FGameTestListItem
{
	int32 Index = 0;
	FString ClassName;
	/** 反射 dump 出来的数据项属性（C# 数据类的 `[UProperty]` 也走同一条反射通道）。 */
	TSharedPtr<FJsonObject> Properties;
	bool bSelected = false;
	TWeakObjectPtr<UObject> Item;
};

/**
 * UI 控件树的一个节点。
 *
 * 它同时是**观察结果**与**选择器的搜索空间**——两者共用同一棵模型，
 * 所以"AI 在快照里看到的控件"与"选择器点到的控件"必然是同一个，不会各走一套遍历而漂移。
 *
 * 那一个 `TWeakObjectPtr<UWidget>` 是给适配层（操作/截图）用的句柄；纯匹配器只读
 * Name / ClassName / Path / Text / Children 这些不依赖引擎对象字段。
 */
struct GAMETEST_API FGameTestWidgetNode
{
	FString Name;
	FString ClassName;
	/** 相对屏根的路径，形如 `RootWidget/LayerCanvas/MenuStack/BtnStart`。 */
	FString Path;
	FString Text;
	/** ESlateVisibility 的名字（Visible / Hidden / Collapsed / HitTestInvisible …）。 */
	FString Visibility;
	bool bEnabled = true;
	bool bFocused = false;
	bool bHasGeometry = false;
	FVector2D Position = FVector2D::ZeroVector;
	FVector2D Size = FVector2D::ZeroVector;
	/**
	 * 有状态的控件的当前值（复选 checked / 滑条 value / 下拉 option / 展开 expanded / 菜单 open / 按键 key）。
	 * 没有它，"操作到底生效没有"就只能靠猜——AI 需要一个可读回的证据。
	 */
	TSharedPtr<FJsonObject> State;

	/** 声明在这个类自己身上的属性（含 C# 的 `[UProperty]`）；不含继承来的引擎属性。 */
	TSharedPtr<FJsonObject> Props;
	/** 嵌套的 UUserWidget 单独算一个屏，本节点只记引用不展开。 */
	bool bIsUserWidget = false;
	FString NestedScreenClass;

	TArray<TSharedPtr<FGameTestWidgetNode>> Children;
	TArray<TSharedPtr<FGameTestListItem>> Items;

	TWeakObjectPtr<UWidget> Widget;

	/**
	 * 纯 Slate 控件（选择器 `>>` 段命中的下拉项、菜单行）：没有 UMG 对象，`Widget` 为空，只有它。
	 * UMG 节点不填，要 Slate 控件时用 `Widget->GetCachedWidget()`。
	 */
	TWeakPtr<SWidget> Slate;

	/** 深度优先查找（含自身）。 */
	TSharedPtr<FGameTestWidgetNode> FindByPath(const FString& InPath) const;
};
