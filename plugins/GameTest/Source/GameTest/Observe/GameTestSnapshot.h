// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UClass;
class UWidget;
class UWorld;
struct FGameTestWidgetNode;

/**
 * 把"实际挂在视口上的 UMG"采集成控件树模型。
 *
 * 采集面是**世界里存活的全部 UUserWidget**，逐个成为一棵"屏"。
 * 嵌套的 UUserWidget（子界面）不再展开，而是单独作为一棵屏出现——这样选择器与快照
 * 的看法一致，也让 `屏名:路径` 的路径永远是屏内相对路径。
 *
 * 存活判据用 `GetCachedWidget().IsValid()`（真的建出了 Slate 控件），而不是"对象存在"：
 * 前者才代表它此刻真的在画、真的能被操作。
 */
class GAMETEST_API GameTestSnapshot
{
public:
	/**
	 * `bInLight`：只要结构（名字、类、文字、状态、列表条目），不导出每个控件自己的属性。
	 * 选择器解析每帧都可能跑（回放的自动等待），属性导出是这里最贵的一块，只有快照要 props 时才需要。
	 */
	static void CaptureScreens(UWorld* InWorld, TArray<TSharedPtr<FGameTestWidgetNode>>& OutScreens, bool bInLight = false);

	/**
	 * 单个控件子树（含它自己）。
	 * @param InIsScreenRoot true 表示这是"屏根"（本身是 UUserWidget）：此时展开它自己的控件树；
	 *                       false 时遇到 UUserWidget 只记引用不展开（子界面自成一棵屏）。
	 */
	static TSharedPtr<FGameTestWidgetNode> CaptureSubtree(UWidget* InWidget, const FString& InPath, bool InIsScreenRoot, bool bInLight = false);

	/** 屏清单 + 控件树 → JSON。`props` 打开时额外带上"声明在自己类上"的属性。 */
	static TSharedPtr<FJsonObject> ScreensToJson(
		const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens,
		bool bInIncludeGeometry,
		bool bInIncludeProps,
		int32 InMaxNodes);

	/** 类名去掉蓝图的 `_C` 后缀（选择器、快照、可选模块都用这一份写法）。 */
	static FString FriendlyClassName(const UClass* InClass);

	/**
	 * 有状态控件的读取器：认得这个控件就往 State 里写字段并返回 true。
	 * 主模块认 UMG 自带的控件，CommonUI 这类可选模块在启动时登记自己的。
	 */
	using FStateReader = TFunction<bool(const UWidget& /*Widget*/, FJsonObject& /*OutState*/)>;
	static FDelegateHandle AddStateReader(FStateReader InReader);
	static void RemoveStateReader(FDelegateHandle InHandle);

	/** 依次问登记的读取器；有一个认就返回 true。 */
	static bool ReadExtraState(const UWidget& InWidget, FJsonObject& OutState);

	/** 反射 dump 一个对象（C# 数据类的 `[UProperty]` 走同一条通道）。 */
	static TSharedPtr<FJsonObject> DumpProperties(const UObject* InObject);
};
