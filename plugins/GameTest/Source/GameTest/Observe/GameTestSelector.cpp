// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestSelector.h"

#include "Observe/GameTestWidgetNode.h"

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

	FString PathPart = Text;

	// 屏名与路径的分隔符是第一个 ':'；'text=' 段里的等号不受影响。
	int32 ColonPosition = INDEX_NONE;
	if (Text.FindChar(TEXT(':'), ColonPosition))
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
		OutError = FString::Printf(TEXT("'%s' 只有屏名没有路径"), *InText);
		return false;
	}

	PathPart.ParseIntoArray(OutParsed.Segments, TEXT("/"), /*CullEmpty=*/true);
	if (OutParsed.Segments.Num() == 0)
	{
		OutError = FString::Printf(TEXT("'%s' 的路径为空"), *InText);
		return false;
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
		|| InScreenRoot.ClassName.Equals(InScreen, ESearchCase::IgnoreCase);
}

bool GameTestSelector::Resolve(
	const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens,
	const FParsed& InParsed,
	TSharedPtr<FGameTestWidgetNode>& OutNode,
	FString& OutError)
{
	if (InParsed.bHandle)
	{
		OutError = TEXT("句柄由调用方解析");
		return false;
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
				Names.Add(Screen->ClassName);
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
		if (WalkPath(Screen, InParsed.Segments, Match, LastError))
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
		OutError = FString::Printf(TEXT("'%s' 在 %d 个屏里都命中：%s（请在 target 里写屏名）"),
			*InParsed.Screen, Found.Num(), *DescribeCandidates(Found));
		return false;
	}

	OutNode = Found[0];
	return true;
}

bool GameTestSelector::ResolveText(
	const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens,
	const FString& InText,
	TSharedPtr<FGameTestWidgetNode>& OutNode,
	FString& OutError)
{
	FParsed Parsed;
	if (GameTestSelector::Parse(InText, Parsed, OutError))
	{
		if (Parsed.bHandle)
		{
			OutError = FString::Printf(TEXT("'%s' 是句柄引用，此处需要选择器"), *InText);
			return false;
		}
		return Resolve(InScreens, Parsed, OutNode, OutError);
	}

	return false;
}
