// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestSlate.h"

#include "Core/GameTestWorld.h"

#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Slate/SObjectWidget.h"
#include "Types/ReflectionMetadata.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace GameTestSlateDetail
{
	/** 元数据查不到时的退路：在 UMG 对象里按 `GetCachedWidget()` 指针比对（慢，只在退路上用）。 */
	UWidget* FindSourceWidgetSlow(const SWidget& InWidget)
	{
		for (TObjectIterator<UWidget> Iterator; Iterator; ++Iterator)
		{
			UWidget* Widget = *Iterator;
			if (!IsValid(Widget) || Widget->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
			{
				continue;
			}

			const TSharedPtr<SWidget> Cached = Widget->GetCachedWidget();
			if (Cached.IsValid() && Cached.Get() == &InWidget)
			{
				return Widget;
			}
		}
		return nullptr;
	}

	/** 元数据只要在，就一定比线性扫描准；只有一个控件都查不到元数据时才说明这个进程没开元数据。 */
	bool bMetadataSeen = false;

	void AppendText(const TSharedRef<SWidget>& InWidget, FString& InOutText, int32 InMaxChars, int32 InDepth)
	{
		if (InOutText.Len() >= InMaxChars || InDepth > 32)
		{
			return;
		}

		if (InWidget->GetType() == FName(TEXT("STextBlock")))
		{
			const FString Text = StaticCastSharedRef<STextBlock>(InWidget)->GetText().ToString().TrimStartAndEnd();
			if (!Text.IsEmpty())
			{
				if (!InOutText.IsEmpty())
				{
					InOutText.AppendChar(TEXT(' '));
				}
				InOutText.Append(Text);
			}
		}

		FChildren* Children = InWidget->GetChildren();
		if (Children == nullptr)
		{
			return;
		}
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			AppendText(Children->GetChildAt(Index), InOutText, InMaxChars, InDepth + 1);
		}
	}
}

UWidget* GameTestSlate::FindSourceWidget(const SWidget& InWidget)
{
	if (const TSharedPtr<FReflectionMetaData> MetaData = InWidget.GetMetaData<FReflectionMetaData>())
	{
		GameTestSlateDetail::bMetadataSeen = true;
		// const_cast：有的引擎版本 SourceObject 存的是 const 指针，这里只拿来反查，不经它改控件。
		if (UWidget* Widget = const_cast<UWidget*>(Cast<UWidget>(MetaData->SourceObject.Get())))
		{
			return Widget;
		}
	}

	// UUserWidget 的外壳 SObjectWidget 自己知道是哪个 WBP：元数据没挂上时也能认出来。
	if (InWidget.GetType() == FName(TEXT("SObjectWidget")))
	{
		return static_cast<const SObjectWidget&>(InWidget).GetWidgetObject();
	}

	// 见过元数据就说明这个进程开着它：这个控件没有，就是纯 Slate 控件，不必再扫一遍。
	return GameTestSlateDetail::bMetadataSeen ? nullptr : GameTestSlateDetail::FindSourceWidgetSlow(InWidget);
}

UWidget* GameTestSlate::FindOwningWidget(const TSharedPtr<SWidget>& InWidget)
{
	const TSharedPtr<SWidget> Viewport = GetGameViewportWidget();
	TSharedPtr<SWidget> Current = InWidget;
	for (int32 Guard = 0; Current.IsValid() && Guard < 256; ++Guard)
	{
		if (Current == Viewport || Current->GetType() == FName(TEXT("SWindow")))
		{
			return nullptr;
		}

		if (UWidget* Widget = FindSourceWidget(*Current))
		{
			return Widget;
		}
		Current = Current->GetParentWidget();
	}
	return nullptr;
}

TSharedPtr<SWidget> GameTestSlate::GetGameViewportWidget()
{
	UGameViewportClient* Viewport = GameTestWorld::FindGameViewport();
	return Viewport != nullptr ? StaticCastSharedPtr<SWidget>(Viewport->GetGameViewportWidget()) : nullptr;
}

TSharedPtr<SWindow> GameTestSlate::GetGameWindow()
{
	UGameViewportClient* Viewport = GameTestWorld::FindGameViewport();
	return Viewport != nullptr ? Viewport->GetWindow() : nullptr;
}

bool GameTestSlate::IsGameWindowOrChild(const TSharedPtr<SWindow>& InWindow)
{
	const TSharedPtr<SWindow> GameWindow = GetGameWindow();
	if (!InWindow.IsValid() || !GameWindow.IsValid())
	{
		return false;
	}

	for (TSharedPtr<SWindow> Window = InWindow; Window.IsValid(); Window = Window->GetParentWindow())
	{
		if (Window == GameWindow)
		{
			return true;
		}
	}
	return false;
}

bool GameTestSlate::IsInGameUi(const TSharedPtr<SWidget>& InWidget, const TSharedPtr<SWindow>& InWindow)
{
	if (!InWidget.IsValid())
	{
		return false;
	}
	const TSharedPtr<SWidget> Viewport = GetGameViewportWidget();
	if (IsSelfOrAncestor(Viewport.Get(), InWidget))
	{
		return true;
	}

	// 不在视口里：只认游戏窗口弹出的子窗口，游戏窗口本身（可能是编辑器主窗口）不算。
	const TSharedPtr<SWindow> Window = InWindow.IsValid() || !FSlateApplication::IsInitialized()
		? InWindow
		: FSlateApplication::Get().FindWidgetWindow(InWidget.ToSharedRef());
	return Window.IsValid() && Window != GetGameWindow() && IsGameWindowOrChild(Window);
}

bool GameTestSlate::IsSelfOrAncestor(const SWidget* InAncestor, const TSharedPtr<SWidget>& InWidget)
{
	if (InAncestor == nullptr)
	{
		return false;
	}
	TSharedPtr<SWidget> Current = InWidget;
	for (int32 Guard = 0; Current.IsValid() && Guard < 256; ++Guard)
	{
		if (Current.Get() == InAncestor)
		{
			return true;
		}
		Current = Current->GetParentWidget();
	}
	return false;
}

FString GameTestSlate::GetAssetPath(const UWidget& InWidget)
{
	const UClass* Class = InWidget.GetClass();
	if (Class == nullptr)
	{
		return FString();
	}
	// 蓝图生成类住在 WBP 资源的包里：包名就是内容浏览器里看到的路径。原生类的包是 /Script/模块。
	const UPackage* Package = Class->GetOutermost();
	return Package != nullptr ? Package->GetName() : Class->GetPathName();
}

FString GameTestSlate::CollectText(const TSharedPtr<SWidget>& InWidget, int32 InMaxChars)
{
	FString Text;
	if (InWidget.IsValid())
	{
		GameTestSlateDetail::AppendText(InWidget.ToSharedRef(), Text, InMaxChars, 0);
	}
	return Text.Left(InMaxChars);
}

FString GameTestSlate::Describe(const TSharedPtr<SWidget>& InWidget)
{
	if (!InWidget.IsValid())
	{
		return TEXT("（无）");
	}

	const FString SlateType = InWidget->GetTypeAsString();
	const UWidget* Widget = FindSourceWidget(*InWidget);
	if (Widget == nullptr)
	{
		return SlateType;
	}

	const UUserWidget* Owner = Widget->GetTypedOuter<UUserWidget>();
	return Owner != nullptr
		? FString::Printf(TEXT("%s → %s(%s)@%s"), *SlateType, *Widget->GetClass()->GetName(), *Widget->GetFName().ToString(), *Owner->GetClass()->GetName())
		: FString::Printf(TEXT("%s → %s(%s)"), *SlateType, *Widget->GetClass()->GetName(), *Widget->GetFName().ToString());
}
