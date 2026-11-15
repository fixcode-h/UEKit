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
	return SendKeyTo(InKey, bInDown, nullptr, OutMessage);
}

bool GameTestInput::SendKeyTo(const FKey& InKey, bool bInDown, const TSharedPtr<SWidget>& InFocus, FString& OutMessage)
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
		// 键盘事件发给有焦点的控件：给了目标就交给目标（输入框里的回车、退格），否则交给游戏视口，
		// 编辑器里才不会落到别的面板上。
		if (InFocus.IsValid())
		{
			App.SetUserFocus(0, InFocus, EFocusCause::SetDirectly);
		}
		else
		{
			App.SetAllUserFocusToGameViewport();
		}

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

bool GameTestInput::MovePointerTo(const FVector2D& InScreenPosition, FString& OutMessage)
{
	if (!FSlateApplication::IsInitialized())
	{
		OutMessage = TEXT("Slate 未初始化，移不了光标（-nullrhi 下没有真实光标）");
		return false;
	}

	FSlateApplication& App = FSlateApplication::Get();
	const FSyntheticScope Scope;
	const FVector2D LastPosition = App.GetCursorPos();
	App.SetCursorPos(InScreenPosition);

	// 只调 SetCursorPos 要等下一帧 Slate 合成移动才更新悬停；这里直接发一次移动，按下时悬停已经对了。
	const FPointerEvent Move(
		FSlateApplicationBase::CursorPointerIndex, InScreenPosition, LastPosition, App.GetPressedMouseButtons(),
		EKeys::Invalid, 0.0f, App.GetModifierKeys());
	App.ProcessMouseMoveEvent(Move);

	OutMessage = FString::Printf(TEXT("光标移到屏幕 (%.0f, %.0f)"), InScreenPosition.X, InScreenPosition.Y);
	return true;
}

bool GameTestInput::SendWheel(float InDelta, FString& OutMessage)
{
	if (!FSlateApplication::IsInitialized())
	{
		OutMessage = TEXT("Slate 未初始化，发不了滚轮");
		return false;
	}

	FSlateApplication& App = FSlateApplication::Get();
	const FSyntheticScope Scope;
	const FVector2D Position = App.GetCursorPos();
	const FPointerEvent Wheel(
		FSlateApplicationBase::CursorPointerIndex, Position, Position, App.GetPressedMouseButtons(),
		EKeys::Invalid, InDelta, App.GetModifierKeys());
	App.ProcessMouseWheelOrGestureEvent(Wheel, nullptr);

	OutMessage = FString::Printf(TEXT("在 (%.0f, %.0f) 滚动 %.1f"), Position.X, Position.Y, InDelta);
	return true;
}

bool GameTestInput::SendCharacters(const FString& InText, FString& OutMessage)
{
	if (!FSlateApplication::IsInitialized())
	{
		OutMessage = TEXT("Slate 未初始化，发不了字符");
		return false;
	}

	FSlateApplication& App = FSlateApplication::Get();
	const FSyntheticScope Scope;
	for (const TCHAR Character : InText)
	{
		const FCharacterEvent Event(Character, App.GetModifierKeys(), /*InUserIndex=*/0, /*bInIsRepeat=*/false);
		App.ProcessKeyCharEvent(Event);
	}

	OutMessage = FString::Printf(TEXT("输入 %d 个字符"), InText.Len());
	return true;
}

bool GameTestInput::SendSelectAll(FString& OutMessage)
{
	if (!FSlateApplication::IsInitialized())
	{
		OutMessage = TEXT("Slate 未初始化，发不了按键");
		return false;
	}

	FSlateApplication& App = FSlateApplication::Get();
	const FSyntheticScope Scope;
	// 文本框按事件里的修饰键判断快捷键，所以只要事件带上 Ctrl，不用真去按下 Ctrl 键。
	const FModifierKeysState Control(
		/*bInIsLeftShiftDown=*/false, /*bInIsRightShiftDown=*/false,
		/*bInIsLeftControlDown=*/true, /*bInIsRightControlDown=*/false,
		/*bInIsLeftAltDown=*/false, /*bInIsRightAltDown=*/false,
		/*bInIsLeftCommandDown=*/false, /*bInIsRightCommandDown=*/false,
		/*bInAreCapsLocked=*/false);
	App.ProcessKeyDownEvent(FKeyEvent(EKeys::A, Control, /*InUserIndex=*/0, /*bInIsRepeat=*/false, /*InCharacterCode=*/0, /*InKeyCode=*/0));
	App.ProcessKeyUpEvent(FKeyEvent(EKeys::A, Control, /*InUserIndex=*/0, /*bInIsRepeat=*/false, /*InCharacterCode=*/0, /*InKeyCode=*/0));

	OutMessage = TEXT("全选");
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
