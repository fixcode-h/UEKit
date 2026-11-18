// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestInspect.h"

#include "Input/GameTestHandlerWatch.h"
#include "Observe/GameTestSelectorWriter.h"
#include "Observe/GameTestSlate.h"

#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
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

	/** 抓控件时往上找按钮 / 勾选框最多几层：按钮里常套一层布局再放文字和图标。 */
	constexpr int32 MaxInteractiveClimb = 4;

	bool IsInteractive(const UWidget& InWidget)
	{
		return InWidget.IsA<UButton>() || InWidget.IsA<UCheckBox>();
	}

	/** 面板「一键分路」按控件种类给分法：按钮 → 能点 / 显示但不能点 / 不显示，勾选框 → 勾上 / 没勾，文字 → 文字是 … / 其余。 */
	const TCHAR* KindOf(const UWidget& InWidget)
	{
		if (InWidget.IsA<UButton>())
		{
			return TEXT("button");
		}
		if (InWidget.IsA<UCheckBox>())
		{
			return TEXT("check");
		}
		if (InWidget.IsA<UTextBlock>())
		{
			return TEXT("text");
		}
		return TEXT("other");
	}

	UWidget* PreferInteractive(UWidget* InWidget)
	{
		UWidget* Current = InWidget;
		for (int32 Level = 0; Current != nullptr && Level <= MaxInteractiveClimb; ++Level)
		{
			if (IsInteractive(*Current))
			{
				return Current;
			}
			Current = Current->GetParent();
		}
		return InWidget;
	}

	/**
	 * 控件所在的界面（WBP）：`screen` 是它的选择器、`screenClass` 是类名。面板抓控件时按它列「所在界面的变量」，
	 * 选到的是按钮时 gt.vars 只列得出按钮自己的属性。点到的本身就是界面时不写。
	 */
	void WriteScreen(const UWidget& InWidget, FJsonObject& OutResult)
	{
		UUserWidget* Screen = InWidget.GetTypedOuter<UUserWidget>();
		if (Screen == nullptr)
		{
			return;
		}
		OutResult.SetStringField(TEXT("screenClass"), Screen->GetClass()->GetName());
		FGameTestWrittenTarget Written;
		FString WriteError;
		if (GameTestSelectorWriter::Write(*Screen, Written, WriteError))
		{
			OutResult.SetStringField(TEXT("screen"), Written.Target);
		}
	}

	/** 控件在视口里的归一化 [x, y, w, h]：面板在实时画面上画框用。几何还没算出来（刚创建）时不写。 */
	void WriteRect(const UWidget& InWidget, const FGeometry& InViewGeometry, const FVector2D& InViewSize, FJsonObject& OutResult)
	{
		const FGeometry& Geometry = InWidget.GetCachedGeometry();
		const FVector2D AbsSize = FVector2D(Geometry.GetAbsoluteSize());
		if (InViewSize.X <= 0.0 || InViewSize.Y <= 0.0 || AbsSize.X < 1.0 || AbsSize.Y < 1.0)
		{
			return;
		}
		const FVector2D AbsMin = FVector2D(Geometry.GetAbsolutePosition());
		const FVector2D Min = FVector2D(InViewGeometry.AbsoluteToLocal(AbsMin));
		const FVector2D Max = FVector2D(InViewGeometry.AbsoluteToLocal(AbsMin + AbsSize));
		TArray<TSharedPtr<FJsonValue>> Rect;
		Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Min.X / InViewSize.X * 1000.0) / 1000.0));
		Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Min.Y / InViewSize.Y * 1000.0) / 1000.0));
		Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt((Max.X - Min.X) / InViewSize.X * 1000.0) / 1000.0));
		Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt((Max.Y - Min.Y) / InViewSize.Y * 1000.0) / 1000.0));
		OutResult.SetArrayField(TEXT("rect"), Rect);
	}
}

bool GameTestInspect::Inspect(const TOptional<FVector2D>& InNormalizedPoint, TSharedPtr<FJsonObject>& OutResult, FString& OutError, bool bInPreferInteractive)
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
	UWidget* Owner = GameTestSlate::FindOwningWidget(Leaf);
	if (Owner != nullptr && bInPreferInteractive)
	{
		Owner = GameTestInspectDetail::PreferInteractive(Owner);
	}
	if (Owner != nullptr)
	{
		OutResult->SetStringField(TEXT("widgetClass"), Owner->GetClass()->GetName());
		OutResult->SetStringField(TEXT("kind"), GameTestInspectDetail::KindOf(*Owner));
		GameTestInspectDetail::WriteRect(*Owner, ViewGeometry, ViewSize, *OutResult);
		GameTestInspectDetail::WriteScreen(*Owner, *OutResult);
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
