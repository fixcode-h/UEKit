// Copyright Epic Games, Inc. All Rights Reserved.

#include "Input/GameTestInput.h"

#include "Core/GameTestWorld.h"
#include "Observe/GameTestSlate.h"

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

	/**
	 * 虚拟光标（1.2.3 起默认开）：回放的光标位置只记在这里、带进发给 Slate 的事件，不调 SetCursorPos，
	 * 系统光标留给人用。run 的点击方式是 pointer（真实鼠标）时关掉，照旧移真实光标。
	 */
	bool bVirtualCursor = true;
	TOptional<FVector2D> VirtualCursorPos;

	/** 鼠标事件该用的位置：虚拟光标摆过就用它，否则用真实光标。 */
	FVector2D CursorForEvents(const FSlateApplication& InApp)
	{
		return bVirtualCursor && VirtualCursorPos.IsSet() ? VirtualCursorPos.GetValue() : FVector2D(InApp.GetCursorPos());
	}

	/** 把光标摆到 InPosition：虚拟时只记下来，真实时移系统光标。返回移动前的位置（移动事件要用）。 */
	FVector2D PlaceCursor(FSlateApplication& InApp, const FVector2D& InPosition)
	{
		const FVector2D Last = CursorForEvents(InApp);
		if (bVirtualCursor)
		{
			VirtualCursorPos = InPosition;
		}
		else
		{
			InApp.SetCursorPos(InPosition);
		}
		return Last;
	}

	/** 发合成事件期间置位，录制器看到就跳过；作用域结束自动复位，提前返回也不会漏。 */
	struct FSyntheticScope
	{
		FSyntheticScope() { bSendingSynthetic = true; }
		~FSyntheticScope() { bSendingSynthetic = false; }
	};

	/**
	 * 发出去的事件带的修饰键 = 物理键盘上真按着的 + 回放按下还没松开的。
	 * `GetModifierKeys()` 只问操作系统，回放用 `key phase=press` 按住的 Alt / Ctrl / Shift 它看不见：
	 * 录制时按住 Alt 点的按钮，回放时控件拿到的点击事件 `IsAltDown()` 是 false，走的是另一条逻辑。
	 */
	FModifierKeysState MergedModifiers(const FSlateApplication& InApp)
	{
		const FModifierKeysState Real = InApp.GetModifierKeys();
		const TSet<FKey>& Held = GetHeldKeys();
		return FModifierKeysState(
			Real.IsLeftShiftDown() || Held.Contains(EKeys::LeftShift),
			Real.IsRightShiftDown() || Held.Contains(EKeys::RightShift),
			Real.IsLeftControlDown() || Held.Contains(EKeys::LeftControl),
			Real.IsRightControlDown() || Held.Contains(EKeys::RightControl),
			Real.IsLeftAltDown() || Held.Contains(EKeys::LeftAlt),
			Real.IsRightAltDown() || Held.Contains(EKeys::RightAlt),
			Real.IsLeftCommandDown() || Held.Contains(EKeys::LeftCommand),
			Real.IsRightCommandDown() || Held.Contains(EKeys::RightCommand),
			Real.AreCapsLocked());
	}
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

	{
		FSlateApplication& App = FSlateApplication::Get();
		const FSyntheticScope Scope;
		const FVector2D LastPosition = PlaceCursor(App, AbsolutePosition);
		if (bVirtualCursor)
		{
			// 虚拟光标没有系统光标移动带出来的事件：补一次移动，悬停、游戏里读鼠标事件的逻辑才看得到新位置
			const FPointerEvent Move(
				FSlateApplicationBase::CursorPointerIndex, AbsolutePosition, LastPosition, App.GetPressedMouseButtons(),
				EKeys::Invalid, 0.0f, MergedModifiers(App));
			App.ProcessMouseMoveEvent(Move);
		}
	}
	OutScreenPosition = AbsolutePosition;

	OutMessage = FString::Printf(TEXT("%s到视口归一化 (%.2f, %.2f) → 屏幕 (%.0f, %.0f)；视口 %.0f×%.0f"),
		bVirtualCursor ? TEXT("虚拟光标已移") : TEXT("光标已移"), Clamped.X, Clamped.Y, AbsolutePosition.X, AbsolutePosition.Y, LocalSize.X, LocalSize.Y);
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

	// 先记下按住的键再发事件：真实按下 Alt 时，Alt 自己的按下事件里修饰键就已经是 Alt。
	if (bInDown)
	{
		GetHeldKeys().Add(InKey);
	}
	else
	{
		GetHeldKeys().Remove(InKey);
	}
	const FModifierKeysState Modifiers = MergedModifiers(App);

	if (InKey.IsMouseButton())
	{
		const FVector2D Position = CursorForEvents(App);
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
			FSlateApplicationBase::CursorPointerIndex, Position, Position, Pressed, InKey, 0.0f, Modifiers);
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
		// 键盘事件发给有焦点的控件：给了目标就交给目标（输入框里的回车、退格）。没给目标时，焦点已经在游戏界面上
		// （游戏视口或它里面的 UMG）就不动它——按住 Alt 这类键时把焦点从打开的界面抢回视口，界面可能因失焦而关掉；
		// 焦点在编辑器别的面板上时才交给游戏视口，按键不会落到编辑器里。
		if (InFocus.IsValid())
		{
			App.SetUserFocus(0, InFocus, EFocusCause::SetDirectly);
		}
		else if (!GameTestSlate::IsInGameUi(App.GetUserFocusedWidget(0)))
		{
			App.SetAllUserFocusToGameViewport();
		}

		const FKeyEvent Event(InKey, Modifiers, /*InUserIndex=*/0, /*bInIsRepeat=*/false, /*InCharacterCode=*/0, /*InKeyCode=*/0);
		if (bInDown)
		{
			App.ProcessKeyDownEvent(Event);
		}
		else
		{
			App.ProcessKeyUpEvent(Event);
		}
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
	const FVector2D LastPosition = PlaceCursor(App, InScreenPosition);

	// 只调 SetCursorPos 要等下一帧 Slate 合成移动才更新悬停；这里直接发一次移动，按下时悬停已经对了。
	const FPointerEvent Move(
		FSlateApplicationBase::CursorPointerIndex, InScreenPosition, LastPosition, App.GetPressedMouseButtons(),
		EKeys::Invalid, 0.0f, MergedModifiers(App));
	App.ProcessMouseMoveEvent(Move);

	OutMessage = FString::Printf(TEXT("光标移到屏幕 (%.0f, %.0f)"), InScreenPosition.X, InScreenPosition.Y);
	return true;
}

bool GameTestInput::SendVirtualClick(const FVector2D& InScreenPosition, FString& OutMessage)
{
	if (!FSlateApplication::IsInitialized())
	{
		OutMessage = TEXT("Slate 未初始化，发不了虚拟鼠标事件");
		return false;
	}

	FSlateApplication& App = FSlateApplication::Get();
	const FSyntheticScope Scope;
	const FModifierKeysState Modifiers = MergedModifiers(App);
	const FKey Button = EKeys::LeftMouseButton;

	const FPointerEvent Move(
		FSlateApplicationBase::CursorPointerIndex, InScreenPosition, InScreenPosition, TSet<FKey>(), EKeys::Invalid, 0.0f, Modifiers);
	App.ProcessMouseMoveEvent(Move);

	TSet<FKey> Pressed;
	Pressed.Add(Button);
	const FPointerEvent Down(
		FSlateApplicationBase::CursorPointerIndex, InScreenPosition, InScreenPosition, Pressed, Button, 0.0f, Modifiers);
	App.ProcessMouseButtonDownEvent(nullptr, Down);

	const FPointerEvent Up(
		FSlateApplicationBase::CursorPointerIndex, InScreenPosition, InScreenPosition, TSet<FKey>(), Button, 0.0f, Modifiers);
	App.ProcessMouseButtonUpEvent(Up);

	OutMessage = FString::Printf(TEXT("虚拟鼠标点击屏幕 (%.0f, %.0f)"), InScreenPosition.X, InScreenPosition.Y);
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
	const FVector2D Position = CursorForEvents(App);
	const FPointerEvent Wheel(
		FSlateApplicationBase::CursorPointerIndex, Position, Position, App.GetPressedMouseButtons(),
		EKeys::Invalid, InDelta, MergedModifiers(App));
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
		const FCharacterEvent Event(Character, MergedModifiers(App), /*InUserIndex=*/0, /*bInIsRepeat=*/false);
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

void GameTestInput::SetVirtualCursor(bool bInVirtual)
{
	bVirtualCursor = bInVirtual;
	VirtualCursorPos.Reset();
}

bool GameTestInput::IsVirtualCursor()
{
	return bVirtualCursor;
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

FString GameTestInput::DescribeHeldKeys()
{
	TArray<FString> Names;
	for (const FKey& Key : GetHeldKeys())
	{
		Names.Add(Key.GetFName().ToString());
	}
	Names.Sort();
	return FString::Join(Names, TEXT("、"));
}
