// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestSelector.h"

#include "Observe/GameTestItemMatch.h"
#include "Observe/GameTestSlate.h"
#include "Observe/GameTestWidgetNode.h"

#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/UserWidget.h"
#include "Components/ListView.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace
{
	/** 把若干节点拼成"候选：a、b、c"——解析失败时最有用的一句话。 */
	FString DescribeCandidates(const TArray<TSharedPtr<FGameTestWidgetNode>>& InNodes, int32 InLimit = 12)
	{
		TArray<FString> Paths;
		for (int32 Index = 0; Index < InNodes.Num() && Index < InLimit; ++Index)
		{
			if (InNodes[Index].IsValid())
			{
				Paths.Add(InNodes[Index]->Path);
			}
		}

		const FString More = InNodes.Num() > InLimit
			? FString::Printf(TEXT(" …（共 %d 个）"), InNodes.Num())
			: FString();
		return FString::Printf(TEXT("候选：%s%s"), *FString::Join(Paths, TEXT("、")), *More);
	}

	void CollectDescendants(const FGameTestWidgetNode& InNode, TArray<TSharedPtr<FGameTestWidgetNode>>& OutNodes)
	{
		for (const TSharedPtr<FGameTestWidgetNode>& Child : InNode.Children)
		{
			if (!Child.IsValid())
			{
				continue;
			}

			OutNodes.Add(Child);
			CollectDescendants(*Child, OutNodes);
		}
	}

	/** 一个路径段的匹配：0 个 → 失败（带候选），多个 → 歧义（带候选），恰好 1 个 → 成功。 */
	bool SelectChild(
		const FString& InSegment,
		const TSharedPtr<FGameTestWidgetNode>& InParent,
		TSharedPtr<FGameTestWidgetNode>& OutChild,
		FString& OutError)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Matches;

		if (InSegment == TEXT("*"))
		{
			Matches = InParent->Children;
		}
		else if (InSegment.StartsWith(TEXT("#")))
		{
			FString ClassName = InSegment.RightChop(1);
			int32 UseIndex = INDEX_NONE;

			int32 HashPosition = INDEX_NONE;
			if (ClassName.FindLastChar(TEXT('#'), HashPosition))
			{
				const FString IndexText = ClassName.RightChop(HashPosition + 1);
				if (IndexText.IsNumeric())
				{
					UseIndex = FCString::Atoi(*IndexText);
					ClassName = ClassName.Left(HashPosition);
				}
			}

			for (const TSharedPtr<FGameTestWidgetNode>& Child : InParent->Children)
			{
				if (Child.IsValid() && Child->ClassName.Equals(ClassName, ESearchCase::IgnoreCase))
				{
					Matches.Add(Child);
				}
			}

			if (UseIndex != INDEX_NONE)
			{
				if (Matches.IsValidIndex(UseIndex))
				{
					OutChild = Matches[UseIndex];
					return true;
				}

				OutError = FString::Printf(TEXT("段 '%s' 只有 %d 个匹配，取不到第 %d 个"),
					*InSegment, Matches.Num(), UseIndex);
				return false;
			}
		}
		else if (InSegment.StartsWith(TEXT("text=")))
		{
			const FString Needle = InSegment.RightChop(5);
			for (const TSharedPtr<FGameTestWidgetNode>& Child : InParent->Children)
			{
				if (Child.IsValid() && Child->Text.Contains(Needle, ESearchCase::IgnoreCase))
				{
					Matches.Add(Child);
				}
			}
		}
		else
		{
			for (const TSharedPtr<FGameTestWidgetNode>& Child : InParent->Children)
			{
				if (Child.IsValid() && Child->Name.Equals(InSegment, ESearchCase::IgnoreCase))
				{
					Matches.Add(Child);
				}
			}
		}

		if (Matches.Num() == 0)
		{
			OutError = FString::Printf(TEXT("段 '%s' 在 %s 下没有匹配；%s"),
				*InSegment, *InParent->Path, *DescribeCandidates(InParent->Children));
			return false;
		}

		if (Matches.Num() > 1)
		{
			OutError = FString::Printf(TEXT("段 '%s' 命中 %d 个，写法不够确定；%s（可用 #类名#序号 或写全路径）"),
				*InSegment, Matches.Num(), *DescribeCandidates(Matches));
			return false;
		}

		OutChild = Matches[0];
		return true;
	}
	/**
	 * 在某个屏根里走一遍路径。
	 *
	 * 用"候选前沿"模型而不是单链匹配：`..` 段把前沿展开成全部后代、**不要求唯一**
	 * （唯一性由下一段负责），其余段在每个前沿节点上按"该段必须唯一"的规则匹配。
	 * 这样 `Root/../Hint` 表示"Root 下面某处的 Hint"，只有当有两处才报歧义。
	 */
	bool WalkPath(
		const TSharedPtr<FGameTestWidgetNode>& InScreen,
		const TArray<FString>& InSegments,
		TSharedPtr<FGameTestWidgetNode>& OutNode,
		FString& OutError)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Frontier;
		Frontier.Add(InScreen);

		for (const FString& Segment : InSegments)
		{
			// `.`：停在当前节点（`屏名:.` 指屏本身——整屏重写了 OnMouseButtonDown 的 WBP 就是这样被点的）。
			if (Segment == TEXT("."))
			{
				continue;
			}

			if (Segment == TEXT(".."))
			{
				TArray<TSharedPtr<FGameTestWidgetNode>> Descendants;
				for (const TSharedPtr<FGameTestWidgetNode>& Node : Frontier)
				{
					if (Node.IsValid())
					{
						CollectDescendants(*Node, Descendants);
					}
				}

				if (Descendants.Num() == 0)
				{
					OutError = FString::Printf(TEXT("段 '..' 在 %s 下没有任何后代"), *InScreen->Path);
					return false;
				}

				Frontier = MoveTemp(Descendants);
				continue;
			}

			TArray<TSharedPtr<FGameTestWidgetNode>> Next;
			FString LastError;
			for (const TSharedPtr<FGameTestWidgetNode>& Node : Frontier)
			{
				TSharedPtr<FGameTestWidgetNode> Child;
				if (SelectChild(Segment, Node, Child, LastError))
				{
					Next.Add(Child);
				}
			}

			if (Next.Num() == 0)
			{
				OutError = LastError;
				return false;
			}

			if (Next.Num() > 1)
			{
				OutError = FString::Printf(TEXT("段 '%s' 在 %s 下命中 %d 个，写法不够确定；%s"),
					*Segment, *InScreen->Path, Next.Num(), *DescribeCandidates(Next));
				return false;
			}

			Frontier = MoveTemp(Next);
		}

		OutNode = Frontier[0];
		return true;
	}
}

namespace GameTestSelectorDetail
{
	/** 按 `>` / `>>` 切开（方括号里的不算）。返回 false 表示括号没配对。 */
	bool SplitHops(const FString& InText, TArray<FString>& OutHops, FString& OutSlatePart, bool& bOutHasSlate)
	{
		OutHops.Reset();
		OutSlatePart.Reset();
		bOutHasSlate = false;

		int32 Depth = 0;
		FString Current;
		for (int32 Index = 0; Index < InText.Len(); ++Index)
		{
			const TCHAR Char = InText[Index];
			if (Char == TEXT('['))
			{
				++Depth;
			}
			else if (Char == TEXT(']'))
			{
				--Depth;
				if (Depth < 0)
				{
					return false;
				}
			}
			else if (Char == TEXT('>') && Depth == 0)
			{
				OutHops.Add(Current.TrimStartAndEnd());
				Current.Reset();
				if (Index + 1 < InText.Len() && InText[Index + 1] == TEXT('>'))
				{
					bOutHasSlate = true;
					OutSlatePart = InText.RightChop(Index + 2).TrimStartAndEnd();
					return true;
				}
				continue;
			}
			Current.AppendChar(Char);
		}

		if (Depth != 0)
		{
			return false;
		}
		OutHops.Add(Current.TrimStartAndEnd());
		return true;
	}

	/** 按 `/` 切路径段（方括号里的 `/` 不切：`[Path=/Game/x]` 这类属性值）。 */
	void SplitSegments(const FString& InPath, TArray<FString>& OutSegments)
	{
		OutSegments.Reset();
		int32 Depth = 0;
		FString Current;
		for (const TCHAR Char : InPath)
		{
			if (Char == TEXT('['))
			{
				++Depth;
			}
			else if (Char == TEXT(']'))
			{
				--Depth;
			}
			else if (Char == TEXT('/') && Depth == 0)
			{
				if (!Current.TrimStartAndEnd().IsEmpty())
				{
					OutSegments.Add(Current.TrimStartAndEnd());
				}
				Current.Reset();
				continue;
			}
			Current.AppendChar(Char);
		}
		if (!Current.TrimStartAndEnd().IsEmpty())
		{
			OutSegments.Add(Current.TrimStartAndEnd());
		}
	}

	/** `名字[内容]` → 名字、内容；没有方括号返回 false。 */
	bool SplitBracket(const FString& InSegment, FString& OutBase, FString& OutInner)
	{
		int32 Open = INDEX_NONE;
		if (!InSegment.EndsWith(TEXT("]")) || !InSegment.FindChar(TEXT('['), Open) || Open <= 0)
		{
			return false;
		}
		OutBase = InSegment.Left(Open).TrimStartAndEnd();
		OutInner = InSegment.Mid(Open + 1, InSegment.Len() - Open - 2).TrimStartAndEnd();
		return true;
	}

	/** 某个 UUserWidget 实例对应的屏（快照把每个 UUserWidget 都单独采成一棵屏）。 */
	TSharedPtr<FGameTestWidgetNode> FindScreenOf(const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens, const UWidget* InWidget)
	{
		if (InWidget == nullptr)
		{
			return nullptr;
		}
		for (const TSharedPtr<FGameTestWidgetNode>& Screen : InScreens)
		{
			if (Screen.IsValid() && Screen->Widget.Get() == InWidget)
			{
				return Screen;
			}
		}
		return nullptr;
	}

	/** 列表节点 + `序号` 或 `属性=值` → 条目控件那棵屏。只认已经生成出来的条目（列表是虚拟化的）。 */
	bool ResolveListEntry(
		const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens,
		const TSharedPtr<FGameTestWidgetNode>& InList,
		const FString& InInner,
		TSharedPtr<FGameTestWidgetNode>& OutEntry,
		FString& OutError)
	{
		UListView* List = Cast<UListView>(InList->Widget.Get());
		if (List == nullptr)
		{
			OutError = FString::Printf(TEXT("%s 不是列表（ListView / TileView / TreeView），不能写 [%s]"), *InList->Path, *InInner);
			return false;
		}

		int32 Index = INDEX_NONE;
		FString Property;
		FString Value;
		if (InInner.IsNumeric())
		{
			Index = FCString::Atoi(*InInner);
		}
		else if (InInner.Split(TEXT("="), &Property, &Value))
		{
			Property.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			Index = GameTestItemMatch::FindIndexByProperty(InList->Items, Property, FJsonValueString(Value));
			if (Index == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("%s 里没有 %s=%s 的条目；%s"), *InList->Path, *Property, *Value,
					*GameTestItemMatch::DescribeProperty(InList->Items, Property, /*InLimit=*/10));
				return false;
			}
		}
		else
		{
			OutError = FString::Printf(TEXT("[%s] 看不懂：列表条目写 [序号] 或 [属性=值]"), *InInner);
			return false;
		}

		if (!InList->Items.IsValidIndex(Index) || !InList->Items[Index].IsValid())
		{
			OutError = FString::Printf(TEXT("%s 只有 %d 个条目，没有第 %d 个"), *InList->Path, InList->Items.Num(), Index);
			return false;
		}

		const UObject* Item = InList->Items[Index]->Item.Get();
		for (UUserWidget* Entry : List->GetDisplayedEntryWidgets())
		{
			if (Entry == nullptr)
			{
				continue;
			}
			TScriptInterface<IUserObjectListEntry> EntryInterface;
			EntryInterface.SetObject(Entry);
			if (UUserObjectListEntryLibrary::GetListItemObject(EntryInterface) != Item)
			{
				continue;
			}

			OutEntry = FindScreenOf(InScreens, Entry);
			if (!OutEntry.IsValid())
			{
				OutError = FString::Printf(TEXT("%s 第 %d 个条目的控件还没建出 Slate 控件（下一帧再看）"), *InList->Path, Index);
				return false;
			}
			return true;
		}

		OutError = FString::Printf(TEXT("%s 第 %d 个条目没生成（不在可视范围内）：先 scroll index=%d 把它滚出来"), *InList->Path, Index, Index);
		return false;
	}

	/**
	 * 带列表条目的路径：段里有 `名字[...]` 时，先用名字找到列表，再跳到条目控件那棵屏，后面的段在条目里继续走。
	 * 没有方括号的段交给原来的 WalkPath（逐段唯一）。
	 */
	bool WalkPathWithEntries(
		const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens,
		const TSharedPtr<FGameTestWidgetNode>& InStart,
		const TArray<FString>& InSegments,
		TSharedPtr<FGameTestWidgetNode>& OutNode,
		FString& OutError)
	{
		TSharedPtr<FGameTestWidgetNode> Current = InStart;
		TArray<FString> Pending;
		for (const FString& Segment : InSegments)
		{
			FString Base;
			FString Inner;
			if (!SplitBracket(Segment, Base, Inner))
			{
				Pending.Add(Segment);
				continue;
			}

			Pending.Add(Base);
			TSharedPtr<FGameTestWidgetNode> ListNode;
			if (!WalkPath(Current, Pending, ListNode, OutError))
			{
				return false;
			}
			Pending.Reset();
			if (!ResolveListEntry(InScreens, ListNode, Inner, Current, OutError))
			{
				return false;
			}
		}

		if (Pending.Num() == 0)
		{
			OutNode = Current;
			return true;
		}
		return WalkPath(Current, Pending, OutNode, OutError);
	}

	/** 一个 Slate 段：`类型`、`类型[text=子串]`、`类型[序号]`（可以叠，`STableRow[text=高][0]`）。 */
	struct FSlateSegment
	{
		FString Type;
		FString Text;
		int32 Index = INDEX_NONE;
	};

	bool ParseSlateSegment(const FString& InText, FSlateSegment& OutSegment, FString& OutError)
	{
		OutSegment = FSlateSegment();
		FString Rest = InText.TrimStartAndEnd();
		int32 Open = INDEX_NONE;
		OutSegment.Type = Rest.FindChar(TEXT('['), Open) ? Rest.Left(Open) : Rest;
		Rest = Open != INDEX_NONE ? Rest.RightChop(Open) : FString();

		while (!Rest.IsEmpty())
		{
			int32 Close = INDEX_NONE;
			if (!Rest.StartsWith(TEXT("[")) || !Rest.FindChar(TEXT(']'), Close))
			{
				OutError = FString::Printf(TEXT("Slate 段 '%s' 的方括号没写对"), *InText);
				return false;
			}
			const FString Inner = Rest.Mid(1, Close - 1).TrimStartAndEnd();
			Rest = Rest.RightChop(Close + 1).TrimStartAndEnd();
			if (Inner.StartsWith(TEXT("text=")))
			{
				OutSegment.Text = Inner.RightChop(5);
			}
			else if (Inner.IsNumeric())
			{
				OutSegment.Index = FCString::Atoi(*Inner);
			}
			else
			{
				OutError = FString::Printf(TEXT("Slate 段 '%s' 里的 [%s] 看不懂（只认 [text=子串] 和 [序号]）"), *InText, *Inner);
				return false;
			}
		}

		if (OutSegment.Type.IsEmpty() && OutSegment.Text.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Slate 段 '%s' 既没有类型也没有 text"), *InText);
			return false;
		}
		return true;
	}

	/** 类型名匹配：`STableRow` 也认 `STableRow<TSharedPtr<FString>>`（模板控件的类型名带参数）。 */
	bool MatchSlateType(const FString& InWanted, const SWidget& InWidget)
	{
		if (InWanted.IsEmpty() || InWanted == TEXT("*"))
		{
			return true;
		}
		const FString Type = InWidget.GetTypeAsString();
		return Type.Equals(InWanted, ESearchCase::IgnoreCase) || Type.StartsWith(InWanted + TEXT("<"), ESearchCase::IgnoreCase);
	}

	void CollectSlateDescendants(const TSharedRef<SWidget>& InRoot, TArray<TSharedRef<SWidget>>& OutWidgets, int32 InDepth)
	{
		if (InDepth > 64)
		{
			return;
		}
		FChildren* Children = InRoot->GetChildren();
		if (Children == nullptr)
		{
			return;
		}
		for (int32 Index = 0; Index < Children->Num(); ++Index)
		{
			const TSharedRef<SWidget> Child = Children->GetChildAt(Index);
			if (!Child->GetVisibility().IsVisible())
			{
				continue;
			}
			OutWidgets.Add(Child);
			CollectSlateDescendants(Child, OutWidgets, InDepth + 1);
		}
	}

	TSharedPtr<FGameTestWidgetNode> MakeSlateNode(const TSharedRef<SWidget>& InWidget, const FString& InPath)
	{
		const TSharedPtr<FGameTestWidgetNode> Node = MakeShared<FGameTestWidgetNode>();
		Node->Name = InWidget->GetTypeAsString();
		Node->ClassName = Node->Name;
		Node->Path = InPath;
		Node->Text = GameTestSlate::CollectText(InWidget);
		Node->Visibility = InWidget->GetVisibility().IsVisible() ? TEXT("Visible") : TEXT("Collapsed");
		Node->bEnabled = InWidget->IsEnabled();
		Node->Slate = InWidget;

		const FGeometry& Geometry = InWidget->GetCachedGeometry();
		const UE::Slate::FDeprecateVector2DResult Position = Geometry.GetAbsolutePosition();
		const UE::Slate::FDeprecateVector2DResult Size = Geometry.GetAbsoluteSize();
		Node->Position = FVector2D(Position.X, Position.Y);
		Node->Size = FVector2D(Size.X, Size.Y);
		Node->bHasGeometry = Node->Size.X >= 1.0 && Node->Size.Y >= 1.0;
		return Node;
	}

	/** 在若干 Slate 根下按段找；每段在上一段命中的任意深度里找，最后必须唯一。 */
	bool ResolveSlate(
		const TArray<TSharedRef<SWidget>>& InRoots,
		const FString& InAnchorPath,
		const TArray<FString>& InSegments,
		TSharedPtr<FGameTestWidgetNode>& OutNode,
		FString& OutError)
	{
		TArray<TSharedRef<SWidget>> Frontier = InRoots;
		for (const FString& SegmentText : InSegments)
		{
			FSlateSegment Segment;
			if (!ParseSlateSegment(SegmentText, Segment, OutError))
			{
				return false;
			}

			TArray<TSharedRef<SWidget>> Matches;
			for (const TSharedRef<SWidget>& Root : Frontier)
			{
				TArray<TSharedRef<SWidget>> Descendants;
				CollectSlateDescendants(Root, Descendants, 0);
				for (const TSharedRef<SWidget>& Widget : Descendants)
				{
					if (MatchSlateType(Segment.Type, *Widget)
						&& (Segment.Text.IsEmpty() || GameTestSlate::CollectText(Widget).Contains(Segment.Text, ESearchCase::IgnoreCase)))
					{
						Matches.AddUnique(Widget);
					}
				}
			}

			if (Segment.Index != INDEX_NONE)
			{
				if (!Matches.IsValidIndex(Segment.Index))
				{
					OutError = FString::Printf(TEXT("Slate 段 '%s' 只命中 %d 个，取不到第 %d 个"), *SegmentText, Matches.Num(), Segment.Index);
					return false;
				}
				const TSharedRef<SWidget> Picked = Matches[Segment.Index];
				Matches.Reset();
				Matches.Add(Picked);
			}

			if (Matches.Num() == 0)
			{
				OutError = FString::Printf(TEXT("Slate 段 '%s' 在 %s 下没有命中（弹出层打开了吗）"), *SegmentText, *InAnchorPath);
				return false;
			}
			Frontier = MoveTemp(Matches);
		}

		if (Frontier.Num() > 1)
		{
			TArray<FString> Names;
			for (int32 Index = 0; Index < Frontier.Num() && Index < 8; ++Index)
			{
				Names.Add(FString::Printf(TEXT("%s「%s」"), *Frontier[Index]->GetTypeAsString(), *GameTestSlate::CollectText(Frontier[Index], 20)));
			}
			OutError = FString::Printf(TEXT("Slate 段命中 %d 个，写法不够确定：%s（加 [text=子串] 或 [序号]）"),
				Frontier.Num(), *FString::Join(Names, TEXT("、")));
			return false;
		}

		OutNode = MakeSlateNode(Frontier[0], FString::Printf(TEXT("%s>>%s"), *InAnchorPath, *FString::Join(InSegments, TEXT("/"))));
		return true;
	}

	/** 不挂在 UMG 上的 Slate 目标：在游戏窗口和它弹出的子窗口里找。 */
	void CollectGameWindowRoots(TArray<TSharedRef<SWidget>>& OutRoots)
	{
		// 根是游戏视口（UMG 和它的弹出层都挂在下面）加游戏窗口弹出的子窗口；不拿游戏窗口本身：
		// PIE 在关卡视口里时它是编辑器主窗口，整个编辑器都会被搜进来。
		const TSharedPtr<SWindow> GameWindow = GameTestSlate::GetGameWindow();
		const TSharedPtr<SWidget> Viewport = GameTestSlate::GetGameViewportWidget();
		if (!GameWindow.IsValid() || !Viewport.IsValid())
		{
			return;
		}
		OutRoots.Add(Viewport.ToSharedRef());
		TArray<TSharedRef<SWindow>> Pending = GameWindow->GetChildWindows();
		while (Pending.Num() > 0)
		{
			const TSharedRef<SWindow> Window = Pending.Pop();
			OutRoots.Add(Window);
			Pending.Append(Window->GetChildWindows());
		}
	}

	/** 屏名写的是资源路径（`/Game/UI/WBP_Main`，也认带 `.WBP_Main` 的对象路径）。 */
	bool MatchAssetPath(const FString& InScreen, const FGameTestWidgetNode& InScreenRoot)
	{
		const UWidget* Widget = InScreenRoot.Widget.Get();
		if (Widget == nullptr || !InScreen.StartsWith(TEXT("/")))
		{
			return false;
		}
		FString Package = InScreen;
		int32 Dot = INDEX_NONE;
		if (Package.FindLastChar(TEXT('.'), Dot))
		{
			Package = Package.Left(Dot);
		}
		return GameTestSlate::GetAssetPath(*Widget).Equals(Package, ESearchCase::IgnoreCase);
	}
}

bool GameTestSelector::Parse(const FString& InText, FParsed& OutParsed, FString& OutError)
{
	OutParsed = FParsed();

	FString Text = InText;
	Text.TrimStartAndEndInline();

	if (Text.IsEmpty())
	{
		OutError = TEXT("target 为空");
		return false;
	}

	if (Text.StartsWith(TEXT("@")))
	{
		OutParsed.bHandle = true;
		OutParsed.Handle = Text.RightChop(1);
		if (OutParsed.Handle.IsEmpty())
		{
			OutError = TEXT("'@' 后面没有句柄名");
			return false;
		}
		return true;
	}

	// `>>` 开头：纯 Slate 目标，不经 UMG。
	if (Text.StartsWith(TEXT(">>")))
	{
		OutParsed.bSlateOnly = true;
		GameTestSelectorDetail::SplitSegments(Text.RightChop(2), OutParsed.SlateSegments);
		if (OutParsed.SlateSegments.Num() == 0)
		{
			OutError = TEXT("'>>' 后面没有 Slate 段");
			return false;
		}
		return true;
	}

	FString PathPart = Text;

	// 屏名与路径的分隔符是第一个 ':'——但只看 '[', '>' 和 'text=' 之前的部分，属性值和文字里的冒号不算。
	int32 Limit = Text.Len();
	for (const TCHAR* Stop : { TEXT("["), TEXT(">"), TEXT("text=") })
	{
		const int32 Found = Text.Find(Stop);
		if (Found != INDEX_NONE)
		{
			Limit = FMath::Min(Limit, Found);
		}
	}
	int32 ColonPosition = INDEX_NONE;
	if (Text.FindChar(TEXT(':'), ColonPosition) && ColonPosition < Limit)
	{
		FString ScreenPart = Text.Left(ColonPosition);
		PathPart = Text.RightChop(ColonPosition + 1);
		ScreenPart.TrimStartAndEndInline();

		if (ScreenPart.IsEmpty() || ScreenPart == TEXT("*"))
		{
			OutParsed.bAnyScreen = true;
		}
		else
		{
			OutParsed.Screen = ScreenPart;
		}
	}
	else
	{
		OutParsed.bAnyScreen = true;
	}

	PathPart.TrimStartAndEndInline();
	if (PathPart.IsEmpty())
	{
		OutError = FString::Printf(TEXT("'%s' 只有屏名没有路径（要指屏本身写 '屏名:.'）"), *InText);
		return false;
	}

	TArray<FString> Hops;
	FString SlatePart;
	bool bHasSlate = false;
	if (!GameTestSelectorDetail::SplitHops(PathPart, Hops, SlatePart, bHasSlate))
	{
		OutError = FString::Printf(TEXT("'%s' 的方括号没配对"), *InText);
		return false;
	}

	for (int32 Index = 0; Index < Hops.Num(); ++Index)
	{
		TArray<FString> Segments;
		GameTestSelectorDetail::SplitSegments(Hops[Index], Segments);
		if (Segments.Num() == 0)
		{
			OutError = Index == 0
				? FString::Printf(TEXT("'%s' 的路径为空"), *InText)
				: FString::Printf(TEXT("'%s' 第 %d 个 '>' 后面没有路径"), *InText, Index);
			return false;
		}
		if (Index == 0)
		{
			OutParsed.Segments = MoveTemp(Segments);
		}
		else
		{
			OutParsed.NestedHops.Add(MoveTemp(Segments));
		}
	}

	if (bHasSlate)
	{
		GameTestSelectorDetail::SplitSegments(SlatePart, OutParsed.SlateSegments);
		if (OutParsed.SlateSegments.Num() == 0)
		{
			OutError = FString::Printf(TEXT("'%s' 的 '>>' 后面没有 Slate 段"), *InText);
			return false;
		}
	}

	return true;
}

bool GameTestSelector::MatchScreen(const FString& InScreen, const FGameTestWidgetNode& InScreenRoot)
{
	if (InScreen.IsEmpty() || InScreen == TEXT("*"))
	{
		return true;
	}

	return InScreenRoot.Name.Equals(InScreen, ESearchCase::IgnoreCase)
		|| InScreenRoot.ClassName.Equals(InScreen, ESearchCase::IgnoreCase)
		|| GameTestSelectorDetail::MatchAssetPath(InScreen, InScreenRoot);
}

bool GameTestSelector::Resolve(
	const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens,
	const FParsed& InParsed,
	TSharedPtr<FGameTestWidgetNode>& OutNode,
	FString& OutError,
	TArray<TSharedPtr<FGameTestWidgetNode>>* OutAmbiguous)
{
	if (OutAmbiguous != nullptr)
	{
		OutAmbiguous->Reset();
	}
	if (InParsed.bHandle)
	{
		OutError = TEXT("句柄由调用方解析");
		return false;
	}

	if (InParsed.bSlateOnly)
	{
		TArray<TSharedRef<SWidget>> Roots;
		GameTestSelectorDetail::CollectGameWindowRoots(Roots);
		if (Roots.Num() == 0)
		{
			OutError = TEXT("没有游戏窗口（先 pie.start）");
			return false;
		}
		return GameTestSelectorDetail::ResolveSlate(Roots, TEXT("（游戏窗口）"), InParsed.SlateSegments, OutNode, OutError);
	}

	TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
	for (const TSharedPtr<FGameTestWidgetNode>& Screen : InScreens)
	{
		if (Screen.IsValid() && MatchScreen(InParsed.Screen, *Screen))
		{
			Screens.Add(Screen);
		}
	}

	if (Screens.Num() == 0)
	{
		TArray<FString> Names;
		for (const TSharedPtr<FGameTestWidgetNode>& Screen : InScreens)
		{
			if (Screen.IsValid())
			{
				Names.AddUnique(Screen->ClassName);
			}
		}
		OutError = FString::Printf(TEXT("找不到屏 '%s'；当前存活的屏：%s"),
			*InParsed.Screen, Names.Num() > 0 ? *FString::Join(Names, TEXT("、")) : TEXT("（一个都没有）"));
		return false;
	}

	TArray<TSharedPtr<FGameTestWidgetNode>> Found;
	FString LastError;

	for (const TSharedPtr<FGameTestWidgetNode>& Screen : Screens)
	{
		TSharedPtr<FGameTestWidgetNode> Match;
		if (!GameTestSelectorDetail::WalkPathWithEntries(InScreens, Screen, InParsed.Segments, Match, LastError))
		{
			continue;
		}

		// `>`：前一段必须落在嵌套的 WBP 实例上，后一段从那个实例自己的控件树根开始走。
		bool bHopsOk = true;
		for (const TArray<FString>& Hop : InParsed.NestedHops)
		{
			const TSharedPtr<FGameTestWidgetNode> Instance = Match->bIsUserWidget && Match->Children.Num() > 0
				? Match
				: GameTestSelectorDetail::FindScreenOf(InScreens, Match->Widget.Get());
			if (!Instance.IsValid() || !Instance->Widget.IsValid() || !Instance->Widget->IsA<UUserWidget>())
			{
				LastError = FString::Printf(TEXT("'>' 前面的 %s 不是嵌套的 WBP 实例（是 %s），进不去"), *Match->Path, *Match->ClassName);
				bHopsOk = false;
				break;
			}
			TSharedPtr<FGameTestWidgetNode> Next;
			if (!GameTestSelectorDetail::WalkPathWithEntries(InScreens, Instance, Hop, Next, LastError))
			{
				bHopsOk = false;
				break;
			}
			Match = Next;
		}
		if (bHopsOk)
		{
			Found.Add(Match);
		}
	}

	if (Found.Num() == 0)
	{
		OutError = FString::Printf(TEXT("'%s' 没解析到控件：%s"), *InParsed.Screen, *LastError);
		return false;
	}

	if (Found.Num() > 1)
	{
		OutError = FString::Printf(TEXT("'%s' 在 %d 个屏里都命中：%s（请在 target 里写屏名，或用 '>' 写出父界面）"),
			*InParsed.Screen, Found.Num(), *DescribeCandidates(Found));
		// 后面还有 `>>` 段时每个候选还要再往 Slate 里找，交出去的就不是最终控件了：只在没有 `>>` 时交候选。
		if (OutAmbiguous != nullptr && InParsed.SlateSegments.Num() == 0)
		{
			*OutAmbiguous = Found;
		}
		return false;
	}

	OutNode = Found[0];
	if (InParsed.SlateSegments.Num() == 0)
	{
		return true;
	}

	// `>>`：在 UMG 锚点的 Slate 子树里找；锚点弹出的菜单在子窗口里，也一起找。
	const UWidget* Anchor = OutNode->Widget.Get();
	const TSharedPtr<SWidget> AnchorSlate = Anchor != nullptr ? Anchor->GetCachedWidget() : nullptr;
	if (!AnchorSlate.IsValid())
	{
		OutError = FString::Printf(TEXT("%s 还没有 Slate 控件，'>>' 无处可找"), *OutNode->Path);
		return false;
	}
	TArray<TSharedRef<SWidget>> Roots;
	Roots.Add(AnchorSlate.ToSharedRef());
	if (const TSharedPtr<SWindow> GameWindow = GameTestSlate::GetGameWindow())
	{
		for (const TSharedRef<SWindow>& Child : GameWindow->GetChildWindows())
		{
			Roots.Add(Child);
		}
	}
	return GameTestSelectorDetail::ResolveSlate(Roots, OutNode->Path, InParsed.SlateSegments, OutNode, OutError);
}

bool GameTestSelector::ResolveText(
	const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens,
	const FString& InText,
	TSharedPtr<FGameTestWidgetNode>& OutNode,
	FString& OutError,
	TArray<TSharedPtr<FGameTestWidgetNode>>* OutAmbiguous)
{
	FParsed Parsed;
	if (GameTestSelector::Parse(InText, Parsed, OutError))
	{
		if (Parsed.bHandle)
		{
			OutError = FString::Printf(TEXT("'%s' 是句柄引用，此处需要选择器"), *InText);
			return false;
		}
		return Resolve(InScreens, Parsed, OutNode, OutError, OutAmbiguous);
	}

	return false;
}
