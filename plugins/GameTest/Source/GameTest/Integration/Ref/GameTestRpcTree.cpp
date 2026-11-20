// Copyright Epic Games, Inc. All Rights Reserved.

// gt.tree（docs/game-test-ref.md 7.1）：此刻游戏页面上的 UMG 树，只列 UWidget，不列纯 Slate。
// id 是这次调用编的临时号，只给 gt.highlight / gt.props / gt.refFor 用，不能存进脚本。

#include "Integration/Ref/GameTestRpcRef.h"

#include "Core/GameTestWorld.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Observe/GameTestItemKey.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRefFor.h"
#include "Observe/Ref/GameTestRefResolve.h"
#include "Observe/Ref/GameTestState.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/DynamicEntryBoxBase.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/ListViewBase.h"
#include "Components/PanelWidget.h"
#include "Components/TextWidgetTypes.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/SViewport.h"

namespace GameTestRpcTreeImpl
{
	constexpr int32 DefaultLimit = 1500;
	constexpr int32 MaxLimit = 5000;
	/** 树太深多半是循环引用（条目里套自己）：到这里停。 */
	constexpr int32 MaxDepth = 64;

	/** 本次会话最近一次 gt.tree 的编号（游戏线程上读写）。 */
	TMap<FString, TWeakObjectPtr<UWidget>> Nodes;
	TMap<TWeakObjectPtr<UWidget>, FString> Ids;

	struct FWalk
	{
		FGameTestFlowContext& Flow;
		FString Filter;
		bool bOnlyVisible = false;
		int32 MaxDepthWanted = MaxDepth;
		int32 Limit = DefaultLimit;
		int32 Total = 0;
		bool bTruncated = false;
		FVector2D ViewportPos = FVector2D::ZeroVector;
		FVector2D ViewportSize = FVector2D::ZeroVector;
		TArray<TSharedPtr<FJsonValue>> Out;
		/** 只要这个子树时：走到它才开始输出。 */
		UWidget* Root = nullptr;
	};

	FString NewId(UWidget* InWidget)
	{
		const FString Id = FString::Printf(TEXT("n%d"), Nodes.Num() + 1);
		Nodes.Add(Id, InWidget);
		Ids.Add(InWidget, Id);
		return Id;
	}

	FString ClassNameOf(const UObject* InObject)
	{
		FString Name = InObject->GetClass()->GetName();
		Name.RemoveFromEnd(TEXT("_C"));
		return Name;
	}

	bool IsTextWidget(const UWidget* InWidget)
	{
		return InWidget->IsA<UTextLayoutWidget>() || InWidget->IsA<UEditableText>() || InWidget->IsA<UEditableTextBox>();
	}

	void AddNode(FWalk& InWalk, UWidget* InWidget, const FString& InId, const FString& InParent, int32 InDepth, const TSharedPtr<FJsonObject>& InContainer, const TSharedPtr<FJsonObject>& InEntry)
	{
		++InWalk.Total;
		if (InWalk.bOnlyVisible && !InWidget->IsVisible())
		{
			return;
		}
		const FString ClassName = ClassNameOf(InWidget);
		if (!InWalk.Filter.IsEmpty() && !ClassName.Contains(InWalk.Filter) && !InWidget->GetName().Contains(InWalk.Filter))
		{
			return;
		}
		if (InWalk.Out.Num() >= InWalk.Limit)
		{
			InWalk.bTruncated = true;
			return;
		}
		const TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("id"), InId);
		if (!InParent.IsEmpty())
		{
			Node->SetStringField(TEXT("parent"), InParent);
		}
		Node->SetNumberField(TEXT("depth"), InDepth);
		Node->SetStringField(TEXT("class"), ClassName);
		Node->SetStringField(TEXT("name"), InWidget->GetName());
		Node->SetBoolField(TEXT("user"), InWidget->IsA<UUserWidget>());
		UUserWidget* Owner = GameTestRefFor::OwnerOf(InWidget);
		if (Owner != nullptr)
		{
			const FString* OwnerId = Ids.Find(Owner);
			if (OwnerId != nullptr)
			{
				Node->SetStringField(TEXT("owner"), *OwnerId);
			}
			Node->SetStringField(TEXT("ownerClass"), Owner->GetClass()->GetName());
			FString Part;
			if (GameTestPropRead::PartPathOf(InWidget, Owner, Part))
			{
				Node->SetStringField(TEXT("part"), Part);
			}
		}
		Node->SetBoolField(TEXT("visible"), InWidget->IsVisible());
		const FGameTestPropRead OnScreen = GameTestPropRead::ReadText(InWidget, TEXT(":onScreen"), InWalk.Flow);
		Node->SetBoolField(TEXT("onScreen"), OnScreen.IsResolved() && OnScreen.Value.Truthy());
		const FGeometry& Geometry = InWidget->GetCachedGeometry();
		const FVector2D Size = FVector2D(Geometry.GetAbsoluteSize());
		if (InWalk.ViewportSize.X > 1.0 && InWalk.ViewportSize.Y > 1.0 && Size.X >= 1.0 && Size.Y >= 1.0)
		{
			const FVector2D Pos = (FVector2D(Geometry.GetAbsolutePosition()) - InWalk.ViewportPos) / InWalk.ViewportSize;
			const FVector2D Rel = Size / InWalk.ViewportSize;
			TArray<TSharedPtr<FJsonValue>> Rect;
			for (const double Value : { Pos.X, Pos.Y, Rel.X, Rel.Y })
			{
				Rect.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Value * 10000.0) / 10000.0));
			}
			Node->SetArrayField(TEXT("rect"), Rect);
		}
		FText Text;
		if (IsTextWidget(InWidget) && GameTestStateUtil::TextWidgetText(InWidget, Text))
		{
			Node->SetStringField(TEXT("text"), Text.ToString().Left(60));
		}
		if (InContainer.IsValid())
		{
			Node->SetObjectField(TEXT("container"), InContainer);
		}
		if (InEntry.IsValid())
		{
			Node->SetObjectField(TEXT("entry"), InEntry);
		}
		InWalk.Out.Add(MakeShared<FJsonValueObject>(Node));
	}

	void Walk(FWalk& InWalk, UWidget* InWidget, const FString& InParent, int32 InDepth, const TSharedPtr<FJsonObject>& InEntry, bool bInEmit)
	{
		if (InWidget == nullptr || InDepth > MaxDepth || Ids.Contains(InWidget))
		{
			return;
		}
		const bool bEmit = bInEmit || InWidget == InWalk.Root;
		const int32 Depth = InWalk.Root != nullptr && bEmit && !bInEmit ? 0 : InDepth;
		const FString Id = NewId(InWidget);

		// 容器：ListView 族、DynamicEntryBox 的条目；面板的子控件
		TSharedPtr<FJsonObject> Container;
		TArray<FGameTestKeyEntry> Entries;
		TArray<UWidget*> Children;
		if (UPanelWidget* Panel = Cast<UPanelWidget>(InWidget))
		{
			for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
			{
				Children.Add(Panel->GetChildAt(Index));
			}
			Container = MakeShared<FJsonObject>();
			Container->SetStringField(TEXT("kind"), TEXT("panel"));
			Container->SetNumberField(TEXT("count"), Children.Num());
		}
		else if ((InWidget->IsA<UListViewBase>() || InWidget->IsA<UDynamicEntryBoxBase>()) && GameTestItemKey::CollectEntries(InWidget, Entries))
		{
			Container = MakeShared<FJsonObject>();
			Container->SetStringField(TEXT("kind"), InWidget->IsA<UListViewBase>() ? TEXT("list") : TEXT("entrybox"));
			Container->SetNumberField(TEXT("count"), Entries.Num());
			for (const FGameTestKeyEntry& Entry : Entries)
			{
				if (UWidget* EntryWidget = Entry.Widget.Get())
				{
					Container->SetStringField(TEXT("entryClass"), ClassNameOf(EntryWidget));
					break;
				}
			}
		}
		if (bEmit && (InWalk.MaxDepthWanted < 0 || Depth <= InWalk.MaxDepthWanted))
		{
			AddNode(InWalk, InWidget, Id, bInEmit ? InParent : FString(), Depth, Container, InEntry);
		}
		if (UUserWidget* User = Cast<UUserWidget>(InWidget))
		{
			if (User->WidgetTree != nullptr)
			{
				Walk(InWalk, User->WidgetTree->RootWidget, Id, Depth + 1, nullptr, bEmit);
			}
			return;
		}
		for (UWidget* Child : Children)
		{
			Walk(InWalk, Child, Id, Depth + 1, nullptr, bEmit);
		}
		for (const FGameTestKeyEntry& Entry : Entries)
		{
			UWidget* EntryWidget = Entry.Widget.Get();
			if (EntryWidget == nullptr)
			{
				continue;
			}
			const TSharedRef<FJsonObject> EntryJson = MakeShared<FJsonObject>();
			EntryJson->SetStringField(TEXT("container"), Id);
			EntryJson->SetNumberField(TEXT("index"), Entry.Index);
			Walk(InWalk, EntryWidget, Id, Depth + 1, EntryJson, bEmit);
		}
	}
}

UWidget* GameTestRpcRef::FindNode(const FString& InId)
{
	const TWeakObjectPtr<UWidget>* Found = GameTestRpcTreeImpl::Nodes.Find(InId);
	return Found != nullptr ? Found->Get() : nullptr;
}

FString GameTestRpcRef::NodeIdOf(const UWidget* InWidget)
{
	const FString* Found = GameTestRpcTreeImpl::Ids.Find(const_cast<UWidget*>(InWidget));
	return Found != nullptr ? *Found : FString();
}

bool GameTestRpcRef::Tree(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
{
	using namespace GameTestRpcTreeImpl;
	FGameTestFlowContext Flow;
	MakeContext(Flow);
	if (Flow.World == nullptr)
	{
		OutError = TEXT("游戏没在跑：gt.tree 要开着游戏（PIE 或游戏进程）");
		return false;
	}
	FWalk Walk{ Flow };
	FString RootId;
	if (InArgs.IsValid())
	{
		InArgs->TryGetStringField(TEXT("filter"), Walk.Filter);
		InArgs->TryGetBoolField(TEXT("onlyVisible"), Walk.bOnlyVisible);
		InArgs->TryGetStringField(TEXT("root"), RootId);
		Walk.MaxDepthWanted = -1;
		InArgs->TryGetNumberField(TEXT("depth"), Walk.MaxDepthWanted);
		InArgs->TryGetNumberField(TEXT("limit"), Walk.Limit);
	}
	Walk.Limit = FMath::Clamp(Walk.Limit, 1, MaxLimit);
	if (!RootId.IsEmpty())
	{
		Walk.Root = FindNode(RootId);
		if (Walk.Root == nullptr)
		{
			OutError = FString::Printf(TEXT("没有节点 %s（gt.tree 每次调用重新编号，先不带 root 调一次）"), *RootId);
			return false;
		}
	}
	// 视口在屏幕上的位置和大小：rect 按它归一化（和 gt.pick at 的坐标同一套）
	if (GEngine != nullptr && GEngine->GameViewport != nullptr)
	{
		if (const TSharedPtr<SViewport> Viewport = GEngine->GameViewport->GetGameViewportWidget())
		{
			const FGeometry& Geometry = Viewport->GetCachedGeometry();
			Walk.ViewportPos = FVector2D(Geometry.GetAbsolutePosition());
			Walk.ViewportSize = FVector2D(Geometry.GetAbsoluteSize());
		}
	}
	Nodes.Reset();
	Ids.Reset();
	TArray<UUserWidget*> Screens;
	GameTestWidgetProbe::ListScreens(Flow.World, Screens);
	for (UUserWidget* Screen : Screens)
	{
		GameTestRpcTreeImpl::Walk(Walk, Screen, FString(), 0, nullptr, Walk.Root == nullptr);
	}
	OutResult = MakeShared<FJsonObject>();
	OutResult->SetNumberField(TEXT("frame"), static_cast<double>(GFrameCounter));
	OutResult->SetArrayField(TEXT("nodes"), Walk.Out);
	OutResult->SetNumberField(TEXT("total"), Walk.Total);
	OutResult->SetBoolField(TEXT("truncated"), Walk.bTruncated);
	return true;
}
