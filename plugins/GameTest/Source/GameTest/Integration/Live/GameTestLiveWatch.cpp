// Copyright Epic Games, Inc. All Rights Reserved.

#include "Integration/Live/GameTestLive.h"
#include "Core/GameTestClock.h"

#include "Core/GameTestJson.h"
#include "AgentPanelBridge.h"
#include "GameTestCompat.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"

/**
 * 此刻的结果实时推送（定稿 9.2、9.3）：面板登记一组只读查询（条件组、表达式、控件字段、控件状态），
 * 这里每隔 intervalMs 用和 /rpc 同一个操作重算一次，结果变了才推 `gt.watch {id, results: {变了的键: 结果}}`。
 * 面板因此不用轮询，也不另写一套求值；只许登记只读操作（下面的白名单）。
 */
namespace GameTestLiveWatchDetail
{
	struct FItem
	{
		FString Key;
		FString Op;
		TSharedPtr<FJsonObject> Args;
		FString Last;
	};

	struct FWatch
	{
		FString Id;
		TArray<FItem> Items;
		double Interval = 0.2;
		double NextAt = 0.0;
		double ExpireAt = 0.0;
		int32 Seq = 0;
	};

	TArray<FWatch>& Watches()
	{
		static TArray<FWatch> Instance;
		return Instance;
	}

	/** 只读、和游戏状态有关的查询；会动手的操作（gt.step、gt.run……）不许挂进来。 */
	const TCHAR* const ReadOnlyOps[] = { TEXT("gt.resolve"), TEXT("gt.vars"), TEXT("gt.inspect") };

	constexpr int32 MaxWatches = 32;
	constexpr int32 MaxItems = 16;
	constexpr double DefaultTtlSeconds = 60.0;
	constexpr double MaxTtlSeconds = 600.0;

	bool IsReadOnlyOp(const FString& InOp)
	{
		for (const TCHAR* Allowed : ReadOnlyOps)
		{
			if (InOp == Allowed)
			{
				return true;
			}
		}
		return false;
	}

	/** 跑一项查询：成功回操作的返回，失败回 `{error}`（游戏没开、控件不在屏上都是正常情况，照常推给面板显示）。 */
	TSharedPtr<FJsonObject> Evaluate(const FItem& InItem)
	{
		TSharedPtr<FJsonObject> Out;
		FString Error;
		if (!FAgentPanelBridge::Get().CallOp(InItem.Op, InItem.Args, Out, Error))
		{
			Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("error"), Error);
		}
		return Out.IsValid() ? Out : MakeShared<FJsonObject>();
	}

	/** 重算一遍；bInAll 时不管变没变都放进结果（第一次登记、续租时回给面板）。返回有没有东西。 */
	bool Collect(FWatch& InOutWatch, bool bInAll, const TSharedRef<FJsonObject>& OutResults)
	{
		bool bAny = false;
		for (FItem& Item : InOutWatch.Items)
		{
			const TSharedPtr<FJsonObject> Result = Evaluate(Item);
			const FString Text = GameTestJson::ToCompactString(Result);
			if (bInAll || Text != Item.Last)
			{
				OutResults->SetObjectField(Item.Key, Result);
				bAny = true;
			}
			Item.Last = Text;
		}
		return bAny;
	}

	bool ParseItems(const TSharedPtr<FJsonObject>& InArgs, TArray<FItem>& OutItems, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* Items = nullptr;
		if (!InArgs->TryGetObjectField(TEXT("items"), Items) || Items == nullptr || !Items->IsValid() || (*Items)->Values.Num() == 0)
		{
			OutError = TEXT("gt.watch 要 items：{键: {op, args}}，op 只能是 gt.resolve / gt.vars / gt.inspect");
			return false;
		}
		if ((*Items)->Values.Num() > MaxItems)
		{
			OutError = FString::Printf(TEXT("gt.watch 一次最多看 %d 项，分成几个 id 登记"), MaxItems);
			return false;
		}
		for (const auto& Pair : (*Items)->Values)
		{
			const FString Key = GAMETEST_JSON_KEY(Pair.Key);
			const TSharedPtr<FJsonObject>* Spec = nullptr;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(Spec) || Spec == nullptr || !Spec->IsValid())
			{
				OutError = FString::Printf(TEXT("gt.watch 的 items.%s 要是 {op, args}"), *Key);
				return false;
			}
			FItem Item;
			Item.Key = Key;
			(*Spec)->TryGetStringField(TEXT("op"), Item.Op);
			if (!IsReadOnlyOp(Item.Op))
			{
				OutError = FString::Printf(TEXT("gt.watch 的 items.%s：op=%s 不能实时看，只能是 gt.resolve / gt.vars / gt.inspect"), *Key, *Item.Op);
				return false;
			}
			const TSharedPtr<FJsonObject>* ItemArgs = nullptr;
			Item.Args = ((*Spec)->TryGetObjectField(TEXT("args"), ItemArgs) && ItemArgs != nullptr && ItemArgs->IsValid()) ? *ItemArgs : MakeShared<FJsonObject>();
			OutItems.Add(MoveTemp(Item));
		}
		return true;
	}

	void Publish(const FWatch& InWatch, const TSharedRef<FJsonObject>& InResults)
	{
		const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
		Event->SetStringField(TEXT("id"), InWatch.Id);
		Event->SetNumberField(TEXT("seq"), InWatch.Seq);
		Event->SetObjectField(TEXT("results"), InResults);
		FAgentPanelBridge::Get().PublishEvent(TEXT("gt.watch"), Event);
	}
}

bool GameTestLive::Watch(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
{
	using namespace GameTestLiveWatchDetail;
	FString Id;
	if (!InArgs.IsValid() || !InArgs->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("gt.watch 要 id（面板起的名字，推送事件按它对回去）");
		return false;
	}
	TArray<FWatch>& All = Watches();
	OutResult = MakeShared<FJsonObject>();
	OutResult->SetStringField(TEXT("id"), Id);

	bool bStop = false;
	InArgs->TryGetBoolField(TEXT("stop"), bStop);
	if (bStop)
	{
		All.RemoveAll([&Id](const FWatch& InWatch) { return InWatch.Id == Id; });
		OutResult->SetBoolField(TEXT("stopped"), true);
		return true;
	}

	TArray<FItem> Items;
	if (!ParseItems(InArgs, Items, OutError))
	{
		return false;
	}
	double IntervalMs = 200.0;
	double TtlMs = DefaultTtlSeconds * 1000.0;
	InArgs->TryGetNumberField(TEXT("intervalMs"), IntervalMs);
	InArgs->TryGetNumberField(TEXT("ttlMs"), TtlMs);

	// 同一个 id 再登记：换成新的查询（也算续租）；满了挤掉最早登记的（多半是面板关掉后没来得及停的）
	All.RemoveAll([&Id](const FWatch& InWatch) { return InWatch.Id == Id; });
	if (All.Num() >= MaxWatches)
	{
		All.RemoveAt(0);
	}
	const double Now = GameTestClock::Now();
	FWatch& Added = All.AddDefaulted_GetRef();
	Added.Id = Id;
	Added.Items = MoveTemp(Items);
	Added.Interval = FMath::Clamp(IntervalMs, 100.0, 5000.0) / 1000.0;
	Added.ExpireAt = Now + FMath::Clamp(TtlMs / 1000.0, 5.0, MaxTtlSeconds);
	Added.NextAt = Now + Added.Interval;

	// 登记时马上回一份完整结果：面板不用等第一次推送
	const TSharedRef<FJsonObject> Results = MakeShared<FJsonObject>();
	Collect(Added, /*bInAll=*/true, Results);
	OutResult->SetObjectField(TEXT("results"), Results);
	OutResult->SetNumberField(TEXT("intervalMs"), FMath::RoundToInt(Added.Interval * 1000.0));
	EnsureTicker();
	return true;
}

bool GameTestLive::WatchTick(double InNow)
{
	using namespace GameTestLiveWatchDetail;
	TArray<FWatch>& All = Watches();
	All.RemoveAll([InNow](const FWatch& InWatch) { return InNow >= InWatch.ExpireAt; });
	for (FWatch& Each : All)
	{
		if (InNow < Each.NextAt)
		{
			continue;
		}
		Each.NextAt = InNow + Each.Interval;
		const TSharedRef<FJsonObject> Results = MakeShared<FJsonObject>();
		if (Collect(Each, /*bInAll=*/false, Results))
		{
			++Each.Seq;
			Publish(Each, Results);
		}
	}
	return All.Num() > 0;
}

void GameTestLive::WatchShutdown()
{
	GameTestLiveWatchDetail::Watches().Reset();
}
