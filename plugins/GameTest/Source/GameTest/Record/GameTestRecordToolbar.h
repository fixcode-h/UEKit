// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class SWidget;
class UGameViewportClient;

/**
 * 录制时游戏窗口右上角的一小条工具栏：录移动、记下位置、在这附近、断言、检查点、停止。
 *
 * **为什么在 UE 里放界面**（UEKit「界面在面板网页里做」的例外，用户 2026-11-17 同意）：录移动时人在游戏窗口里走，
 * 走到了要马上点「结束」，切到浏览器点会让游戏失焦、鼠标被游戏锁住时根本切不过去。
 * 所以只做这一条：只在录制时出现、不进 Shipping、不存任何设置（拖动位置只在内存里），回放时没有它。
 * 鼠标被游戏锁住（射击类视角）时用快捷键 Alt+数字，效果一样。
 *
 * 挂在游戏视口上（和 run 时的高亮框同一种做法），录制器的输入处理器看到点在它上面就不录。只在游戏线程上用。
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

	/** 更新内容；没挂上或游戏视口换了（PIE 重开）就挂到当前的游戏视口上。没有游戏视口时什么都不做。 */
	void Sync(const FState& InState);

	/** 从游戏视口上摘掉。 */
	void Hide();

	/** 屏幕坐标是不是落在工具栏上。 */
	bool Contains(const FVector2D& InScreenPosition) const;

private:
	TSharedRef<SWidget> Build();

	TSharedRef<FState> State = MakeShared<FState>();
	TSharedPtr<SWidget> Root;
	TSharedPtr<SWidget> Bar;
	TWeakObjectPtr<UGameViewportClient> Viewport;

	/** 拖动：相对右上角的偏移（屏幕像素）。 */
	FVector2D Offset = FVector2D::ZeroVector;
	FVector2D DragStart = FVector2D::ZeroVector;
	FVector2D OffsetAtDragStart = FVector2D::ZeroVector;
	bool bDragging = false;
};
