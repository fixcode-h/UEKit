// Copyright Epic Games, Inc. All Rights Reserved.

#include "Integration/GameTestRpcKeys.h"

#include "Core/GameTestWorld.h"
#include "Observe/GameTestItemKey.h"
#include "Observe/GameTestKeyPreview.h"
#include "Observe/GameTestSelector.h"
#include "Observe/GameTestSelectorWriter.h"
#include "Observe/GameTestSnapshot.h"
#include "Observe/GameTestWidgetNode.h"
#include "Record/GameTestRecorder.h"

#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"

namespace GameTestRpcKeysDetail
{
	constexpr int32 DefaultFieldPage = 60;
	constexpr int32 MaxFieldPage = 200;
	/** 预览最多框几个：命中一大片时框多了也看不清，数照样报全。 */
	constexpr int32 MaxPreviewHits = 50;

	FString Str(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name)
	{
		FString Value;
		if (Args.IsValid())
		{
			Args->TryGetStringField(Name, Value);
		}
		return Value.TrimStartAndEnd();
	}

	/**
	 * 要看哪个条目：录制中给 `step`（从 0 数，用按下那一刻记下的站点），否则给 `target`（现在的游戏界面里解析，
	 * 再按写选择器的同一套规则找它所在的容器）。
	 */
	bool SiteFromArgs(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FGameTestKeySite>& OutSite, FString& OutError)
	{
		double StepNumber = 0.0;
		if (Args.IsValid() && Args->TryGetNumberField(TEXT("step"), StepNumber))
		{
			if (!FGameTestRecorder::Get().IsRecording())
			{
				OutError = TEXT("step 只在录制中能用：录完的场景传 target");
				return false;
			}
			const int32 Step = static_cast<int32>(StepNumber);
			OutSite = FGameTestRecorder::Get().FindKeySite(Step);
			if (!OutSite.IsValid())
			{
				OutError = FString::Printf(TEXT("第 %d 步没有点在列表、格子这类容器的条目上"), Step);
				return false;
			}
			if (!OutSite->Container.IsValid())
			{
				OutError = FString::Printf(TEXT("第 %d 步点的那个容器已经关掉了：打开那个界面后传 target 再看"), Step);
				return false;
			}
			return true;
		}

		const FString Target = Str(Args, TEXT("target"));
		if (Target.IsEmpty())
		{
			OutError = TEXT("要 step（录制中，从 0 数）或 target（选择器）");
			return false;
		}
		UWorld* World = GameTestWorld::FindGameWorld();
		if (World == nullptr)
		{
			OutError = TEXT("游戏没在跑：先起 PIE 或游戏，打开那个界面再看");
			return false;
		}
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		GameTestSnapshot::CaptureScreens(World, Screens, /*bInLight=*/true);
		TSharedPtr<FGameTestWidgetNode> Node;
		if (!GameTestSelector::ResolveText(Screens, Target, Node, OutError) || !Node.IsValid())
		{
			return false;
		}
		UWidget* Widget = Node->Widget.Get();
		if (Widget == nullptr)
		{
			OutError = TEXT("这个目标是纯 Slate 控件（下拉项、菜单行），没有条目字段可挑");
			return false;
		}
		FGameTestWrittenTarget Written;
		if (!GameTestSelectorWriter::Write(*Widget, Written, OutError))
		{
			return false;
		}
		if (!Written.KeySite.IsValid() || !Written.KeySite->Container.IsValid())
		{
			OutError = TEXT("这个控件不在列表、格子这类容器的条目里：按控件路径认就行，不用挑标识");
			return false;
		}
		OutSite = Written.KeySite;
		return true;
	}
}

bool GameTestRpcKeys::RecordKey(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
{
	return FGameTestRecorder::Get().AnswerKey(Args, Out, Error);
}

bool GameTestRpcKeys::Keys(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
{
	using namespace GameTestRpcKeysDetail;
	// 录制中按 step 看、按下时存过全部字段：直接用那份（松开后列表可能刷新，现读会是别的条目），容器关了也能看
	double StepNumber = 0.0;
	if (Args.IsValid() && Args->TryGetNumberField(TEXT("step"), StepNumber) && FGameTestRecorder::Get().IsRecording())
	{
		const TSharedPtr<FGameTestKeySite> Pressed = FGameTestRecorder::Get().FindKeySite(static_cast<int32>(StepNumber));
		if (Pressed.IsValid() && Pressed->PressedFields.IsValid())
		{
			int32 PageOffset = 0;
			int32 PageLimit = DefaultFieldPage;
			Args->TryGetNumberField(TEXT("offset"), PageOffset);
			Args->TryGetNumberField(TEXT("limit"), PageLimit);
			Out = GameTestItemKey::PageFields(*Pressed->PressedFields, Str(Args, TEXT("filter")), FMath::Max(0, PageOffset), FMath::Clamp(PageLimit, 1, MaxFieldPage));
			Out->SetObjectField(TEXT("key"), GameTestItemKey::SiteToJson(*Pressed, /*InMaxCandidates=*/12));
			double Index = 0.0;
			if (Pressed->PressedFields->TryGetNumberField(TEXT("index"), Index))
			{
				Out->SetNumberField(TEXT("index"), Index);
			}
			return true;
		}
	}
	TSharedPtr<FGameTestKeySite> Site;
	if (!SiteFromArgs(Args, Site, Error))
	{
		return false;
	}
	UWidget* Container = Site->Container.Get();
	TArray<FGameTestKeyEntry> Entries;
	if (!GameTestItemKey::CollectEntries(Container, Entries))
	{
		Error = TEXT("容器里读不到条目（已经清空或关掉了）");
		return false;
	}
	const int32 Mine = GameTestItemKey::FindEntry(Entries, Site->Entry.Widget.Get(), Site->Entry.Item.Get());
	if (Mine == INDEX_NONE)
	{
		Error = TEXT("点的那个条目已经不在容器里了（列表刷新过）：打开界面后传 target 再看");
		return false;
	}
	int32 Offset = 0;
	int32 Limit = DefaultFieldPage;
	if (Args.IsValid())
	{
		Args->TryGetNumberField(TEXT("offset"), Offset);
		Args->TryGetNumberField(TEXT("limit"), Limit);
	}
	Out = GameTestItemKey::ListFields(Container, Entries, Mine, Str(Args, TEXT("filter")), FMath::Max(0, Offset), FMath::Clamp(Limit, 1, MaxFieldPage));
	if (!Out.IsValid())
	{
		Error = TEXT("读不到这个条目的字段");
		return false;
	}
	Out->SetObjectField(TEXT("key"), GameTestItemKey::SiteToJson(*Site, /*InMaxCandidates=*/12));
	Out->SetNumberField(TEXT("index"), Mine);
	return true;
}

bool GameTestRpcKeys::Preview(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
{
	using namespace GameTestRpcKeysDetail;
	const FString Cond = Str(Args, TEXT("cond"));
	if (Cond.IsEmpty())
	{
		Error = TEXT("要 cond（如 Data.ItemId=1001），配合 step 或 target");
		return false;
	}
	TArray<FGameTestKeyCond> Conds;
	FString ParseError;
	if (!GameTestItemKey::ParseConds(Cond, Conds, ParseError))
	{
		Error = ParseError.IsEmpty() ? FString(TEXT("cond 是序号不是条件：按位置认不用预览")) : ParseError;
		return false;
	}
	TSharedPtr<FGameTestKeySite> Site;
	if (!SiteFromArgs(Args, Site, Error))
	{
		return false;
	}
	TArray<FGameTestKeyEntry> Entries;
	if (!GameTestItemKey::CollectEntries(Site->Container.Get(), Entries))
	{
		Error = TEXT("容器里读不到条目（已经清空或关掉了）");
		return false;
	}
	FGameTestKeyCache Cache;
	TArray<int32> Hits;
	GameTestItemKey::FindMatches(Entries, Site->bList, Conds, Cache, MaxPreviewHits, Hits);

	TArray<UWidget*> Widgets;
	TArray<TSharedPtr<FJsonValue>> Items;
	const int32 Mine = GameTestItemKey::FindEntry(Entries, Site->Entry.Widget.Get(), Site->Entry.Item.Get());
	for (const int32 Hit : Hits)
	{
		const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), Hit);
		UWidget* Widget = Entries[Hit].Widget.Get();
		if (Widget != nullptr)
		{
			Widgets.Add(Widget);
			Item->SetStringField(TEXT("name"), Widget->GetName());
		}
		else
		{
			// 虚拟化列表里没滚到的条目：命中了但画面上没有，框不出来
			Item->SetBoolField(TEXT("offscreen"), true);
		}
		if (Hit == Mine)
		{
			Item->SetBoolField(TEXT("mine"), true);
		}
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	GameTestKeyPreview::Show(Widgets);

	Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("cond"), Cond);
	Out->SetNumberField(TEXT("hits"), Hits.Num());
	Out->SetBoolField(TEXT("unique"), Hits.Num() == 1);
	if (Hits.Num() >= MaxPreviewHits)
	{
		Out->SetBoolField(TEXT("more"), true);
	}
	Out->SetBoolField(TEXT("includesMine"), Hits.Contains(Mine));
	Out->SetNumberField(TEXT("count"), Entries.Num());
	Out->SetArrayField(TEXT("items"), Items);
	return true;
}
