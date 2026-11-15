// Copyright Epic Games, Inc. All Rights Reserved.

#include "Input/GameTestHandlerWatch.h"

#include "Debugging/SlateDebugging.h"
#include "Input/Events.h"
#include "Input/Reply.h"
#include "Widgets/SWidget.h"

namespace GameTestHandlerWatchDetail
{
	GameTestHandlerWatch::FHandled LastByKind[static_cast<int32>(GameTestHandlerWatch::EKind::Num)];
	uint64 NextSerial = 1;
	bool bStarted = false;

#if WITH_SLATE_DEBUGGING
	FDelegateHandle InputHandle;

	bool ToKind(ESlateDebuggingInputEvent InEvent, GameTestHandlerWatch::EKind& OutKind)
	{
		switch (InEvent)
		{
		case ESlateDebuggingInputEvent::MouseButtonDown:
		// 快速连点的第二下是以双击事件送进来的（没有单独的按下），按「按下」算，否则连点会少录一下。
		case ESlateDebuggingInputEvent::MouseButtonDoubleClick:
			OutKind = GameTestHandlerWatch::EKind::MouseDown;
			return true;
		case ESlateDebuggingInputEvent::MouseButtonUp:
			OutKind = GameTestHandlerWatch::EKind::MouseUp;
			return true;
		case ESlateDebuggingInputEvent::MouseWheel:
			OutKind = GameTestHandlerWatch::EKind::MouseWheel;
			return true;
		case ESlateDebuggingInputEvent::KeyDown:
			OutKind = GameTestHandlerWatch::EKind::KeyDown;
			return true;
		case ESlateDebuggingInputEvent::KeyUp:
			OutKind = GameTestHandlerWatch::EKind::KeyUp;
			return true;
		case ESlateDebuggingInputEvent::KeyChar:
			OutKind = GameTestHandlerWatch::EKind::KeyChar;
			return true;
		default:
			return false;
		}
	}

	void OnInputEvent(const FSlateDebuggingInputEventArgs& InArgs)
	{
		// 冒泡途中每个被问到的控件都可能广播一次：只记真正接住（Handled）的那一个。
		if (!InArgs.Reply.IsEventHandled() || !InArgs.HandlerWidget.IsValid())
		{
			return;
		}

		GameTestHandlerWatch::EKind Kind;
		if (!ToKind(InArgs.InputEventType, Kind))
		{
			return;
		}

		GameTestHandlerWatch::FHandled Record;
		Record.Handler = InArgs.HandlerWidget;
		Record.Serial = NextSerial++;
		Record.Frame = GFrameCounter;

		// 事件对象的实际类型由事件类型决定（引擎这里传的是基类指针）。
		if (InArgs.InputEvent != nullptr)
		{
			if (Kind == GameTestHandlerWatch::EKind::KeyChar)
			{
				Record.Character = static_cast<const FCharacterEvent*>(InArgs.InputEvent)->GetCharacter();
			}
			else if (Kind == GameTestHandlerWatch::EKind::KeyDown || Kind == GameTestHandlerWatch::EKind::KeyUp)
			{
				Record.Key = static_cast<const FKeyEvent*>(InArgs.InputEvent)->GetKey().GetFName();
			}
		}

		LastByKind[static_cast<int32>(Kind)] = Record;
		GameTestHandlerWatch::OnHandled().Broadcast(Kind, Record);
	}
#endif
}

bool GameTestHandlerWatch::IsSupported()
{
#if WITH_SLATE_DEBUGGING
	return true;
#else
	return false;
#endif
}

void GameTestHandlerWatch::Start()
{
	if (GameTestHandlerWatchDetail::bStarted)
	{
		return;
	}
	GameTestHandlerWatchDetail::bStarted = true;
#if WITH_SLATE_DEBUGGING
	GameTestHandlerWatchDetail::InputHandle = FSlateDebugging::InputEvent.AddStatic(&GameTestHandlerWatchDetail::OnInputEvent);
#endif
}

void GameTestHandlerWatch::Stop()
{
	if (!GameTestHandlerWatchDetail::bStarted)
	{
		return;
	}
	GameTestHandlerWatchDetail::bStarted = false;
#if WITH_SLATE_DEBUGGING
	FSlateDebugging::InputEvent.Remove(GameTestHandlerWatchDetail::InputHandle);
	GameTestHandlerWatchDetail::InputHandle.Reset();
#endif
}

GameTestHandlerWatch::FHandled GameTestHandlerWatch::GetLast(EKind InKind)
{
	return GameTestHandlerWatchDetail::LastByKind[static_cast<int32>(InKind)];
}

GameTestHandlerWatch::FOnHandled& GameTestHandlerWatch::OnHandled()
{
	static FOnHandled Delegate;
	return Delegate;
}
