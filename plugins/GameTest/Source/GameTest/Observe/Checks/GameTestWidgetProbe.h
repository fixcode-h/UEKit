// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FGameTestFlowContext;
class UClass;
class UUserWidget;
class UWorld;
struct FGameTestWidgetNode;

/** 一次「看看这个控件」的结果：条件里的控件判断、表达式的 text(sel) / visible(sel)… 共用。 */
struct GAMETEST_API FGameTestWidgetLook
{
	/** 选择器求值（`$item>BtnClaim`）后的文字。 */
	FString Target;
	bool bResolved = false;
	TSharedPtr<FGameTestWidgetNode> Node;
	/** 没解析到的原因（选择器写错、控件不在、多个命中）。 */
	FString Error;
};

/**
 * 控件的读取（只看不动）。
 *
 * 为什么单独一份：判断（GameTestCheckWidget）、表达式函数（text、visible、count…）、接口（Widget.*）、守护的 `onScreen`
 * 问的都是「这个控件此刻怎么样」，各自去调选择器、各自判在不在屏上，规则迟早对不上（skipIf 踩过：
 * 「在视口里」各处算法不一样，一边说在、一边说不在）。
 */
namespace GameTestWidgetProbe
{
	/** 解析选择器（先替换 `$变量`），不滚动、不等待。 */
	GAMETEST_API FGameTestWidgetLook Look(const FString& InTarget, FGameTestFlowContext& InContext);

	/** 真的在屏上：有 Slate 控件、整条父链可见、有尺寸。 */
	GAMETEST_API bool IsOnScreen(const FGameTestWidgetNode& InNode);

	/**
	 * 某个界面在不在屏上（`onScreen`）：按继承认——界面类或它的任一父类叫这个名字（`WBP_X`、`WBP_X_C`、`UMyBaseWidget`）
	 * 或是这个资产路径，就算。同一个界面开了几个实例时，有一个在屏上就算在。`OutCount` 是命中的实例数。
	 */
	GAMETEST_API bool IsScreenShown(const FString& InScreen, UWorld* InWorld, int32* OutCount = nullptr);

	/** 选择器的屏名部分（`WBP_Shop:Root/Btn` → `WBP_Shop`）；没写屏名返回空。 */
	GAMETEST_API FString ScreenOf(const FString& InTarget);

	/** 这个类（或它的父类）是不是叫这个名字 / 是不是这个路径。 */
	GAMETEST_API bool ClassMatches(const UClass* InClass, const FString& InName);

	/** 屏上所有界面（按继承 UUserWidget 认，挂在视口上的顶层界面），按视口 ZOrder 从上到下。 */
	GAMETEST_API void ListScreens(UWorld* InWorld, TArray<UUserWidget*>& OutScreens);

	/**
	 * 从文字里取第一个数（`number(sel)`、判断的 `number`）：认千分位逗号（「1,234」）、负号、小数，
	 * 和游戏里常见的单位（「1.2万」「3亿」「15K」「2.5M」「8w」）。「12/30」取 12。没有数返回 false。
	 */
	GAMETEST_API bool NumberInText(const FString& InText, double& OutNumber);

	/** 条目数：列表按数据条目数（含没滚到的），其余按子控件数。 */
	GAMETEST_API int32 CountOf(const FGameTestWidgetNode& InNode);
}
