// Copyright Epic Games, Inc. All Rights Reserved.

// 录制时「这一格怎么认」（1.4.0）：点到列表、格子这类容器里的条目时，按下那一刻挑好的标识在松开后复查，
// 拿不准（或设成每次都问）就暂停游戏，推送 gt.record { phase: "ask" }，等面板（或 Agent）用 gt.record.key 回答。
// 暂停期间不录任何输入，这段时间也不算进录制间隔。

#include "Record/GameTestRecorder.h"

#include "Core/GameTestRunConfig.h"
#include "Core/GameTestWorld.h"
#include "Observe/GameTestItemKey.h"
#include "Observe/GameTestKeyRules.h"
#include "GameTest.h"

#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

namespace GameTestRecordKeyAskDetail
{
	/** 推送和状态里带的候选数：弹窗一屏看得完，状态返回也不超 4000 字。 */
	constexpr int32 AskCandidates = 10;
	/** 自动继续最长一天：填错成很大的数也不至于溢出。 */
	constexpr double MaxTimeoutSeconds = 86400.0;
	/** 工具栏上一直挂着的提示：人在游戏窗口里也知道为什么停了。 */
	const TCHAR* const AskNotice = TEXT("游戏已暂停：到 Agent 面板里选这一格怎么认");

	/** 条件 `Data.ItemId=1001,@lua.Slot=3` → 规则里存的字段 `["Data.ItemId", "@lua.Slot"]`。 */
	TArray<FString> FieldsOfCond(const FString& InCond)
	{
		TArray<FString> Parts;
		InCond.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty=*/true);
		TArray<FString> Fields;
		for (const FString& Part : Parts)
		{
			FString Left;
			FString Right;
			if (Part.Split(TEXT("="), &Left, &Right))
			{
				Fields.Add(Left.TrimStartAndEnd());
			}
		}
		return Fields;
	}

	/** 步骤的 hint（没有就建一个挂上）。 */
	TSharedRef<FJsonObject> HintOf(const TSharedPtr<FJsonObject>& InStep)
	{
		const TSharedPtr<FJsonObject>* Hint = nullptr;
		if (InStep->TryGetObjectField(TEXT("hint"), Hint) && Hint != nullptr && Hint->IsValid())
		{
			return Hint->ToSharedRef();
		}
		const TSharedRef<FJsonObject> Created = MakeShared<FJsonObject>();
		InStep->SetObjectField(TEXT("hint"), Created);
		return Created;
	}

	void WriteKey(const TSharedPtr<FJsonObject>& InStep, const FGameTestKeySite& InSite)
	{
		const TSharedRef<FJsonObject> Hint = HintOf(InStep);
		Hint->SetObjectField(TEXT("key"), GameTestItemKey::SiteToJson(InSite));
		// 「只按第几个」才写 byIndex（回放不按文字换位置）；换成别的认法要去掉
		if (InSite.By == TEXT("index"))
		{
			Hint->SetBoolField(TEXT("byIndex"), true);
		}
		else
		{
			Hint->RemoveField(TEXT("byIndex"));
		}
	}
}

void FGameTestRecorder::SetKeyAskOptions(const FString& InMode, double InTimeoutSeconds)
{
	const FString Mode = InMode.TrimStartAndEnd().ToLower();
	AskMode = (Mode == TEXT("always") || Mode == TEXT("never")) ? Mode : FString(TEXT("unsure"));
	AskTimeoutSeconds = FMath::Clamp(InTimeoutSeconds, 0.0, GameTestRecordKeyAskDetail::MaxTimeoutSeconds);
}

bool FGameTestRecorder::IsAskingKey() const
{
	return KeyAsk.IsSet();
}

int32 FGameTestRecorder::IndexOfStep(const TSharedPtr<FJsonObject>& InStep) const
{
	if (!InStep.IsValid())
	{
		return INDEX_NONE;
	}
	for (int32 Index = Steps.Num() - 1; Index >= 0; --Index)
	{
		if (&Steps[Index].Get() == InStep.Get())
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

TSharedPtr<FGameTestKeySite> FGameTestRecorder::FindKeySite(int32 InStep) const
{
	if (!Steps.IsValidIndex(InStep))
	{
		return nullptr;
	}
	for (const FKeyStep& KeyStep : KeySteps)
	{
		const TSharedPtr<FJsonObject> Step = KeyStep.Step.Pin();
		if (Step.IsValid() && Step.Get() == &Steps[InStep].Get())
		{
			return KeyStep.Site;
		}
	}
	return nullptr;
}

void FGameTestRecorder::AfterKeyStep(const TSharedRef<FJsonObject>& InStep, const TSharedPtr<FGameTestKeySite>& InSite, bool bInMayAsk)
{
	using namespace GameTestRecordKeyAskDetail;
	// 步数到上限没加进去的步骤不管
	if (!InSite.IsValid() || IndexOfStep(InStep) == INDEX_NONE)
	{
		return;
	}

	FGameTestKeySite& Site = *InSite;
	if (UWidget* Container = Site.Container.Get())
	{
		// 点完再读一次：选中、数量这类点一下就变的字段，回放时（点之前）对不上，不能当标识
		const FString Before = Site.Cond;
		GameTestItemKey::Recheck(Container, Site.Entry, Site.Candidates);
		bool bChosenChanged = false;
		for (const FGameTestKeyCandidate& Candidate : Site.Candidates)
		{
			bChosenChanged |= !Before.IsEmpty() && Candidate.bChanged && Candidate.Cond == Before;
		}
		if (Site.By == TEXT("rule") && !Before.IsEmpty())
		{
			// 规则拼出来的条件可能是组合，候选里不一定有：按规则的字段再拼一次，和按下时不一样就是变了
			TArray<FString> Fields = FieldsOfCond(Before);
			bChosenChanged = GameTestItemKey::CondFromFields(Site, Fields) != Before;
		}
		if (bChosenChanged || (Site.By == TEXT("auto") && !GameTestItemKey::IsConfident(Site.Candidates)))
		{
			GameTestItemKey::Choose(Site);
		}
		if (Site.Cond != Before)
		{
			FString Target;
			InStep->TryGetStringField(TEXT("target"), Target);
			if (GameTestItemKey::Apply(Site, Site.Cond, Target))
			{
				InStep->SetStringField(TEXT("target"), Target);
				UE_LOG(LogGameTest, Display, TEXT("录制：条目标识点完就变了，改按 %s 认"), Site.Cond.IsEmpty() ? TEXT("位置") : *Site.Cond);
			}
		}
	}
	WriteKey(InStep, Site);

	FKeyStep KeyStep;
	KeyStep.Step = InStep;
	KeyStep.Site = InSite;
	KeySteps.Add(MoveTemp(KeyStep));

	const bool bAsk = bInMayAsk && (AskMode == TEXT("always") || (AskMode == TEXT("unsure") && Site.By == TEXT("pending")));
	if (bAsk)
	{
		BeginAsk(InStep, InSite);
	}
	else
	{
		Broadcast(TEXT("step"));
	}
}

void FGameTestRecorder::BeginAsk(const TSharedRef<FJsonObject>& InStep, const TSharedPtr<FGameTestKeySite>& InSite)
{
	using namespace GameTestRecordKeyAskDetail;
	if (KeyAsk.IsSet())
	{
		return;
	}
	FKeyAsk Ask;
	Ask.Step = InStep;
	Ask.Site = InSite;
	Ask.StartedAt = FPlatformTime::Seconds();

	// 世界停住、UI 照常画；游戏原本就停着的不动它（继续时也不替它解开）
	if (UWorld* World = GameTestWorld::FindGameWorld())
	{
		if (!World->IsPaused() && UGameplayStatics::SetGamePaused(World, true) && World->IsPaused())
		{
			Ask.PausedWorld = World;
		}
	}
	KeyAsk = MoveTemp(Ask);
	SetNotice(AskNotice);
	SyncToolbar();
	UE_LOG(LogGameTest, Display, TEXT("录制：第 %d 步点到的条目拿不准怎么认，暂停等你选"), IndexOfStep(InStep) + 1);
	Broadcast(TEXT("ask"));
}

void FGameTestRecorder::EndAsk()
{
	if (!KeyAsk.IsSet())
	{
		return;
	}
	const double Paused = FPlatformTime::Seconds() - KeyAsk->StartedAt;
	if (UWorld* World = KeyAsk->PausedWorld.Get())
	{
		UGameplayStatics::SetGamePaused(World, false);
	}
	KeyAsk.Reset();
	// 暂停的这段时间不算进下一步前的等待（keepPace 时不会录出一段长 wait）
	if (LastEventAt > 0.0)
	{
		LastEventAt += Paused;
	}
	SetNotice(FString());
	SyncToolbar();
	if (bRecording)
	{
		Broadcast(TEXT("askEnd"));
	}
}

void FGameTestRecorder::TickAsk()
{
	if (!KeyAsk.IsSet())
	{
		return;
	}
	if (!bRecording || !KeyAsk->Step.IsValid())
	{
		EndAsk();
		return;
	}
	// 我们停的世界没了（PIE 停了）：没什么可等的
	if (KeyAsk->PausedWorld.IsStale())
	{
		EndAsk();
		return;
	}
	// 提示过几秒会自己消失：等着的时候一直续上
	SetNotice(GameTestRecordKeyAskDetail::AskNotice);
	if (AskTimeoutSeconds > 0.0 && FPlatformTime::Seconds() - KeyAsk->StartedAt >= AskTimeoutSeconds)
	{
		UE_LOG(LogGameTest, Display, TEXT("录制：%.0f 秒没选，这一步先按自动挑的写（待定），游戏继续"), AskTimeoutSeconds);
		EndAsk();
	}
}

void FGameTestRecorder::DescribeAsk(FJsonObject& OutStatus) const
{
	using namespace GameTestRecordKeyAskDetail;
	OutStatus.SetStringField(TEXT("askKey"), AskMode);
	if (AskTimeoutSeconds > 0.0)
	{
		OutStatus.SetNumberField(TEXT("askTimeoutSec"), AskTimeoutSeconds);
	}
	if (!KeyAsk.IsSet() || !KeyAsk->Site.IsValid())
	{
		return;
	}
	const TSharedPtr<FJsonObject> Step = KeyAsk->Step.Pin();
	if (!Step.IsValid())
	{
		return;
	}
	const FGameTestKeySite& Site = *KeyAsk->Site;
	const TSharedRef<FJsonObject> Ask = MakeShared<FJsonObject>();
	Ask->SetNumberField(TEXT("step"), IndexOfStep(Step));
	FString Target;
	Step->TryGetStringField(TEXT("target"), Target);
	Ask->SetStringField(TEXT("target"), Target);
	Ask->SetNumberField(TEXT("pausedSec"), FMath::RoundToInt(FPlatformTime::Seconds() - KeyAsk->StartedAt));
	Ask->SetBoolField(TEXT("gamePaused"), KeyAsk->PausedWorld.IsValid());
	if (const UWidget* Container = Site.Container.Get())
	{
		Ask->SetStringField(TEXT("container"), Container->GetName());
		Ask->SetStringField(TEXT("containerClass"), Container->GetClass()->GetName());
	}
	if (const UWidget* EntryWidget = Site.Entry.Widget.Get())
	{
		Ask->SetStringField(TEXT("entryName"), EntryWidget->GetName());
	}
	Ask->SetNumberField(TEXT("index"), Site.Entry.Index);
	Ask->SetObjectField(TEXT("key"), GameTestItemKey::SiteToJson(Site, AskCandidates));
	// 按下那一刻的截图（录制截图开着时有）：弹窗左边显示
	const TSharedPtr<FJsonObject>* Hint = nullptr;
	if (Step->TryGetObjectField(TEXT("hint"), Hint) && Hint != nullptr && Hint->IsValid())
	{
		for (const TCHAR* Field : { TEXT("shot"), TEXT("screen"), TEXT("text") })
		{
			if (const TSharedPtr<FJsonValue> Value = (*Hint)->TryGetField(Field))
			{
				Ask->SetField(Field, Value);
			}
		}
	}
	OutStatus.SetObjectField(TEXT("ask"), Ask);
}

bool FGameTestRecorder::AnswerKey(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
{
	using namespace GameTestRecordKeyAskDetail;
	if (!bRecording)
	{
		OutError = TEXT("没有在录制：录完的场景直接改步骤的 target（面板步骤详情「回放怎么认」）");
		return false;
	}
	const TSharedRef<FJsonObject> Args = InArgs.IsValid() ? InArgs.ToSharedRef() : MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> Step;
	TSharedPtr<FGameTestKeySite> Site;
	double StepNumber = 0.0;
	if (Args->TryGetNumberField(TEXT("step"), StepNumber))
	{
		const int32 Index = static_cast<int32>(StepNumber);
		if (!Steps.IsValidIndex(Index))
		{
			OutError = FString::Printf(TEXT("没有第 %d 步（从 0 数，现在一共 %d 步）"), Index, Steps.Num());
			return false;
		}
		Step = Steps[Index];
		Site = FindKeySite(Index);
		if (!Site.IsValid())
		{
			OutError = FString::Printf(TEXT("第 %d 步没有点在列表、格子这类容器的条目上，不用挑标识"), Index);
			return false;
		}
	}
	else
	{
		if (!KeyAsk.IsSet())
		{
			OutError = TEXT("现在没有在问哪一步：传 step（从 0 数）指定要改的步骤");
			return false;
		}
		Step = KeyAsk->Step.Pin();
		Site = KeyAsk->Site;
		if (!Step.IsValid() || !Site.IsValid())
		{
			EndAsk();
			OutError = TEXT("问的那一步已经没了（被删掉或录制重开了），游戏已继续");
			return false;
		}
	}

	FString Mode;
	FString Cond;
	Args->TryGetStringField(TEXT("mode"), Mode);
	Args->TryGetStringField(TEXT("cond"), Cond);
	Mode = Mode.TrimStartAndEnd().ToLower();
	Cond = Cond.TrimStartAndEnd();
	const TArray<TSharedPtr<FJsonValue>>* ByValues = nullptr;
	TArray<FString> ByFields;
	if (Args->TryGetArrayField(TEXT("by"), ByValues) && ByValues != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ByValues)
		{
			FString Field;
			if (Value.IsValid() && Value->TryGetString(Field) && !Field.TrimStartAndEnd().IsEmpty())
			{
				ByFields.Add(Field.TrimStartAndEnd());
			}
		}
	}

	FString NewBy;
	if (Mode == TEXT("skip"))
	{
		// 先跳过：保持现在挑的（待定），录完再定
		NewBy = Site->By;
		Cond = Site->Cond;
	}
	else if (Mode == TEXT("index") || Mode == TEXT("text"))
	{
		NewBy = Mode;
		Cond.Reset();
	}
	else if (!Mode.IsEmpty())
	{
		OutError = FString::Printf(TEXT("mode 只能是 index（只按第几个）、text（先按文字找）或 skip（先跳过），不认识 %s"), *Mode);
		return false;
	}
	else
	{
		if (ByFields.Num() > 0)
		{
			Cond = GameTestItemKey::CondFromFields(*Site, ByFields);
			if (Cond.IsEmpty())
			{
				OutError = FString::Printf(TEXT("这个条目上读不到 %s：字段名写错了，或值是空的。用 gt.keys 看有哪些字段"), *FString::Join(ByFields, TEXT("、")));
				return false;
			}
		}
		if (Cond.IsEmpty())
		{
			OutError = TEXT("要给 cond（如 Data.ItemId=1001）、by（字段数组）或 mode（index / text / skip）之一");
			return false;
		}
		TArray<FGameTestKeyCond> Conds;
		FString ParseError;
		if (!GameTestItemKey::ParseConds(Cond, Conds, ParseError))
		{
			OutError = ParseError.IsEmpty() ? FString::Printf(TEXT("[%s] 是序号，不是条件：只按位置认用 mode=index"), *Cond) : ParseError;
			return false;
		}
		// 容器还在（暂停着一般都在）就核对一下：这个条件在容器里只认出一个
		TArray<FGameTestKeyEntry> Entries;
		if (GameTestItemKey::CollectEntries(Site->Container.Get(), Entries))
		{
			FGameTestKeyCache Cache;
			TArray<int32> Hits;
			GameTestItemKey::FindMatches(Entries, Site->bList, Conds, Cache, 4, Hits);
			if (Hits.Num() == 0)
			{
				OutError = FString::Printf(TEXT("按 [%s] 现在一个都找不到：这个值点完就变了，换一个不会变的字段"), *Cond);
				return false;
			}
			if (Hits.Num() > 1)
			{
				OutError = FString::Printf(TEXT("按 [%s] 能找到 %d 个以上：再加一个字段组合，或换一个唯一的"), *Cond, Hits.Num());
				return false;
			}
		}
		NewBy = TEXT("user");
	}

	FString Target;
	Step->TryGetStringField(TEXT("target"), Target);
	if (!GameTestItemKey::Apply(*Site, Cond, Target))
	{
		OutError = TEXT("这一步的 target 已经被改过，和录制时对不上：直接改 target 的方括号");
		return false;
	}
	Step->SetStringField(TEXT("target"), Target);
	Site->Cond = Cond;
	Site->By = NewBy;
	WriteKey(Step, *Site);

	OutResult = MakeShared<FJsonObject>();
	OutResult->SetNumberField(TEXT("step"), IndexOfStep(Step));
	OutResult->SetStringField(TEXT("target"), Target);
	OutResult->SetStringField(TEXT("by"), NewBy);
	if (!Cond.IsEmpty())
	{
		OutResult->SetStringField(TEXT("cond"), Cond);
	}

	bool bRule = false;
	Args->TryGetBoolField(TEXT("rule"), bRule);
	if (bRule && !Cond.IsEmpty() && !Site->EntryClass.IsEmpty())
	{
		FGameTestKeyRule Rule;
		Rule.Entry = Site->EntryClass;
		Rule.Container = Site->Base;
		Rule.By = FieldsOfCond(Cond);
		FString RuleError;
		if (GameTestKeyRules::Save(Rule, RuleError))
		{
			OutResult->SetBoolField(TEXT("rule"), true);
		}
		else
		{
			// 规则存不进去不影响这一步：告诉调用方，这一步照样改好
			OutResult->SetStringField(TEXT("ruleError"), RuleError);
		}
	}

	bool bResume = true;
	Args->TryGetBoolField(TEXT("resume"), bResume);
	const bool bThisAsk = KeyAsk.IsSet() && KeyAsk->Step.Pin() == Step;
	if (bThisAsk && bResume)
	{
		EndAsk();
		OutResult->SetBoolField(TEXT("resumed"), true);
	}
	else
	{
		Broadcast(TEXT("key"));
	}
	return true;
}
