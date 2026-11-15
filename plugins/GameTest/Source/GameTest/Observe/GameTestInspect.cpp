// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestInspect.h"

#include "Input/GameTestHandlerWatch.h"
#include "Observe/GameTestSelectorWriter.h"
#include "Observe/GameTestSlate.h"

#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace GameTestInspectDetail
{
	/** 路径最多列这么多层：再深的是布局容器，看最里面几层就够定位，返回体也不撑大。 */
	constexpr int32 MaxPathItems = 30;
}

bool GameTestInspect::Inspect(const TOptional<FVector2D>& InNormalizedPoint, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
{
	if (!FSlateApplication::IsInitialized())
	{
		OutError = TEXT("这个进程没有 Slate（命令行或 -nullrhi），查不了控件");
		return false;
	}

	FSlateApplication& App = FSlateApplication::Get();
	const TSharedPtr<SWidget> Viewport = GameTestSlate::GetGameViewportWidget();
	if (!Viewport.IsValid())
	{
		OutError = TEXT("没有游戏视口：先启动 PIE 或游戏");
		return false;
	}
	const FGeometry& ViewGeometry = Viewport->GetCachedGeometry();
	const FVector2D ViewSize = FVector2D(ViewGeometry.GetLocalSize());

	FVector2D Screen;
	if (InNormalizedPoint.IsSet())
	{
		const FVector2D Clamped(FMath::Clamp(InNormalizedPoint->X, 0.0, 1.0), FMath::Clamp(InNormalizedPoint->Y, 0.0, 1.0));
		Screen = FVector2D(ViewGeometry.GetAbsolutePositionAtCoordinates(FVector2f(Clamped)));
	}
	else
	{
		Screen = App.GetCursorPos();
	}

	const FWidgetPath Path = App.LocateWindowUnderMouse(Screen, App.GetInteractiveTopLevelWindows());
	if (!Path.IsValid() || !GameTestSlate::IsInGameUi(Path.GetLastWidget(), Path.GetWindow()))
	{
		OutError = InNormalizedPoint.IsSet()
			? TEXT("这个点下面不是游戏界面（被别的窗口挡住了？）")
			: TEXT("光标不在游戏界面上：把光标移到要查的控件上再查，或传 x、y");
		return false;
	}

	OutResult = MakeShared<FJsonObject>();
	OutResult->SetBoolField(TEXT("supported"), GameTestHandlerWatch::IsSupported());
	OutResult->SetStringField(TEXT("handlerSource"), GameTestHandlerWatch::IsSupported() ? TEXT("slateDebugging") : TEXT("hitPath"));

	const FVector2D Local = FVector2D(ViewGeometry.AbsoluteToLocal(Screen));
	TArray<TSharedPtr<FJsonValue>> Point;
	Point.Add(MakeShared<FJsonValueNumber>(ViewSize.X > 0.0 ? FMath::RoundToInt(Local.X / ViewSize.X * 1000.0) / 1000.0 : 0.0));
	Point.Add(MakeShared<FJsonValueNumber>(ViewSize.Y > 0.0 ? FMath::RoundToInt(Local.Y / ViewSize.Y * 1000.0) / 1000.0 : 0.0));
	OutResult->SetArrayField(TEXT("point"), Point);

	// 命中路径从外到内；太长时只留最里面几层（外面是窗口和布局容器）。
	TArray<TSharedPtr<FJsonValue>> Items;
	const int32 Count = Path.Widgets.Num();
	for (int32 Index = FMath::Max(0, Count - GameTestInspectDetail::MaxPathItems); Index < Count; ++Index)
	{
		const TSharedRef<SWidget>& Widget = Path.Widgets[Index].Widget;
		const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("slate"), Widget->GetTypeAsString());
		if (UWidget* Source = GameTestSlate::FindSourceWidget(*Widget))
		{
			Item->SetStringField(TEXT("widget"), Source->GetName());
			Item->SetStringField(TEXT("class"), Source->GetClass()->GetName());
		}
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	if (Count > GameTestInspectDetail::MaxPathItems)
	{
		OutResult->SetNumberField(TEXT("pathSkipped"), Count - GameTestInspectDetail::MaxPathItems);
	}
	OutResult->SetArrayField(TEXT("path"), Items);

	// 最深的 UMG 控件写成选择器：和录制用同一套写法和核对。
	const TSharedPtr<SWidget> Leaf = Path.GetLastWidget();
	if (UWidget* Owner = GameTestSlate::FindOwningWidget(Leaf))
	{
		FGameTestWrittenTarget Written;
		FString WriteError;
		if (GameTestSelectorWriter::Write(*Owner, Written, WriteError))
		{
			OutResult->SetStringField(TEXT("target"), Written.Target);
			OutResult->SetBoolField(TEXT("verified"), Written.bVerified);
			if (Written.Hint.IsValid())
			{
				OutResult->SetObjectField(TEXT("hint"), Written.Hint);
			}
		}
		else
		{
			OutResult->SetStringField(TEXT("targetError"), WriteError);
		}
	}
	else
	{
		OutResult->SetStringField(TEXT("targetError"), TEXT("这个点下面没有 UMG 控件：是游戏画面，或纯 Slate 界面"));
	}

	const GameTestHandlerWatch::FHandled Last = GameTestHandlerWatch::GetLast(GameTestHandlerWatch::EKind::MouseDown);
	if (const TSharedPtr<SWidget> Handler = Last.Handler.Pin())
	{
		OutResult->SetStringField(TEXT("lastHandler"), GameTestSlate::Describe(Handler));
	}
	return true;
}
