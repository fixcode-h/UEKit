// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class SWidget;
class SWindow;
class FWidgetPath;

/**
 * 编辑器界面上的一个控件怎么写（editor.click / editor.assert，1.10.0）。
 *
 * 不写 Slate 路径：编辑器的控件树很深、布局一换路径就变。写成「在哪个停靠页里、什么类型、上面写着什么字、第几个」，
 * 停靠页按标签认（外层到里层用 `>` 连，例如 `WBP_Shop>Details`），和人描述「资产编辑器工具栏上的『编译』」是同一个说法。
 */
struct FGameTestEditorWidgetSpec
{
	/** 停靠页标签链（外到里，`>` 连）；不在停靠页里时为空，改用 Window。 */
	FString Tab;
	/** 窗口标题里包含的字（不在停靠页里时才用，例如弹出的对话框）。 */
	FString Window;
	/** Slate 类型名（SButton、SCheckBox……）。 */
	FString Widget;
	/** 控件里的文字（含子控件的文字）；空的按钮只能靠第几个认。 */
	FString Text;
	/** 同样条件的第几个（从 0 数）。 */
	int32 Nth = 0;

	void WriteTo(FJsonObject& OutJson) const;
	static bool ReadFrom(const FJsonObject& InJson, FGameTestEditorWidgetSpec& OutSpec, FString& OutError);

	/** 给人看：「WBP_Shop · SButton「编译」」。 */
	FString Describe() const;
};

namespace GameTestEditorSlateFind
{
	/** 点到的命中路径 → 往上找真正可点的控件（按钮、复选框、下拉）；找不到就是最深的那个。 */
	TSharedPtr<SWidget> FindClickable(const FWidgetPath& InPath);

	/** 点在这些区域里的操作由编辑器事件录（视口、细节面板、大纲、内容浏览器），不按界面点击录。 */
	bool IsSemanticArea(const FWidgetPath& InPath);

	/** 一个控件 → 写法（Nth 按回放时的查找顺序算出来）。 */
	bool Describe(const TSharedRef<SWidget>& InWidget, const TSharedPtr<SWindow>& InWindow, FGameTestEditorWidgetSpec& OutSpec);

	/** 按写法找控件（不看 Nth，按查找顺序全部返回）。只找看得见的；游戏界面里的不算。 */
	void Find(const FGameTestEditorWidgetSpec& InSpec, TArray<TSharedRef<SWidget>>& OutWidgets);

	/** 控件所在的停靠页标签链（外到里，`>` 连）。 */
	FString TabChain(const TSharedRef<SWidget>& InWidget);

	/** 控件中心的屏幕坐标；还没排版（大小为 0）时返回 false。 */
	bool GetCenter(const SWidget& InWidget, FVector2D& OutCenter);
}
