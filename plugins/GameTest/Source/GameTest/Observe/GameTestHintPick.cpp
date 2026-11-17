// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestHintPick.h"

#include "Core/GameTestJson.h"
#include "Observe/GameTestSelector.h"
#include "Observe/GameTestSlate.h"
#include "Observe/GameTestTextMatch.h"
#include "Observe/GameTestWidgetNode.h"

#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Widgets/SWidget.h"

namespace GameTestHintPickDetail
{
	/** 位置差（视口归一化：中心差 + 一半的尺寸差）超过这个就不算同一个：界面换了布局，挑了也多半是错的。 */
	constexpr double MaxRectDistance = 0.08;

	/** 最近的和第二近的至少差这么多才挑：两个都差不多近时说明线索分不开，宁可报错。 */
	constexpr double MinRectMargin = 0.02;

	/** 录制和回放的视口宽高比差超过这么多，算换了屏幕比例（16:9 录、4:3 或带刘海的屏上放）。 */
	constexpr double AspectTolerance = 0.03;

	/** 比例变了时位置只比中心，允许的偏差放宽到这么多：锚在边上的控件会整体挪一截。 */
	constexpr double MaxRectDistanceAspectChanged = 0.15;

	/** 录制时的视口像素 `hint.vp: [宽, 高]`；老录制没有。 */
	bool ReadViewport(const FJsonObject& InHint, FVector2D& OutSize)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = GameTestJson::TryGetArray(InHint, TEXT("vp"));
		if (Values == nullptr || Values->Num() != 2 || !(*Values)[0].IsValid() || !(*Values)[1].IsValid())
		{
			return false;
		}
		OutSize = FVector2D((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber());
		return OutSize.X >= 1.0 && OutSize.Y >= 1.0;
	}

	/** 此刻游戏视口的像素尺寸。 */
	bool CurrentViewport(FVector2D& OutSize)
	{
		const TSharedPtr<SWidget> Viewport = GameTestSlate::GetGameViewportWidget();
		if (!Viewport.IsValid())
		{
			return false;
		}
		OutSize = FVector2D(Viewport->GetCachedGeometry().GetAbsoluteSize());
		return OutSize.X >= 1.0 && OutSize.Y >= 1.0;
	}

	/** 录制时和现在的宽高比差得多不多（没录视口、取不到视口都当没变）。 */
	bool AspectChanged(const FJsonObject& InHint)
	{
		FVector2D Recorded;
		FVector2D Now;
		if (!ReadViewport(InHint, Recorded) || !CurrentViewport(Now))
		{
			return false;
		}
		const double Want = Recorded.X / Recorded.Y;
		const double Have = Now.X / Now.Y;
		return FMath::Abs(Have - Want) / Want > AspectTolerance;
	}

	struct FCandidate
	{
		TSharedPtr<FGameTestWidgetNode> Node;
		TSharedPtr<SWidget> Slate;
		FVector2D Min = FVector2D::ZeroVector;
		FVector2D Size = FVector2D::ZeroVector;
		double Distance = 0.0;
	};

	TSharedPtr<SWidget> SlateOfNode(const FGameTestWidgetNode& InNode)
	{
		if (const TSharedPtr<SWidget> Slate = InNode.Slate.Pin())
		{
			return Slate;
		}
		const UWidget* Widget = InNode.Widget.Get();
		return Widget != nullptr ? Widget->GetCachedWidget() : nullptr;
	}

	/** 真在屏上：在游戏界面里、整条父链可见、有尺寸。对象池里藏着的实例往往也有 Slate 控件，只是不在视口下面。 */
	bool IsOnScreen(const TSharedPtr<SWidget>& InSlate)
	{
		if (!InSlate.IsValid() || !GameTestSlate::IsInGameUi(InSlate))
		{
			return false;
		}
		for (TSharedPtr<SWidget> Current = InSlate; Current.IsValid(); Current = Current->GetParentWidget())
		{
			if (!Current->GetVisibility().IsVisible())
			{
				return false;
			}
		}
		const FVector2D Size = FVector2D(InSlate->GetCachedGeometry().GetLocalSize());
		return Size.X >= 1.0 && Size.Y >= 1.0;
	}

	bool ReadRect(const FJsonObject& InHint, FVector2D& OutMin, FVector2D& OutSize)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = GameTestJson::TryGetArray(InHint, TEXT("rect"));
		if (Values == nullptr || Values->Num() != 4)
		{
			return false;
		}
		double Numbers[4] = { 0.0, 0.0, 0.0, 0.0 };
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
			if (!Value.IsValid() || Value->Type != EJson::Number)
			{
				return false;
			}
			Numbers[Index] = Value->AsNumber();
		}
		OutMin = FVector2D(Numbers[0], Numbers[1]);
		OutSize = FVector2D(Numbers[2], Numbers[3]);
		return true;
	}

	FString DescribePick(const FCandidate& InCandidate)
	{
		return FString::Printf(TEXT("%s（位置 %.3f, %.3f）"), *InCandidate.Node->Path, InCandidate.Min.X, InCandidate.Min.Y);
	}

	/**
	 * 同一个容器里最多试这么多个序号：历史记录、GM 按钮这类容器一般十几个；
	 * 自动等待每帧都可能走到这里（只在文字对不上时），试太多会拖慢回放。
	 */
	constexpr int32 MaxSiblingTries = 48;

	/** 选择器里按位置写的一段：`#类名#序号` 或 `列表[序号]`。`Hop` 为 -1 表示第一段路径，否则是第几个 `>` 之后。 */
	struct FPositional
	{
		int32 Hop = INDEX_NONE;
		int32 Segment = INDEX_NONE;
		FString Prefix;
		FString Suffix;
		int32 Index = 0;
	};

	bool IsDigits(const FString& InText)
	{
		if (InText.IsEmpty() || InText.Len() > 6)
		{
			return false;
		}
		for (const TCHAR Char : InText)
		{
			if (!FChar::IsDigit(Char))
			{
				return false;
			}
		}
		return true;
	}

	/** `#类名#3` → 前缀 `#类名#`、序号 3；`List[3]` → 前缀 `List[`、后缀 `]`、序号 3。`List[Id=7]`、`#类名` 不算。 */
	bool ParsePositional(const FString& InSegment, FString& OutPrefix, FString& OutSuffix, int32& OutIndex)
	{
		if (InSegment.StartsWith(TEXT("#")))
		{
			int32 Hash = INDEX_NONE;
			if (!InSegment.FindLastChar(TEXT('#'), Hash) || Hash <= 0)
			{
				return false;
			}
			const FString Number = InSegment.RightChop(Hash + 1);
			if (!IsDigits(Number))
			{
				return false;
			}
			OutPrefix = InSegment.Left(Hash + 1);
			OutSuffix.Reset();
			OutIndex = FCString::Atoi(*Number);
			return true;
		}
		if (InSegment.EndsWith(TEXT("]")))
		{
			int32 Open = INDEX_NONE;
			if (!InSegment.FindLastChar(TEXT('['), Open) || Open <= 0)
			{
				return false;
			}
			const FString Number = InSegment.Mid(Open + 1, InSegment.Len() - Open - 2);
			if (!IsDigits(Number))
			{
				return false;
			}
			OutPrefix = InSegment.Left(Open + 1);
			OutSuffix = TEXT("]");
			OutIndex = FCString::Atoi(*Number);
			return true;
		}
		return false;
	}

	TArray<FString>& SegmentsOf(GameTestSelector::FParsed& InParsed, int32 InHop)
	{
		return InHop < 0 ? InParsed.Segments : InParsed.NestedHops[InHop];
	}

	/** 按出现顺序列出所有按位置写的段（调用方从后往前试：越靠近目标的容器越可能是顺序变了的那个）。 */
	void CollectPositionals(const GameTestSelector::FParsed& InParsed, TArray<FPositional>& OutPositionals)
	{
		const auto AddFrom = [&OutPositionals](const TArray<FString>& InSegments, int32 InHop)
		{
			for (int32 Index = 0; Index < InSegments.Num(); ++Index)
			{
				FPositional Positional;
				if (ParsePositional(InSegments[Index], Positional.Prefix, Positional.Suffix, Positional.Index))
				{
					Positional.Hop = InHop;
					Positional.Segment = Index;
					OutPositionals.Add(MoveTemp(Positional));
				}
			}
		};
		AddFrom(InParsed.Segments, INDEX_NONE);
		for (int32 Hop = 0; Hop < InParsed.NestedHops.Num(); ++Hop)
		{
			AddFrom(InParsed.NestedHops[Hop], Hop);
		}
	}

	FString TextOf(const FGameTestWidgetNode& InNode)
	{
		return GameTestSlate::CollectTextNear(SlateOfNode(InNode), 40);
	}
}

const FJsonObject* GameTestHintPick::GetHint(const TSharedPtr<FJsonObject>& InStepParams)
{
	if (!InStepParams.IsValid())
	{
		return nullptr;
	}
	const TSharedPtr<FJsonObject> Hint = GameTestJson::TryGetObject(*InStepParams, TEXT("hint"));
	// 指向步骤 JSON 里那份对象：步骤活着它就活着，调用方只在这一步里用。
	return Hint.Get();
}

bool GameTestHintPick::Pick(
	const TArray<TSharedPtr<FGameTestWidgetNode>>& InCandidates,
	const FJsonObject& InHint,
	TSharedPtr<FGameTestWidgetNode>& OutNode,
	FString& OutNote,
	FString& OutWhy)
{
	using namespace GameTestHintPickDetail;

	// 1. 只留真在屏上的。
	TArray<FCandidate> OnScreen;
	for (const TSharedPtr<FGameTestWidgetNode>& Node : InCandidates)
	{
		if (!Node.IsValid())
		{
			continue;
		}
		FCandidate Candidate;
		Candidate.Node = Node;
		Candidate.Slate = SlateOfNode(*Node);
		if (!IsOnScreen(Candidate.Slate))
		{
			continue;
		}
		GameTestSlate::GetViewportRect(*Candidate.Slate, Candidate.Min, Candidate.Size);
		OnScreen.Add(MoveTemp(Candidate));
	}

	if (OnScreen.Num() == 0)
	{
		OutWhy = FString::Printf(TEXT("%d 个候选都不在屏上（界面还没打开，或都被隐藏了）"), InCandidates.Num());
		return false;
	}
	if (OnScreen.Num() == 1)
	{
		OutNode = OnScreen[0].Node;
		OutNote = FString::Printf(TEXT("选择器命中 %d 个，只有 %s 在屏上，点它"), InCandidates.Num(), *OnScreen[0].Node->Path);
		return true;
	}

	// 2. 按文字挑：完全一样 > 忽略数字和标点后一样 > 包含（GameTestTextMatch），只留最高那一档；
	//    一个都对不上（换了语言）就不按文字筛。
	FString Text;
	if (GameTestJson::TryGetString(InHint, TEXT("text"), Text) && !Text.IsEmpty())
	{
		TArray<FString> Texts;
		for (const FCandidate& Candidate : OnScreen)
		{
			Texts.Add(GameTestSlate::CollectTextNear(Candidate.Slate, 40));
		}
		TArray<int32> Hits;
		const GameTestTextMatch::ETier Tier = GameTestTextMatch::FilterBest(Text, Texts, Hits);
		if (Hits.Num() == 1)
		{
			OutNode = OnScreen[Hits[0]].Node;
			OutNote = FString::Printf(TEXT("选择器命中 %d 个，按录制时的文字「%s」挑了 %s（%s）"), InCandidates.Num(), *Text,
				*OutNode->Path, GameTestTextMatch::Describe(Tier));
			return true;
		}
		if (Hits.Num() > 1)
		{
			TArray<FCandidate> Kept;
			for (const int32 Hit : Hits)
			{
				Kept.Add(OnScreen[Hit]);
			}
			OnScreen = MoveTemp(Kept);
		}
	}

	// 3. 按录制时的位置挑最近的。
	FVector2D WantMin;
	FVector2D WantSize;
	if (!ReadRect(InHint, WantMin, WantSize))
	{
		OutWhy = FString::Printf(TEXT("屏上有 %d 个候选，录制线索里没有位置（rect），分不出是哪一个"), OnScreen.Num());
		return false;
	}
	// 屏幕比例变了（录制带了 vp 才知道）：归一化坐标会整体挪，尺寸也跟着比例变，只比中心、放宽偏差。
	const bool bAspectChanged = AspectChanged(InHint);
	const double MaxDistance = bAspectChanged ? MaxRectDistanceAspectChanged : MaxRectDistance;
	const FVector2D WantCenter = WantMin + WantSize * 0.5;
	for (FCandidate& Candidate : OnScreen)
	{
		const FVector2D Center = Candidate.Min + Candidate.Size * 0.5;
		Candidate.Distance = FMath::Abs(Center.X - WantCenter.X) + FMath::Abs(Center.Y - WantCenter.Y);
		if (!bAspectChanged)
		{
			Candidate.Distance += 0.5 * (FMath::Abs(Candidate.Size.X - WantSize.X) + FMath::Abs(Candidate.Size.Y - WantSize.Y));
		}
	}
	OnScreen.Sort([](const FCandidate& InLeft, const FCandidate& InRight) { return InLeft.Distance < InRight.Distance; });

	const FCandidate& Best = OnScreen[0];
	if (Best.Distance > MaxDistance)
	{
		OutWhy = FString::Printf(TEXT("屏上有 %d 个候选，离录制时的位置（%.3f, %.3f）最近的 %s 也差 %.3f%s，界面布局可能变了"),
			OnScreen.Num(), WantMin.X, WantMin.Y, *DescribePick(Best), Best.Distance,
			bAspectChanged ? TEXT("（屏幕比例和录制时不同，已经放宽）") : TEXT(""));
		return false;
	}
	if (OnScreen.Num() > 1 && OnScreen[1].Distance - Best.Distance < MinRectMargin)
	{
		OutWhy = FString::Printf(TEXT("屏上有 %d 个候选，%s 和 %s 离录制时的位置差不多近，分不出是哪一个"),
			OnScreen.Num(), *DescribePick(Best), *DescribePick(OnScreen[1]));
		return false;
	}

	OutNode = Best.Node;
	OutNote = FString::Printf(TEXT("选择器命中 %d 个，按录制时的位置挑了 %s（偏差 %.3f%s）"), InCandidates.Num(), *Best.Node->Path, Best.Distance,
		bAspectChanged ? TEXT("，屏幕比例和录制时不同，只比中心") : TEXT(""));
	return true;
}

bool GameTestHintPick::Rematch(
	const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens,
	const FString& InTarget,
	const FJsonObject& InHint,
	TSharedPtr<FGameTestWidgetNode>& InOutNode,
	FString& OutNote)
{
	using namespace GameTestHintPickDetail;

	// 用户在面板上把这一步定成「只按第几个点」：就是要点容器里那个位置上的，不管上面写的是什么。
	bool bByIndex = false;
	if (InHint.TryGetBoolField(TEXT("byIndex"), bByIndex) && bByIndex)
	{
		return false;
	}
	// 这一步已经按唯一标识认（hint.key.cond，1.4.0）：方括号里的条件就是答案，按文字换位置反而会认错
	const TSharedPtr<FJsonObject>* Key = nullptr;
	FString KeyCond;
	if (InHint.TryGetObjectField(TEXT("key"), Key) && Key != nullptr && Key->IsValid() && (*Key)->TryGetStringField(TEXT("cond"), KeyCond) && !KeyCond.IsEmpty())
	{
		return false;
	}
	FString WantText;
	if (!InOutNode.IsValid() || !GameTestJson::TryGetString(InHint, TEXT("text"), WantText) || WantText.IsEmpty())
	{
		return false;
	}
	// 原位置上的文字按三档里任一档对得上（数字、红点变了）就算没换位置，不动。
	const FString NowText = TextOf(*InOutNode);
	if (GameTestTextMatch::Compare(WantText, NowText) != GameTestTextMatch::ETier::None)
	{
		return false;
	}

	GameTestSelector::FParsed Parsed;
	FString ParseError;
	if (!GameTestSelector::Parse(InTarget, Parsed, ParseError) || Parsed.bHandle || Parsed.bSlateOnly)
	{
		return false;
	}
	TArray<FPositional> Positionals;
	CollectPositionals(Parsed, Positionals);

	// 从最靠近目标的那段往外试：这一层的兄弟里有对得上的就停，不再动外层。
	for (int32 Slot = Positionals.Num() - 1; Slot >= 0; --Slot)
	{
		const FPositional& Positional = Positionals[Slot];
		TArray<TSharedPtr<FGameTestWidgetNode>> Siblings;
		TArray<int32> SiblingIndices;
		TArray<FString> SiblingTexts;
		for (int32 Index = 0; Index < MaxSiblingTries; ++Index)
		{
			if (Index == Positional.Index)
			{
				continue;
			}
			GameTestSelector::FParsed Variant = Parsed;
			SegmentsOf(Variant, Positional.Hop)[Positional.Segment] = Positional.Prefix + FString::FromInt(Index) + Positional.Suffix;
			TSharedPtr<FGameTestWidgetNode> Node;
			FString Error;
			// 只认唯一命中：兄弟换过去还歧义的不碰（那是多实例消歧的事，见 Pick）。
			if (!GameTestSelector::Resolve(InScreens, Variant, Node, Error) || !Node.IsValid())
			{
				continue;
			}
			if (!IsOnScreen(SlateOfNode(*Node)))
			{
				continue;
			}
			Siblings.Add(Node);
			SiblingIndices.Add(Index);
			SiblingTexts.Add(TextOf(*Node));
		}
		// 兄弟里按三档挑最高的一档：有完全一样的就不看忽略数字的，免得「领取(3)」抢了「领取」。
		TArray<int32> Hits;
		const GameTestTextMatch::ETier Tier = GameTestTextMatch::FilterBest(WantText, SiblingTexts, Hits);
		if (Hits.Num() == 0)
		{
			continue;
		}
		TArray<TSharedPtr<FGameTestWidgetNode>> Matches;
		TArray<int32> MatchIndices;
		for (const int32 Hit : Hits)
		{
			Matches.Add(Siblings[Hit]);
			MatchIndices.Add(SiblingIndices[Hit]);
		}

		const FString Segment = SegmentsOf(Parsed, Positional.Hop)[Positional.Segment];
		int32 Chosen = 0;
		if (Matches.Num() > 1)
		{
			// 同一个容器里有几个文字一样的：按录制时的位置挑；挑不出就别猜，照旧按序号。
			TSharedPtr<FGameTestWidgetNode> Picked;
			FString PickNote;
			FString Why;
			if (!Pick(Matches, InHint, Picked, PickNote, Why) || !Picked.IsValid())
			{
				return false;
			}
			Chosen = Matches.IndexOfByKey(Picked);
			if (Chosen == INDEX_NONE)
			{
				return false;
			}
		}

		OutNote = FString::Printf(TEXT("录制时 %s 的文字是「%s」，现在那个位置是「%s」（容器里的顺序变了）；同容器里第 %d 个对得上（%s），点它：%s"),
			*Segment, *WantText, *NowText.Left(40), MatchIndices[Chosen] + 1, GameTestTextMatch::Describe(Tier), *Matches[Chosen]->Path);
		InOutNode = Matches[Chosen];
		return true;
	}
	return false;
}
