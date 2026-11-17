// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestSelectorWriter.h"
#include "GameTestCompat.h"

#include "Core/GameTestWorld.h"
#include "Observe/GameTestHintPick.h"
#include "Observe/GameTestSelector.h"
#include "Observe/GameTestSlate.h"
#include "Observe/GameTestSnapshot.h"
#include "Observe/GameTestWidgetNode.h"

#include "Blueprint/IUserListEntry.h"
#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ListView.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Widgets/SWidget.h"

namespace GameTestSelectorWriterDetail
{
	/** 一层套一层的 WBP 写到这么深还没到顶，说明父子关系绕成了环（不该发生），直接放弃。 */
	constexpr int32 MaxDepth = 16;

	/** 找 Id 类属性时最多看这么多条目：再多就只用序号，录制时不为一次点击导出几千个数据项。 */
	constexpr int32 MaxItemsForProperty = 200;

	double Round3(double InValue)
	{
		return FMath::RoundToDouble(InValue * 1000.0) / 1000.0;
	}

	/** 控件所在的 WBP：沿 Outer 找到 UWidgetTree，它的 Outer 就是那个 UUserWidget。 */
	UUserWidget* GetTreeOwner(const UWidget& InWidget)
	{
		const UWidgetTree* Tree = Cast<UWidgetTree>(InWidget.GetOuter());
		return Tree != nullptr ? Cast<UUserWidget>(Tree->GetOuter()) : nullptr;
	}

	/**
	 * 这个控件是不是在所在 WBP 的设计器里建的（名字存在资源里，稳定）。
	 * 不能只看 Outer：`CreateWidget(this, …)` 建的控件、列表条目的 Outer 也是父 WBP 的 WidgetTree。
	 * 所以查 WBP 生成类里的控件树原型（含父类的），名字在里面才算设计的。
	 */
	bool IsDesigned(const UWidget& InWidget)
	{
		const UUserWidget* Owner = GetTreeOwner(InWidget);
		if (Owner == nullptr)
		{
			return false;
		}
		for (const UClass* Class = Owner->GetClass(); Class != nullptr; Class = Class->GetSuperClass())
		{
			const UWidgetBlueprintGeneratedClass* Generated = Cast<UWidgetBlueprintGeneratedClass>(Class);
			const UWidgetTree* Archetype = Generated != nullptr ? Generated->GetWidgetTreeArchetype() : nullptr;
			if (Archetype != nullptr && Archetype->FindWidget(InWidget.GetFName()) != nullptr)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * 运行时生成、名字不稳定：名字就是「类名_序号」，又不是在 WBP 设计器里建的（CreateWidget、Lua 里 new 出来再 AddChild）。
	 * 设计器里的控件哪怕叫 `Button_42` 也是稳定的（存在资源里），照用名字。
	 */
	bool HasUnstableName(const UWidget& InWidget)
	{
		const FName Name = InWidget.GetFName();
		return Name.GetNumber() > 0
			&& Name.GetPlainNameString().Equals(InWidget.GetClass()->GetName(), ESearchCase::IgnoreCase)
			&& !IsDesigned(InWidget);
	}

	/** `InRoot` 是不是 `InOwner` 自己控件树的根：不是的话，它只是 Outer 挂在 Owner 下面（运行时建的弹窗、列表条目）。 */
	bool IsTreeRootOf(const UUserWidget* InOwner, const UWidget* InRoot)
	{
		return InOwner != nullptr && InOwner->WidgetTree != nullptr && InOwner->WidgetTree->RootWidget == InRoot;
	}

	/** 一个路径段：名字，名字不稳定时 `#类名#同类序号`（序号按父面板里的顺序，和快照一致）。 */
	FString SegmentFor(const UWidget& InWidget)
	{
		if (!HasUnstableName(InWidget))
		{
			return InWidget.GetFName().ToString();
		}

		const FString ClassName = GameTestSnapshot::FriendlyClassName(InWidget.GetClass());
		int32 SameClassIndex = 0;
		if (const UPanelWidget* Parent = InWidget.GetParent())
		{
			for (int32 Index = 0; Index < Parent->GetChildrenCount(); ++Index)
			{
				const UWidget* Sibling = Parent->GetChildAt(Index);
				if (Sibling == &InWidget)
				{
					break;
				}
				if (Sibling != nullptr && GameTestSnapshot::FriendlyClassName(Sibling->GetClass()) == ClassName)
				{
					++SameClassIndex;
				}
			}
		}
		return FString::Printf(TEXT("#%s#%d"), *ClassName, SameClassIndex);
	}

	/**
	 * 挂在容器 Slate 上的子界面的路径段：和快照的子节点顺序一致——容器的 UMG 子控件在前，挂上来的在后，
	 * `#类名#序号` 的序号在这两部分合起来的同类里数。名字稳定（不是「类名_序号」）时照用名字。
	 */
	FString AttachedSegment(const UWidget& InContainer, const UUserWidget& InWidget)
	{
		if (!HasUnstableName(InWidget))
		{
			return InWidget.GetFName().ToString();
		}

		const FString ClassName = GameTestSnapshot::FriendlyClassName(InWidget.GetClass());
		int32 SameClassIndex = 0;
		if (const UPanelWidget* Panel = Cast<UPanelWidget>(&InContainer))
		{
			for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
			{
				const UWidget* Child = Panel->GetChildAt(Index);
				if (Child != nullptr && GameTestSnapshot::FriendlyClassName(Child->GetClass()) == ClassName)
				{
					++SameClassIndex;
				}
			}
		}
		TArray<UUserWidget*> Attached;
		GameTestSlate::CollectAttachedUserWidgets(InContainer, Attached);
		for (const UUserWidget* Other : Attached)
		{
			if (Other == &InWidget)
			{
				break;
			}
			if (Other != nullptr && GameTestSnapshot::FriendlyClassName(Other->GetClass()) == ClassName)
			{
				++SameClassIndex;
			}
		}
		return FString::Printf(TEXT("#%s#%d"), *ClassName, SameClassIndex);
	}

	/** 控件 → 它所在控件树的根，以及从根到它的路径段。 */
	UWidget* WalkToRoot(UWidget& InWidget, TArray<FString>& OutSegments)
	{
		TArray<UWidget*> Chain;
		UWidget* Current = &InWidget;
		for (int32 Guard = 0; Current != nullptr && Guard < 256; ++Guard)
		{
			Chain.Add(Current);
			UWidget* Parent = Current->GetParent();
			if (Parent == nullptr)
			{
				break;
			}
			Current = Parent;
		}

		OutSegments.Reset();
		for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
		{
			OutSegments.Add(SegmentFor(*Chain[Index]));
		}
		return Chain.Last();
	}

	/** 列表条目 → 它在列表里的位置写法：唯一的 Id 类属性优先，没有就用序号。 */
	FString EntryBracket(UListView& InList, const UObject* InItem)
	{
		const TArray<UObject*>& Items = InList.GetListItems();
		const int32 Index = Items.IndexOfByKey(InItem);
		if (Index == INDEX_NONE)
		{
			return FString();
		}

		if (Items.Num() <= MaxItemsForProperty)
		{
			const TSharedPtr<FJsonObject> Mine = GameTestSnapshot::DumpProperties(InItem);
			if (Mine.IsValid())
			{
				for (const auto& Pair : Mine->Values)
				{
					const FString PairKey = GAMETEST_JSON_KEY(Pair.Key);
					const bool bIdLike = PairKey.Equals(TEXT("id"), ESearchCase::IgnoreCase) || PairKey.Equals(TEXT("key"), ESearchCase::IgnoreCase)
						|| PairKey.Equals(TEXT("guid"), ESearchCase::IgnoreCase) || PairKey.EndsWith(TEXT("Id"), ESearchCase::CaseSensitive)
						|| PairKey.EndsWith(TEXT("ID"), ESearchCase::CaseSensitive);
					if (!bIdLike || !Pair.Value.IsValid() || (Pair.Value->Type != EJson::String && Pair.Value->Type != EJson::Number))
					{
						continue;
					}

					// 值里有方括号、`>` 的写不进选择器（会被当成下一层），跳过。
					const FString Value = Pair.Value->AsString();
					if (Value.IsEmpty() || Value.Contains(TEXT("[")) || Value.Contains(TEXT("]")) || Value.Contains(TEXT(">")))
					{
						continue;
					}

					int32 SameValue = 0;
					for (const UObject* Other : Items)
					{
						const TSharedPtr<FJsonObject> OtherProps = GameTestSnapshot::DumpProperties(Other);
						const TSharedPtr<FJsonValue> OtherValue = OtherProps.IsValid() ? OtherProps->TryGetField(PairKey) : nullptr;
						if (OtherValue.IsValid() && OtherValue->AsString() == Value)
						{
							++SameValue;
						}
					}
					if (SameValue == 1)
					{
						return FString::Printf(TEXT("[%s=%s]"), *PairKey, *Value);
					}
				}
			}
		}
		return FString::Printf(TEXT("[%d]"), Index);
	}

	/** 列表条目控件 → 它所在的列表（不是条目时返回 nullptr）。 */
	UListView* GetOwningList(UUserWidget& InEntry, const UObject*& OutItem)
	{
		TScriptInterface<IUserListEntry> ListEntry;
		ListEntry.SetObject(&InEntry);
		UListView* List = Cast<UListView>(UUserListEntryLibrary::GetOwningListView(ListEntry));
		if (List == nullptr)
		{
			return nullptr;
		}
		TScriptInterface<IUserObjectListEntry> ObjectEntry;
		ObjectEntry.SetObject(&InEntry);
		OutItem = UUserObjectListEntryLibrary::GetListItemObject(ObjectEntry);
		return OutItem != nullptr ? List : nullptr;
	}

	/** 写出能解析到 `InWidget` 的选择器（递归：所在的 WBP 先写出来）。 */
	bool Describe(UWidget& InWidget, int32 InDepth, FString& OutTarget, FString& OutError)
	{
		if (InDepth > MaxDepth)
		{
			OutError = TEXT("WBP 嵌套太深（父子关系可能绕成了环）");
			return false;
		}

		TArray<FString> Segments;
		UWidget* Root = WalkToRoot(InWidget, Segments);
		UUserWidget* Owner = GetTreeOwner(*Root);
		if (!IsTreeRootOf(Owner, Root))
		{
			// Outer 挂在别的 WBP 下面，但不在它的控件树里（列表条目、CreateWidget(this) 建的弹窗）：当独立的 WBP 看。
			Owner = nullptr;
		}

		if (Owner == nullptr)
		{
			// 根不在任何 WBP 的控件树里：它自己是一个独立的 WBP——顶层屏，或者列表条目。
			UUserWidget* Standalone = Cast<UUserWidget>(Root);
			if (Standalone == nullptr || Root != &InWidget)
			{
				OutError = FString::Printf(TEXT("%s 不在任何 WBP 的控件树里，写不出选择器"), *InWidget.GetName());
				return false;
			}

			const UObject* Item = nullptr;
			if (UListView* List = GetOwningList(*Standalone, Item))
			{
				FString ListTarget;
				if (!Describe(*List, InDepth + 1, ListTarget, OutError))
				{
					return false;
				}
				const FString Bracket = EntryBracket(*List, Item);
				if (Bracket.IsEmpty())
				{
					OutError = TEXT("条目的数据项不在列表里（列表刚刷新？）");
					return false;
				}
				OutTarget = ListTarget + Bracket;
				return true;
			}

			// 没有 UMG 父面板、但显示在某个 UMG 控件的 Slate 里（DynamicEntryBox、自定义列表、Lua 挂上去的）：
			// 写成「容器/#类名#序号」，快照把它挂在同一个容器下面，同类的几个实例才分得开。
			if (UWidget* Container = GameTestSlate::FindAttachContainer(*Standalone))
			{
				FString ContainerTarget;
				if (!Describe(*Container, InDepth + 1, ContainerTarget, OutError))
				{
					return false;
				}
				OutTarget = ContainerTarget + TEXT("/") + AttachedSegment(*Container, *Standalone);
				return true;
			}

			OutTarget = GameTestSnapshot::FriendlyClassName(Standalone->GetClass()) + TEXT(":.");
			return true;
		}

		const FString Relative = FString::Join(Segments, TEXT("/"));
		if (GameTestSelectorWriter::IsTopLevel(*Owner))
		{
			OutTarget = GameTestSnapshot::FriendlyClassName(Owner->GetClass()) + TEXT(":") + Relative;
			return true;
		}

		// 所在的 WBP 是嵌套实例或列表条目：先写出它，再用 `>` 进去。
		FString OwnerTarget;
		if (!Describe(*Owner, InDepth + 1, OwnerTarget, OutError))
		{
			return false;
		}
		OutTarget = OwnerTarget + TEXT(">") + Relative;
		return true;
	}

	/** 用回放同一套解析核对：唯一、指回同一个控件。 */
	bool Verify(const FString& InTarget, const UWidget* InWidget, const TSharedPtr<SWidget>& InSlate, FString& OutError)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		GameTestSnapshot::CaptureScreens(GameTestWorld::FindGameWorld(), Screens, /*bInLight=*/true);
		TSharedPtr<FGameTestWidgetNode> Node;
		if (!GameTestSelector::ResolveText(Screens, InTarget, Node, OutError) || !Node.IsValid())
		{
			return false;
		}
		const bool bSame = InWidget != nullptr ? Node->Widget.Get() == InWidget : Node->Slate.Pin() == InSlate;
		if (!bSame)
		{
			OutError = FString::Printf(TEXT("解析到的是 %s，不是录到的那个控件"), *Node->Path);
		}
		return bSame;
	}

	/**
	 * 选择器命中同一个 WBP 的几个实例时，回放能不能按录制线索（位置、文字）挑回它：用回放同一套挑法（GameTestHintPick）当场试一次。
	 * 挑得回来就不算歧义，只在 hint 里记一句 `pick`；挑不回来才写 `ambiguous`。
	 */
	bool VerifyByHint(const FString& InTarget, const UWidget* InWidget, const TSharedPtr<SWidget>& InSlate, const FJsonObject& InHint)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		GameTestSnapshot::CaptureScreens(GameTestWorld::FindGameWorld(), Screens, /*bInLight=*/true);
		TArray<TSharedPtr<FGameTestWidgetNode>> Ambiguous;
		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		if (GameTestSelector::ResolveText(Screens, InTarget, Node, Error, &Ambiguous) || Ambiguous.Num() < 2)
		{
			return false;
		}
		FString Note;
		FString Why;
		if (!GameTestHintPick::Pick(Ambiguous, InHint, Node, Note, Why) || !Node.IsValid())
		{
			return false;
		}
		return InWidget != nullptr ? Node->Widget.Get() == InWidget : Node->Slate.Pin() == InSlate;
	}

	TSharedPtr<FJsonObject> MakeHint(const UWidget* InWidget, const TSharedPtr<SWidget>& InSlate)
	{
		const TSharedPtr<FJsonObject> Hint = MakeShared<FJsonObject>();
		if (InWidget != nullptr)
		{
			const UUserWidget* AssetOwner = Cast<UUserWidget>(InWidget);
			if (AssetOwner == nullptr)
			{
				AssetOwner = InWidget->GetTypedOuter<UUserWidget>();
			}
			if (AssetOwner != nullptr)
			{
				Hint->SetStringField(TEXT("asset"), GameTestSlate::GetAssetPath(*AssetOwner));
			}
			Hint->SetStringField(TEXT("class"), GameTestSnapshot::FriendlyClassName(InWidget->GetClass()));
		}
		else if (InSlate.IsValid())
		{
			Hint->SetStringField(TEXT("class"), InSlate->GetTypeAsString());
		}

		// 叠在按钮上的兄弟文字也算（CollectTextNear）：回放按文字找回时用同一个函数取字，两边一致
		const FString Text = GameTestSlate::CollectTextNear(InSlate, 40);
		if (!Text.IsEmpty())
		{
			Hint->SetStringField(TEXT("text"), Text);
		}

		// 在视口里的位置（归一化）：面板上显示；选择器命中多个时回放按它挑（GameTestHintPick）。
		FVector2D Min;
		FVector2D Size;
		if (InSlate.IsValid() && GameTestSlate::GetViewportRect(*InSlate, Min, Size))
		{
			TArray<TSharedPtr<FJsonValue>> Rect;
			Rect.Add(MakeShared<FJsonValueNumber>(Round3(Min.X)));
			Rect.Add(MakeShared<FJsonValueNumber>(Round3(Min.Y)));
			Rect.Add(MakeShared<FJsonValueNumber>(Round3(Size.X)));
			Rect.Add(MakeShared<FJsonValueNumber>(Round3(Size.Y)));
			Hint->SetArrayField(TEXT("rect"), Rect);
		}
		// 录制时视口的像素尺寸：回放时屏幕比例不同，按位置挑要放宽（GameTestHintPick）。
		if (const TSharedPtr<SWidget> Viewport = GameTestSlate::GetGameViewportWidget())
		{
			const FVector2D ViewSize = FVector2D(Viewport->GetCachedGeometry().GetAbsoluteSize());
			if (ViewSize.X >= 1.0 && ViewSize.Y >= 1.0)
			{
				TArray<TSharedPtr<FJsonValue>> Vp;
				Vp.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(ViewSize.X)));
				Vp.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(ViewSize.Y)));
				Hint->SetArrayField(TEXT("vp"), Vp);
			}
		}
		return Hint;
	}

	/** Slate 类型名去掉模板参数：`STableRow<TSharedPtr<FString>>` → `STableRow`。 */
	FString ShortSlateType(const SWidget& InWidget)
	{
		FString Type = InWidget.GetTypeAsString();
		int32 Angle = INDEX_NONE;
		if (Type.FindChar(TEXT('<'), Angle))
		{
			Type = Type.Left(Angle);
		}
		return Type;
	}
}

bool GameTestSelectorWriter::IsTopLevel(const UWidget& InWidget)
{
	// 顶层屏：没有父面板、不是哪个 WBP 控件树的根（嵌套实例）、也不是列表条目。
	// 不看 Outer：WBP 里 CreateWidget(this) 建的弹窗 Outer 是创建者的 WidgetTree，但它是独立的一屏。
	if (InWidget.GetParent() != nullptr)
	{
		return false;
	}
	const UUserWidget* UserWidget = Cast<UUserWidget>(&InWidget);
	if (UserWidget == nullptr || GameTestSelectorWriterDetail::IsTreeRootOf(GameTestSelectorWriterDetail::GetTreeOwner(InWidget), &InWidget))
	{
		return false;
	}
	const UObject* Item = nullptr;
	if (GameTestSelectorWriterDetail::GetOwningList(*const_cast<UUserWidget*>(UserWidget), Item) != nullptr)
	{
		return false;
	}
	// 挂在别的控件 Slate 上的子界面也不是顶层屏：要先写出它的容器。
	return GameTestSlate::FindAttachContainer(*UserWidget) == nullptr;
}

bool GameTestSelectorWriter::Write(UWidget& InWidget, FGameTestWrittenTarget& OutTarget, FString& OutError)
{
	OutTarget = FGameTestWrittenTarget();
	if (!GameTestSelectorWriterDetail::Describe(InWidget, 0, OutTarget.Target, OutError))
	{
		return false;
	}

	const TSharedPtr<SWidget> Slate = InWidget.GetCachedWidget();
	OutTarget.Hint = GameTestSelectorWriterDetail::MakeHint(&InWidget, Slate);

	FString VerifyError;
	OutTarget.bVerified = GameTestSelectorWriterDetail::Verify(OutTarget.Target, &InWidget, Slate, VerifyError);
	if (!OutTarget.bVerified)
	{
		if (GameTestSelectorWriterDetail::VerifyByHint(OutTarget.Target, &InWidget, Slate, *OutTarget.Hint))
		{
			OutTarget.Hint->SetStringField(TEXT("pick"), TEXT("选择器会命中同一个界面的几个实例，回放按录制时的位置和文字挑回这一个"));
		}
		else
		{
			OutTarget.Hint->SetStringField(TEXT("ambiguous"), VerifyError);
		}
	}
	return true;
}

bool GameTestSelectorWriter::WriteSlate(const TSharedRef<SWidget>& InWidget, FGameTestWrittenTarget& OutTarget, FString& OutError)
{
	OutTarget = FGameTestWrittenTarget();

	// 锚点：往上最近的 UMG 控件（下拉框本身）；弹出层在子窗口里时往上走不到 UMG，就不挂锚点。
	FString Anchor;
	if (UWidget* Owner = GameTestSlate::FindOwningWidget(InWidget->GetParentWidget()))
	{
		FString AnchorError;
		if (GameTestSelectorWriterDetail::Describe(*Owner, 0, Anchor, AnchorError))
		{
			Anchor += TEXT(">>");
		}
		else
		{
			Anchor = TEXT(">>");
		}
	}
	else
	{
		Anchor = TEXT(">>");
	}

	FString Segment = GameTestSelectorWriterDetail::ShortSlateType(*InWidget);
	const FString Text = GameTestSlate::CollectText(InWidget, 40);
	if (!Text.IsEmpty() && !Text.Contains(TEXT("[")) && !Text.Contains(TEXT("]")) && !Text.Contains(TEXT("/")))
	{
		Segment += FString::Printf(TEXT("[text=%s]"), *Text);
	}

	OutTarget.Hint = GameTestSelectorWriterDetail::MakeHint(nullptr, InWidget);

	// 先不带序号试；不唯一再从 0 往上加序号，直到指回同一个控件。
	FString VerifyError;
	OutTarget.Target = Anchor + Segment;
	OutTarget.bVerified = GameTestSelectorWriterDetail::Verify(OutTarget.Target, nullptr, InWidget, VerifyError);
	for (int32 Index = 0; !OutTarget.bVerified && Index < 20; ++Index)
	{
		const FString Candidate = FString::Printf(TEXT("%s%s[%d]"), *Anchor, *Segment, Index);
		FString CandidateError;
		if (GameTestSelectorWriterDetail::Verify(Candidate, nullptr, InWidget, CandidateError))
		{
			OutTarget.Target = Candidate;
			OutTarget.bVerified = true;
		}
	}
	if (!OutTarget.bVerified)
	{
		OutTarget.Hint->SetStringField(TEXT("ambiguous"), VerifyError);
	}
	return true;
}

bool GameTestSelectorWriter::ToLocalPoint(const SWidget& InWidget, const FVector2D& InScreenPosition, FVector2D& OutPoint)
{
	const FGeometry& Geometry = InWidget.GetCachedGeometry();
	const FVector2D Size = FVector2D(Geometry.GetLocalSize());
	if (Size.X < 1.0 || Size.Y < 1.0)
	{
		return false;
	}
	const FVector2D Local = FVector2D(Geometry.AbsoluteToLocal(InScreenPosition));
	OutPoint.X = GameTestSelectorWriterDetail::Round3(FMath::Clamp(Local.X / Size.X, 0.0, 1.0));
	OutPoint.Y = GameTestSelectorWriterDetail::Round3(FMath::Clamp(Local.Y / Size.Y, 0.0, 1.0));
	return true;
}
