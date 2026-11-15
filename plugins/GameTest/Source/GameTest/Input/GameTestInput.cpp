// Copyright Epic Games, Inc. All Rights Reserved.

#include "Input/GameTestInput.h"

#include "Core/GameTestWorld.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace
{
	/** 按下还没松开的键（run 收尾时全部松开）。 */
	TSet<FKey>& GetHeldKeys()
	{
		static TSet<FKey> Held;
		return Held;
	}

	bool bSendingSynthetic = false;

	/** 发合成事件期间置位，录制器看到就跳过；作用域结束自动复位，提前返回也不会漏。 */
	struct FSyntheticScope
	{
		FSyntheticScope() { bSendingSynthetic = true; }
		~FSyntheticScope() { bSendingSynthetic = false; }
	};
}

bool GameTestInput::MoveCursor(UWorld& InWorld, const FVector2D& InNormalizedPoint, FVector2D& OutScreenPosition, FString& OutMessage)
{
	if (!FSlateApplication::IsInitialized())
	{
		OutMessage = TEXT("Slate 未初始化，无法移动光标（-nullrhi 下没有真实光标）");
		return false;
	}

	UGameViewportClient* ViewportClient = InWorld.GetGameViewport();
	if (ViewportClient == nullptr)
	{
		OutMessage = TEXT("游戏世界还没有视口（PIE 起了吗）");
		return false;
	}

	const TSharedPtr<SViewport> ViewportWidget = ViewportClient->GetGameViewportWidget();
	if (!ViewportWidget.IsValid())
	{
		OutMessage = TEXT("拿不到游戏视口的 Slate 控件（视口还没建出来，或这一帧还没布局）");
		return false;
	}

	const FGeometry& ViewportGeometry = ViewportWidget->GetCachedGeometry();
	const FVector2D LocalSize = FVector2D(ViewportGeometry.GetLocalSize());
	if (LocalSize.X < 1.0 || LocalSize.Y < 1.0)
	{
		OutMessage = TEXT("视口尺寸为 0（还没布局，或窗口被最小化）");
		return false;
	}

	// 归一化 → 屏幕绝对坐标用引擎自己的换算（与高亮层、截图标注同一条链的同一份数）：
	// 它同时消掉窗口平移与 DPI 缩放，手写"乘尺寸再加窗口位置"会在缩放/DPI 上歪掉。
	const FVector2D Clamped(
		FMath::Clamp(InNormalizedPoint.X, 0.0, 1.0),
		FMath::Clamp(InNormalizedPoint.Y, 0.0, 1.0));
	const FVector2D AbsolutePosition = FVector2D(ViewportGeometry.GetAbsolutePositionAtCoordinates(FVector2f(Clamped)));

	FSlateApplication::Get().SetCursorPos(AbsolutePosition);
	OutScreenPosition = AbsolutePosition;

	OutMessage = FString::Printf(TEXT("光标已移到视口归一化 (%.2f, %.2f) → 屏幕 (%.0f, %.0f)；视口 %.0f×%.0f"),
		Clamped.X, Clamped.Y, AbsolutePosition.X, AbsolutePosition.Y, LocalSize.X, LocalSize.Y);
	return true;
}

bool GameTestInput::SendKey(const FKey& InKey, bool bInDown, FString& OutMessage)
{
	if (!FSlateApplication::IsInitialized())
	{
		OutMessage = TEXT("Slate 未初始化，发不了按键（-nullrhi 下没有输入系统）");
		return false;
	}

	if (!InKey.IsValid())
	{
		OutMessage = TEXT("不认识的键名（用 UE 的 FKey 名，例如 W、SpaceBar、LeftMouseButton、Gamepad_FaceButton_Bottom）");
		return false;
	}

	FSlateApplication& App = FSlateApplication::Get();
	const FSyntheticScope Scope;

	if (InKey.IsMouseButton())
	{
		const FVector2D Position = App.GetCursorPos();
		TSet<FKey> Pressed = App.GetPressedMouseButtons();
		if (bInDown)
		{
			Pressed.Add(InKey);
		}
		else
		{
			Pressed.Remove(InKey);
		}

		const FPointerEvent Event(
			FSlateApplicationBase::CursorPointerIndex, Position, Position, Pressed, InKey, 0.0f, App.GetModifierKeys());
		if (bInDown)
		{
			App.ProcessMouseButtonDownEvent(nullptr, Event);
		}
		else
		{
			App.ProcessMouseButtonUpEvent(Event);
		}
	}
	else
	{
		// 键盘事件发给有焦点的控件：先把焦点交给游戏视口，编辑器里才不会落到别的面板上。
		App.SetAllUserFocusToGameViewport();

		const FKeyEvent Event(InKey, App.GetModifierKeys(), /*InUserIndex=*/0, /*bInIsRepeat=*/false, /*InCharacterCode=*/0, /*InKeyCode=*/0);
		if (bInDown)
		{
			App.ProcessKeyDownEvent(Event);
		}
		else
		{
			App.ProcessKeyUpEvent(Event);
		}
	}

	if (bInDown)
	{
		GetHeldKeys().Add(InKey);
	}
	else
	{
		GetHeldKeys().Remove(InKey);
	}

	OutMessage = FString::Printf(TEXT("已%s %s"), bInDown ? TEXT("按下") : TEXT("松开"), *InKey.GetFName().ToString());
	return true;
}

void GameTestInput::ReleaseHeldKeys()
{
	const TArray<FKey> Held = GetHeldKeys().Array();
	for (const FKey& Key : Held)
	{
		FString Ignored;
		SendKey(Key, /*bInDown=*/false, Ignored);
	}
	GetHeldKeys().Reset();
}

bool GameTestInput::IsSendingSynthetic()
{
	return bSendingSynthetic;
}
