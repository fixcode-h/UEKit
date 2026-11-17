// Copyright Epic Games, Inc. All Rights Reserved.

// 录制时给容器条目定标识（设计稿 GameTest列表子控件唯一标识 第 3、4 节）：规则 → 够准的自动推荐 → 待定。
// 从 GameTestItemKey.cpp 拆出来：那边是读值、找命中、打分这些「怎么认」，这边是「录的时候选哪个」。

#include "Observe/GameTestItemKey.h"

#include "Core/GameTestRunConfig.h"
#include "Observe/GameTestKeyRules.h"
#include "Observe/GameTestSnapshot.h"

#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonValue.h"

namespace GameTestItemKeyDecideDetail
{
	/** 待定时，分数到这个的唯一字段先用上（总比按位置强）；再低（像位置的）不如按位置。 */
	constexpr double PendingUseScore = 2.0;
	constexpr int32 MaxCandidates = 12;
}

FString GameTestItemKey::CondFromFields(const FGameTestKeySite& InSite, const TArray<FString>& InBy)
{
	FGameTestKeyCache Cache;
	TArray<FString> Parts;
	for (const FString& By : InBy)
	{
		TArray<FGameTestKeyCond> Conds;
		FString Ignore;
		if (!ParseConds(By + TEXT("=_"), Conds, Ignore) || Conds.Num() != 1)
		{
			return FString();
		}
		const TSharedPtr<FJsonValue> Value = ReadValue(InSite.Entry, InSite.bList, Conds[0].Source, Conds[0].Path, Cache);
		const FString Text = Value.IsValid() ? FormatValue(*Value) : FString();
		if (Text.IsEmpty())
		{
			return FString();
		}
		Parts.Add(By + TEXT("=") + Text);
	}
	return FString::Join(Parts, TEXT(","));
}

bool GameTestItemKey::Decide(FGameTestKeySite& InOutSite)
{
	using namespace GameTestItemKeyDecideDetail;
	UWidget* Container = InOutSite.Container.Get();
	TArray<FGameTestKeyEntry> Entries;
	if (!CollectEntries(Container, Entries))
	{
		return false;
	}
	const int32 Mine = FindEntry(Entries, InOutSite.Entry.Widget.Get(), InOutSite.Entry.Item.Get());
	if (Mine == INDEX_NONE)
	{
		return false;
	}
	InOutSite.Entry = Entries[Mine];
	InOutSite.Count = Entries.Num();
	if (const UWidget* EntryWidget = InOutSite.Entry.Widget.Get())
	{
		InOutSite.EntryClass = GameTestSnapshot::FriendlyClassName(EntryWidget->GetClass());
	}
	BuildCandidates(Container, Entries, Mine, MaxCandidates, InOutSite.Candidates);

	// 规则：问过一次、勾了「以后都按这个认」的
	FGameTestKeyRule Rule;
	if (!InOutSite.EntryClass.IsEmpty() && GameTestKeyRules::Find(InOutSite.EntryClass, InOutSite.Base, Rule))
	{
		const FString Cond = CondFromFields(InOutSite, Rule.By);
		TArray<FGameTestKeyCond> Conds;
		FString Ignore;
		if (!Cond.IsEmpty() && ParseConds(Cond, Conds, Ignore))
		{
			FGameTestKeyCache Cache;
			TArray<int32> Hits;
			FindMatches(Entries, InOutSite.bList, Conds, Cache, 2, Hits);
			if (Hits.Num() == 1)
			{
				InOutSite.Cond = Cond;
				InOutSite.By = TEXT("rule");
				return true;
			}
		}
		// 规则在这次对不上（字段读不到、不唯一）：照常挑，让人看见再定
	}

	Choose(InOutSite);
	return true;
}

void GameTestItemKey::Choose(FGameTestKeySite& InOutSite)
{
	using namespace GameTestItemKeyDecideDetail;
	if (IsConfident(InOutSite.Candidates))
	{
		InOutSite.Cond = InOutSite.Candidates[0].Cond;
		InOutSite.By = TEXT("auto");
		return;
	}

	InOutSite.By = TEXT("pending");
	InOutSite.Cond.Reset();
	for (const FGameTestKeyCandidate& Candidate : InOutSite.Candidates)
	{
		if (Candidate.Hits == 1 && !Candidate.bChanged && Candidate.Score >= PendingUseScore)
		{
			InOutSite.Cond = Candidate.Cond;
			break;
		}
	}
}

bool GameTestItemKey::Apply(FGameTestKeySite& InOutSite, const FString& InCond, FString& InOutTarget)
{
	if (InOutSite.From.IsEmpty() || !InOutTarget.StartsWith(InOutSite.From, ESearchCase::CaseSensitive))
	{
		return false;
	}
	const FString NewFrom = InCond.IsEmpty() ? InOutSite.Positional : FString::Printf(TEXT("%s[%s]"), *InOutSite.Base, *InCond);
	InOutTarget = NewFrom + InOutTarget.RightChop(InOutSite.From.Len());
	InOutSite.From = NewFrom;
	return true;
}
