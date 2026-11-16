// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class SWidget;
class SWindow;
class UUserWidget;
class UWidget;

/**
 * Slate 控件和 UMG 控件之间的互查，录制、回放和 `gt.inspect` 共用这一份。
 *
 * **为什么需要它**：输入事件、命中测试、焦点拿到的都是 Slate 控件（`SButton`、`SObjectWidget`、`SImage`……），
 * 选择器要的是 UMG 控件和它所在的 WBP。引擎给每个由 UMG 建出来的 Slate 控件挂了一份 `FReflectionMetaData`
 * （Widget Reflector 显示「WBP_Main [BtnStart]」用的就是它），`SourceObject` 就是那个 `UWidget`。
 * 元数据只在非 Shipping 有，GameTest 本来就不进 Shipping；查不到时退回按 `GetCachedWidget()` 指针线性比对。
 *
 * 这里**不按控件类分支**：只管「这个 Slate 控件是哪个 UMG 控件建的」，不管它是不是按钮。
 * 只在游戏线程上用。
 */
class GAMETEST_API GameTestSlate
{
public:
	/** 这个 Slate 控件**自己**是不是某个 UMG 控件建的（不往上找）；不是返回 nullptr。 */
	static UWidget* FindSourceWidget(const SWidget& InWidget);

	/**
	 * 从这个 Slate 控件往上找最近的、由 UMG 建出来的控件。
	 * 走到游戏视口（`SViewport`）或窗口就停：视口以上已经不是界面，是游戏画面。
	 */
	static UWidget* FindOwningWidget(const TSharedPtr<SWidget>& InWidget);

	/** 当前游戏视口的 Slate 控件（PIE / 游戏进程都有）；没有时返回空。 */
	static TSharedPtr<SWidget> GetGameViewportWidget();

	/** 当前游戏窗口；没有时返回空。 */
	static TSharedPtr<SWindow> GetGameWindow();

	/** 这个窗口是不是游戏窗口，或游戏窗口弹出来的子窗口（下拉框、菜单常开成子窗口）。 */
	static bool IsGameWindowOrChild(const TSharedPtr<SWindow>& InWindow);

	/**
	 * 这个控件是不是游戏界面：在游戏视口里面（UMG 都挂在视口下），或在游戏窗口弹出的子窗口里（下拉框、菜单）。
	 * 不能只看窗口：PIE 跑在关卡编辑器的视口里时，「游戏窗口」就是整个编辑器主窗口，内容浏览器、细节面板都在里面。
	 * `InWindow` 是已知的所在窗口（命中路径里有），不给时现查。
	 */
	static bool IsInGameUi(const TSharedPtr<SWidget>& InWidget, const TSharedPtr<SWindow>& InWindow = nullptr);

	/** `InAncestor` 是不是 `InWidget` 自己或它的祖先（沿 `GetParentWidget()` 往上走）。 */
	static bool IsSelfOrAncestor(const SWidget* InAncestor, const TSharedPtr<SWidget>& InWidget);

	/** WBP 资源路径（`/Game/UI/WBP_Main`）：取控件类所在的包；原生类返回类路径。 */
	static FString GetAssetPath(const UWidget& InWidget);

	/** 子树里所有 `STextBlock` 的文字拼起来（最多 `InMaxChars` 字），选择器的 `text=` 和录制的 hint 用。 */
	static FString CollectText(const TSharedPtr<SWidget>& InWidget, int32 InMaxChars = 120);

	/**
	 * 「挂在 Slate 上的子界面」：一个 WBP 实例没有 UMG 父面板（`GetParent()` 为空）、也不是别的 WBP 控件树的根，
	 * 却显示在某个 UMG 控件的 Slate 子树里——DynamicEntryBox 的条目、自定义列表（不是 UListView）的条目、
	 * Lua 或 C++ 直接塞进 Slate 的子界面都是这样。沿 UMG 父链找不到它的父界面，快照和选择器要靠这里把它挂回去。
	 *
	 * 收集 `InContainer` 的 Slate 子树里这样的 WBP（遇到别的 UMG 控件就不往下走，它们归各自的控件），按 Slate 子控件顺序。
	 * 快照每帧都会调：只认元数据和 SObjectWidget，不走线性扫描；每个容器最多看几百个 Slate 控件。
	 * UListView 不在这里收：它的条目走 `列表[序号]`。
	 */
	static void CollectAttachedUserWidgets(const UWidget& InContainer, TArray<UUserWidget*>& OutWidgets);

	/** 这个 WBP 挂在哪个 UMG 控件的 Slate 上（见上）；有 UMG 父面板、直接加在视口上、或容器是 WBP / 列表时返回空。 */
	static UWidget* FindAttachContainer(const UUserWidget& InWidget);

	/** 控件在游戏视口里的位置（归一化，`OutMin`/`OutMax` 各 0–1）；没有视口或几何无效时返回 false。录制的 `hint.rect` 和回放消歧共用。 */
	static bool GetViewportRect(const SWidget& InWidget, FVector2D& OutMin, FVector2D& OutSize);

	/** 可读描述：`SButton → Button(BtnStart)@WBP_Main`，报错和 inspect 用。 */
	static FString Describe(const TSharedPtr<SWidget>& InWidget);
};
