// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestSlate.h"

#include "Core/GameTestWorld.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ListView.h"
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

	/** 只看元数据和 SObjectWidget 的反查：快照每帧要走很多 Slate 控件，不能掉进线性扫描。 */
	UWidget* FindSourceWidgetFast(const SWidget& InWidget)
	{
		if (const TSharedPtr<FReflectionMetaData> MetaData = InWidget.GetMetaData<FReflectionMetaData>())
		{
			if (UWidget* Widget = const_cast<UWidget*>(Cast<UWidget>(MetaData->SourceObject.Get())))
			{
				return Widget;
			}
		}
		if (InWidget.GetType() == FName(TEXT("SObjectWidget")))
		{
			return static_cast<const SObjectWidget&>(InWidget).GetWidgetObject();
		}
		return nullptr;
	}

	/** 没有 UMG 父面板、也不是哪个 WBP 控件树的根：只能是从 Slate 挂上去的（或直接加在视口上）。 */
	bool HasNoUmgParent(const UUserWidget& InWidget)
	{
		if (InWidget.GetParent() != nullptr)
		{
			return false;
		}
		const UWidgetTree* Tree = Cast<UWidgetTree>(InWidget.GetOuter());
		const UUserWidget* TreeOwner = Tree != nullptr ? Cast<UUserWidget>(Tree->GetOuter()) : nullptr;
		return TreeOwner == nullptr || TreeOwner->WidgetTree == nullptr || TreeOwner->WidgetTree->RootWidget != &InWidget;
	}

	/** 每个容器最多看这么多 Slate 控件：正常容器到下一个 UMG 控件只隔一两层包装，再多说明走进了不该走的地方。 */
	constexpr int32 AttachedScanBudget = 512;

	void CollectAttached(const SWidget& InSlate, const UWidget& InContainer, TArray<UUserWidget*>& OutWidgets, int32& InOutBudget, int32 InDepth)
	{
		FChildren* Children = const_cast<SWidget&>(InSlate).GetChildren();
		if (Children == nullptr || InDepth > 32)
		{
			return;
		}
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			if (--InOutBudget < 0)
			{
				return;
			}
			const TSharedRef<SWidget> Child = Children->GetChildAt(Index);
			UWidget* Source = FindSourceWidgetFast(*Child);
			if (Source != nullptr && Source != &InContainer)
			{
				// 别的 UMG 控件：它的子树归它自己。是没有 UMG 父面板的 WBP 才算挂在这个容器上。
				UUserWidget* UserWidget = Cast<UUserWidget>(Source);
				if (UserWidget != nullptr && HasNoUmgParent(*UserWidget))
				{
					OutWidgets.AddUnique(UserWidget);
				}
				continue;
			}
			CollectAttached(*Child, InContainer, OutWidgets, InOutBudget, InDepth + 1);
		}
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

void GameTestSlate::CollectAttachedUserWidgets(const UWidget& InContainer, TArray<UUserWidget*>& OutWidgets)
{
	OutWidgets.Reset();
	if (InContainer.IsA<UUserWidget>() || InContainer.IsA<UListView>())
	{
		return;
	}
	const TSharedPtr<SWidget> Slate = InContainer.GetCachedWidget();
	if (!Slate.IsValid())
	{
		return;
	}
	int32 Budget = GameTestSlateDetail::AttachedScanBudget;
	GameTestSlateDetail::CollectAttached(*Slate, InContainer, OutWidgets, Budget, 0);
}

UWidget* GameTestSlate::FindAttachContainer(const UUserWidget& InWidget)
{
	if (!GameTestSlateDetail::HasNoUmgParent(InWidget))
	{
		return nullptr;
	}
	const TSharedPtr<SWidget> Slate = InWidget.GetCachedWidget();
	if (!Slate.IsValid())
	{
		return nullptr;
	}
	UWidget* Container = FindOwningWidget(Slate->GetParentWidget());
	if (Container == nullptr || Container->IsA<UUserWidget>() || Container->IsA<UListView>())
	{
		return nullptr;
	}
	// 用快照同一套收集核对一遍：两边认的必须是同一个容器，否则写出来的路径回放时走不到。
	TArray<UUserWidget*> Attached;
	CollectAttachedUserWidgets(*Container, Attached);
	return Attached.Contains(&InWidget) ? Container : nullptr;
}

bool GameTestSlate::GetViewportRect(const SWidget& InWidget, FVector2D& OutMin, FVector2D& OutSize)
{
	const TSharedPtr<SWidget> Viewport = GetGameViewportWidget();
	if (!Viewport.IsValid())
	{
		return false;
	}
	const FGeometry& ViewGeometry = Viewport->GetCachedGeometry();
	const FGeometry& Geometry = InWidget.GetCachedGeometry();
	const FVector2D ViewPos = FVector2D(ViewGeometry.GetAbsolutePosition());
	const FVector2D ViewSize = FVector2D(ViewGeometry.GetAbsoluteSize());
	const FVector2D Pos = FVector2D(Geometry.GetAbsolutePosition());
	const FVector2D Size = FVector2D(Geometry.GetAbsoluteSize());
	if (ViewSize.X < 1.0 || ViewSize.Y < 1.0 || Size.X < 1.0 || Size.Y < 1.0)
	{
		return false;
	}
	OutMin = FVector2D((Pos.X - ViewPos.X) / ViewSize.X, (Pos.Y - ViewPos.Y) / ViewSize.Y);
	OutSize = FVector2D(Size.X / ViewSize.X, Size.Y / ViewSize.Y);
	return true;
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
