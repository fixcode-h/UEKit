// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class SWidget;
class SWindow;
class UGameViewportClient;

/**
 * 录制时的一小条工具栏：录移动、记下位置、在这附近、断言、检查点、停止。
 *
 * 它是编辑器的一个独立小窗口（不带系统标题栏的 SWindow，作为游戏所在窗口的子窗口），不是游戏界面：
 * 不挂在游戏视口里，所以不会被游戏自己的界面盖住、也不会混进游戏的控件树；可以拖出游戏视口、拖到屏幕任何地方。
 * 一开始停在游戏视口的中下方并跟着视口走，用户拖过一次之后就留在拖到的位置。
 * 窗口不抢焦点（ActivationPolicy Never、按钮不可聚焦）：点它不会让游戏失去键盘焦点，快捷键和录按键照常。
 * 拖动和点按钮的按下、松开都落在工具栏上，录制器照「点在工具栏上不录」跳过，不会录成步骤。
 *
 * **为什么在 UE 里放界面**（UEKit「界面在面板网页里做」的例外，用户 2026-11-17 同意）：录移动时人在游戏窗口里走，
 * 走到了要马上点「结束」，切到浏览器点会让游戏失焦、鼠标被游戏锁住时根本切不过去。
 * 所以只做这一条：只在录制时出现、不进 Shipping、不存任何设置（拖动位置只在内存里），回放时没有它。
 * 鼠标被游戏锁住（射击类视角）时用快捷键 Alt+数字，效果一样。只在游戏线程上用。
 */
class FGameTestRecordToolbar : public TSharedFromThis<FGameTestRecordToolbar>
{
public:
	/** 工具栏显示的内容：由录制器每帧填。 */
	struct FState
	{
		FString Status;
		bool bMoving = false;
		bool bHotkeys = true;
		bool bPickArmed = false;
		FString Notice;
	};

	/** 更新内容；没开窗口或游戏视口换了（PIE 重开）就在当前游戏视口旁边开一个。没有游戏视口时什么都不做。 */
	void Sync(const FState& InState);

	/** 关掉工具栏窗口。 */
	void Hide();

	/** 屏幕坐标是不是落在工具栏上。 */
	bool Contains(const FVector2D& InScreenPosition) const;

private:
	/** 在游戏所在窗口上开子窗口；找不到父窗口时返回 false。 */
	bool Open(UGameViewportClient* InViewport);

	/** 没拖过时把窗口摆到游戏视口中下方（视口移动、缩放后每帧跟上）。 */
	void FollowViewport();

	/** 把窗口大小改成正好包住工具栏（每帧，差一个像素以上才改）。 */
	void FitToContent();

	TSharedRef<SWidget> Build();

	TSharedRef<FState> State = MakeShared<FState>();
	TSharedPtr<SWindow> Window;
	TSharedPtr<SWidget> Bar;
	TWeakObjectPtr<UGameViewportClient> Viewport;

	/** 用户拖过就不再跟着视口摆位置。只存在内存里，停止录制就丢。 */
	bool bPlaced = false;
	/** 按下时鼠标相对窗口左上角的位置（屏幕像素），拖动中保持不变。 */
	FVector2D GrabOffset = FVector2D::ZeroVector;
	/** 按下时取的整个桌面范围（多显示器合起来），拖动时窗口不出这个范围，免得找不回来。 */
	FVector2D DesktopMin = FVector2D::ZeroVector;
	FVector2D DesktopMax = FVector2D::ZeroVector;
	bool bDragging = false;
};
