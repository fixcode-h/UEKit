// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/GameTestGuards.h"

#include "Core/Flow/GameTestScriptParser.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"
#include "Observe/Checks/GameTestWidgetProbe.h"

namespace
{
	/** 处理完 when 还成立连续几次算处理不掉（协议第 5 节）。 */
	constexpr int32 StuckLimit = 3;

	/** when 里所有 target 的屏名（递归 all / any / not）：unlessStepTargets 拿它和这一步的目标比。 */
	void CollectScreens(const TSharedPtr<FJsonValue>& InValue, TArray<FString>& OutScreens)
	{
		if (!InValue.IsValid())
		{
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (InValue->TryGetArray(Items) && Items)
		{
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				CollectScreens(Item, OutScreens);
			}
			return;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!InValue->TryGetObject(Object) || !Object || !Object->IsValid())
		{
			return;
		}
		FString Target;
		if ((*Object)->TryGetStringField(TEXT("target"), Target))
		{
			const FString Screen = GameTestWidgetProbe::ScreenOf(Target);
			if (!Screen.IsEmpty())
			{
				OutScreens.AddUnique(Screen);
			}
		}
		for (const TCHAR* Key : { TEXT("all"), TEXT("any"), TEXT("not") })
		{
			CollectScreens((*Object)->TryGetField(Key), OutScreens);
		}
	}

	/** 阻挡界面的 widget（类名或资产路径）→ 选择器的屏名。 */
	FString ScreenNameOfBlocker(const FString& InWidget)
	{
		FString Name = InWidget.TrimStartAndEnd();
		int32 Slash = INDEX_NONE;
		if (Name.FindLastChar(TEXT('/'), Slash))
		{
			Name.RightChopInline(Slash + 1);
		}
		int32 Dot = INDEX_NONE;
		if (Name.FindChar(TEXT('.'), Dot))
		{
			Name.LeftInline(Dot);
		}
		Name.RemoveFromEnd(TEXT("_C"));
		return Name;
	}

	TSharedRef<FJsonObject> MakeStep(const TCHAR* InAction, const FString& InTarget)
	{
		TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("action"), InAction);
		if (!InTarget.IsEmpty())
		{
			Step->SetStringField(TEXT("target"), InTarget);
		}
		return Step;
	}
}

namespace GameTestGuards
{
	const TArray<FString>& Fields()
	{
		static const TArray<FString> Keys = {
			TEXT("id"), TEXT("name"), TEXT("when"), TEXT("do"), TEXT("check"), TEXT("after"), TEXT("max"), TEXT("cooldownMs"),
			TEXT("unlessStepTargets"), TEXT("onFail"), TEXT("disabled"), TEXT("note"),
		};
		return Keys;
	}

	void ParseList(const TSharedPtr<FJsonValue>& InJson, const FString& InScope, const FString& InPath, FGameTestScriptParser& InParser, TArray<TSharedRef<FGameTestGuardDef>>& OutGuards)
	{
		FGameTestDiagSink& Sink = InParser.Sink();
		FGameTestDiagSink::FScope ListScope(Sink, InPath, FString(), FGameTestDiagSink::EScopeMode::FromRoot);
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!InJson.IsValid() || !InJson->TryGetArray(Items) || !Items)
		{
			Sink.Error(TEXT("bad_type"), TEXT("guards 要写成数组"));
			return;
		}
		for (int32 Index = 0; Index < Items->Num(); ++Index)
		{
			FGameTestDiagSink::FScope ItemScope(Sink, FString::FromInt(Index));
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Object) || !Object || !Object->IsValid())
			{
				Sink.Error(TEXT("bad_type"), TEXT("守护要写成对象"));
				continue;
			}
			const FJsonObject& Json = **Object;
			TSharedRef<FGameTestGuardDef> Guard = MakeShared<FGameTestGuardDef>();
			Guard->Scope = InScope;
			Guard->Path = FString::Printf(TEXT("%s.%d"), *InPath, Index);
			Json.TryGetStringField(TEXT("id"), Guard->Id);
			Json.TryGetStringField(TEXT("name"), Guard->Name);
			FGameTestDiagSink::FScope IdScope(Sink, FString(), Guard->Id);
			for (const auto& Pair : Json.Values)
			{
				const FString Key = GAMETEST_JSON_KEY(Pair.Key);
				if (!Fields().Contains(Key))
				{
					Sink.Error(TEXT("unknown_field"), FString::Printf(TEXT("守护不认识字段 %s（可用：%s）"), *Key, *FString::Join(Fields(), TEXT(", "))));
				}
			}

			const TSharedPtr<FJsonValue> When = Json.TryGetField(TEXT("when"));
			Guard->When = GameTestCond::AsObject(When);
			if (!Guard->When.IsValid())
			{
				Sink.Error(TEXT("bad_type"), TEXT("守护缺少 when（什么时候处理，统一条件）"));
			}
			else
			{
				InParser.CheckCond(When, TEXT("when"));
				CollectScreens(When, Guard->Screens);
			}

			if (!Json.HasField(TEXT("do")))
			{
				Sink.Error(TEXT("bad_type"), TEXT("守护缺少 do（处理它的步骤数组）"));
			}
			else
			{
				InParser.ParseNodes(Json.TryGetField(TEXT("do")), Guard->Path + TEXT(".do"), Guard->Do);
			}

			FString Text;
			if (Json.TryGetStringField(TEXT("check"), Text))
			{
				if (Text == TEXT("beforeStep")) Guard->Check = EGameTestGuardCheck::BeforeStep;
				else if (Text == TEXT("whileWaiting")) Guard->Check = EGameTestGuardCheck::WhileWaiting;
				else if (Text == TEXT("both")) Guard->Check = EGameTestGuardCheck::Both;
				else Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("check 只能是 beforeStep、whileWaiting、both，写的是 %s"), *Text));
			}
			if (Json.TryGetStringField(TEXT("after"), Text))
			{
				if (Text == TEXT("retry")) Guard->After = EGameTestGuardAfter::Retry;
				else if (Text == TEXT("skip")) Guard->After = EGameTestGuardAfter::Skip;
				else if (Text == TEXT("fail")) Guard->After = EGameTestGuardAfter::Fail;
				else Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("after 只能是 retry、skip、fail，写的是 %s"), *Text));
			}
			if (Json.TryGetStringField(TEXT("onFail"), Text))
			{
				if (Text == TEXT("ignore")) Guard->bIgnoreFail = true;
				else if (Text != TEXT("stop")) Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("守护的 onFail 只能是 stop、ignore，写的是 %s"), *Text));
			}
			Json.TryGetNumberField(TEXT("max"), Guard->Max);
			Json.TryGetNumberField(TEXT("cooldownMs"), Guard->CooldownMs);
			// max 写 0 = 不限次数（用户 2026-11-19：0 表示不限）；有 cooldownMs 隔开，不会空转
			if (Guard->Max < 0 || Guard->Max > 10000)
			{
				Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("max 要在 0–10000 之间（0 = 不限），写的是 %d"), Guard->Max));
			}
			Guard->CooldownMs = FMath::Clamp(Guard->CooldownMs, 0, 600000);
			Json.TryGetBoolField(TEXT("unlessStepTargets"), Guard->bUnlessStepTargets);
			Json.TryGetBoolField(TEXT("disabled"), Guard->bDisabled);
			OutGuards.Add(Guard);
		}
	}

	bool MigrateBlockers(FJsonObject& InOutSettings, TArray<FString>& OutNotes)
	{
		const TArray<TSharedPtr<FJsonValue>>* Blockers = nullptr;
		if (!InOutSettings.TryGetArrayField(TEXT("blockers"), Blockers) || !Blockers)
		{
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Guards;
		const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
		if (InOutSettings.TryGetArrayField(TEXT("guards"), Existing) && Existing)
		{
			Guards = *Existing;
		}
		int32 Seq = 0;
		for (const TSharedPtr<FJsonValue>& Item : *Blockers)
		{
			const TSharedPtr<FJsonObject>* Rule = nullptr;
			if (!Item.IsValid() || !Item->TryGetObject(Rule) || !Rule || !Rule->IsValid())
			{
				OutNotes.Add(TEXT("跳过一条不是对象的 blockers 项"));
				continue;
			}
			const FJsonObject& Blocker = **Rule;
			FString Widget;
			Blocker.TryGetStringField(TEXT("widget"), Widget);
			const FString Screen = ScreenNameOfBlocker(Widget);
			if (Screen.IsEmpty())
			{
				OutNotes.Add(TEXT("跳过一条没写 widget 的 blockers 项"));
				continue;
			}
			const FString Target = Screen + TEXT(":.");
			TSharedRef<FJsonObject> Guard = MakeShared<FJsonObject>();
			// id 按 b1、b2 补；和已有的守护 id 撞了就往后挪。
			FString Id;
			do
			{
				Id = FString::Printf(TEXT("b%d"), ++Seq);
			} while (Guards.ContainsByPredicate([&Id](const TSharedPtr<FJsonValue>& InGuard)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				FString Other;
				return InGuard.IsValid() && InGuard->TryGetObject(Object) && Object && Object->IsValid()
					&& (*Object)->TryGetStringField(TEXT("id"), Other) && Other == Id;
			}));
			Guard->SetStringField(TEXT("id"), Id);
			FString Name;
			if (!Blocker.TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				Blocker.TryGetStringField(TEXT("note"), Name);
			}
			if (!Name.IsEmpty())
			{
				Guard->SetStringField(TEXT("name"), Name);
			}

			TSharedRef<FJsonObject> When = MakeShared<FJsonObject>();
			When->SetStringField(TEXT("target"), Target);
			When->SetBoolField(TEXT("onScreen"), true);
			int32 DelayMs = 0;
			if (Blocker.TryGetNumberField(TEXT("delayMs"), DelayMs) && DelayMs > 0)
			{
				When->SetNumberField(TEXT("settleMs"), DelayMs);
			}
			Guard->SetObjectField(TEXT("when"), When);

			FString Action = TEXT("close");
			Blocker.TryGetStringField(TEXT("action"), Action);
			TSharedRef<FJsonObject> Step = MakeStep(TEXT("widget.close"), Target);
			if (Action == TEXT("call"))
			{
				Step = MakeStep(TEXT("widget.call"), Target);
				FString Function;
				Blocker.TryGetStringField(TEXT("function"), Function);
				Step->SetStringField(TEXT("function"), Function);
			}
			else if (Action == TEXT("command"))
			{
				Step = MakeStep(TEXT("console"), FString());
				FString Command;
				Blocker.TryGetStringField(TEXT("command"), Command);
				Step->SetStringField(TEXT("command"), Command);
			}
			TArray<TSharedPtr<FJsonValue>> Do;
			Do.Add(MakeShared<FJsonValueObject>(Step));
			Guard->SetArrayField(TEXT("do"), Do);

			bool bEnabled = true;
			if (Blocker.TryGetBoolField(TEXT("enabled"), bEnabled) && !bEnabled)
			{
				Guard->SetBoolField(TEXT("disabled"), true);
			}
			Guards.Add(MakeShared<FJsonValueObject>(Guard));
			OutNotes.Add(FString::Printf(TEXT("阻挡界面 %s（%s）→ 守护 %s"), *Widget, *Action, *Id));
		}
		InOutSettings.RemoveField(TEXT("blockers"));
		InOutSettings.SetArrayField(TEXT("guards"), Guards);
		return true;
	}
}

void FGameTestGuardMonitor::Reset()
{
	States.Reset();
	NextWaitCheckAt = 0.0;
}

const FGameTestGuardDef* FGameTestGuardMonitor::Poll(const TArray<TSharedRef<FGameTestGuardDef>>& InActive, bool bWaiting, const TArray<FString>& InStepTargets, FGameTestFlowContext& InContext, double InNowSeconds, bool& OutPending)
{
	OutPending = false;
	TArray<FString> StepScreens;
	for (const FString& Target : InStepTargets)
	{
		const FString Screen = GameTestWidgetProbe::ScreenOf(Target);
		if (!Screen.IsEmpty())
		{
			StepScreens.AddUnique(Screen);
		}
	}
	// 守护的判断一律只读：when 里的表达式、API 都不许改游戏状态。
	FGameTestFlowContext::FReadOnlyScope ReadOnly(InContext);
	for (const TSharedRef<FGameTestGuardDef>& Guard : InActive)
	{
		if (Guard->bDisabled || !Guard->When.IsValid())
		{
			continue;
		}
		const bool bModeOk = Guard->Check == EGameTestGuardCheck::Both
			|| (bWaiting ? Guard->Check == EGameTestGuardCheck::WhileWaiting : Guard->Check == EGameTestGuardCheck::BeforeStep);
		if (!bModeOk)
		{
			continue;
		}
		FState& State = States.FindOrAdd(Guard->Key());
		if (State.LastAt >= 0.0 && (InNowSeconds - State.LastAt) * 1000.0 < Guard->CooldownMs)
		{
			continue;
		}
		if (Guard->bUnlessStepTargets && Guard->Screens.ContainsByPredicate([&StepScreens](const FString& InScreen)
		{
			return StepScreens.Contains(InScreen);
		}))
		{
			// 这一步本来就是要操作这个界面（点引导里的按钮）：不当它是挡路的。
			State.bWatching = false;
			continue;
		}
		if (!State.bWatching)
		{
			State.Watch.Start(Guard->When, InContext, false);
			State.bWatching = true;
		}
		const FGameTestCondWatch::EVerdict Verdict = State.Watch.Tick(InContext, InNowSeconds);
		if (Verdict == FGameTestCondWatch::EVerdict::Holds)
		{
			State.bWatching = false;
			return &Guard.Get();
		}
		if (Verdict == FGameTestCondWatch::EVerdict::NotHolds || State.Watch.GetLast().State == EGameTestCondState::Unknown)
		{
			// 判断不了（反面判断的屏不在）对守护来说就是没出现：不拦着这一步。
			State.bWatching = State.Watch.GetLast().State == EGameTestCondState::Unknown;
			continue;
		}
		OutPending = true;
	}
	return nullptr;
}

bool FGameTestGuardMonitor::BeginHandle(const FGameTestGuardDef& InGuard, double InNowSeconds, FString& OutError)
{
	FState& State = States.FindOrAdd(InGuard.Key());
	++State.Count;
	State.LastAt = InNowSeconds;
	if (InGuard.Max > 0 && State.Count > InGuard.Max)
	{
		OutError = FString::Printf(TEXT("守护「%s」这次 run 已经触发 %d 次，超过 max=%d：界面可能在反复弹出，检查 when 是不是太宽，或把 max 调大"),
			*InGuard.Label(), State.Count, InGuard.Max);
		return false;
	}
	return true;
}

bool FGameTestGuardMonitor::EndHandle(const FGameTestGuardDef& InGuard, FGameTestFlowContext& InContext, double InNowSeconds, bool& OutStillThere, FString& OutError)
{
	FState& State = States.FindOrAdd(InGuard.Key());
	// 冷却从处理完开始算：do 本身可能要好几秒。
	State.LastAt = InNowSeconds;
	State.bWatching = false;
	OutStillThere = false;
	if (!InGuard.When.IsValid())
	{
		return true;
	}
	FGameTestCondResult Result;
	{
		FGameTestFlowContext::FReadOnlyScope ReadOnly(InContext);
		Result = GameTestCond::Evaluate(*InGuard.When, InContext);
	}
	if (!Result.Holds())
	{
		State.StillThere = 0;
		return true;
	}
	OutStillThere = true;
	if (++State.StillThere >= StuckLimit)
	{
		OutError = FString::Printf(TEXT("守护「%s」连续 %d 次处理完界面还在：do 没能处理掉它（%s）。检查 do 的步骤，或换一种关法"),
			*InGuard.Label(), StuckLimit, *Result.Summary());
		return false;
	}
	return true;
}

int32 FGameTestGuardMonitor::GetCount(const FString& InKey) const
{
	const FState* State = States.Find(InKey);
	return State ? State->Count : 0;
}

TSharedPtr<FJsonObject> FGameTestGuardMonitor::CountsToJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	for (const TPair<FString, FState>& Pair : States)
	{
		if (Pair.Value.Count > 0)
		{
			Json->SetNumberField(Pair.Key, Pair.Value.Count);
		}
	}
	return Json;
}
