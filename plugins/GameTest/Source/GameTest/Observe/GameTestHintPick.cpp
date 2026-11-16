// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestHintPick.h"

#include "Core/GameTestJson.h"
#include "Observe/GameTestSlate.h"
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

	// 2. 文字完全一样的优先；一个都对不上（换了语言）就不按文字筛。
	FString Text;
	if (GameTestJson::TryGetString(InHint, TEXT("text"), Text) && !Text.IsEmpty())
	{
		TArray<FCandidate> SameText;
		for (const FCandidate& Candidate : OnScreen)
		{
			if (GameTestSlate::CollectText(Candidate.Slate, 40).Equals(Text))
			{
				SameText.Add(Candidate);
			}
		}
		if (SameText.Num() == 1)
		{
			OutNode = SameText[0].Node;
			OutNote = FString::Printf(TEXT("选择器命中 %d 个，按录制时的文字「%s」挑了 %s"), InCandidates.Num(), *Text, *SameText[0].Node->Path);
			return true;
		}
		if (SameText.Num() > 1)
		{
			OnScreen = MoveTemp(SameText);
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
	const FVector2D WantCenter = WantMin + WantSize * 0.5;
	for (FCandidate& Candidate : OnScreen)
	{
		const FVector2D Center = Candidate.Min + Candidate.Size * 0.5;
		Candidate.Distance = FMath::Abs(Center.X - WantCenter.X) + FMath::Abs(Center.Y - WantCenter.Y)
			+ 0.5 * (FMath::Abs(Candidate.Size.X - WantSize.X) + FMath::Abs(Candidate.Size.Y - WantSize.Y));
	}
	OnScreen.Sort([](const FCandidate& InLeft, const FCandidate& InRight) { return InLeft.Distance < InRight.Distance; });

	const FCandidate& Best = OnScreen[0];
	if (Best.Distance > MaxRectDistance)
	{
		OutWhy = FString::Printf(TEXT("屏上有 %d 个候选，离录制时的位置（%.3f, %.3f）最近的 %s 也差 %.3f，界面布局可能变了"),
			OnScreen.Num(), WantMin.X, WantMin.Y, *DescribePick(Best), Best.Distance);
		return false;
	}
	if (OnScreen.Num() > 1 && OnScreen[1].Distance - Best.Distance < MinRectMargin)
	{
		OutWhy = FString::Printf(TEXT("屏上有 %d 个候选，%s 和 %s 离录制时的位置差不多近，分不出是哪一个"),
			OnScreen.Num(), *DescribePick(Best), *DescribePick(OnScreen[1]));
		return false;
	}

	OutNode = Best.Node;
	OutNote = FString::Printf(TEXT("选择器命中 %d 个，按录制时的位置挑了 %s（偏差 %.3f）"), InCandidates.Num(), *Best.Node->Path, Best.Distance);
	return true;
}
