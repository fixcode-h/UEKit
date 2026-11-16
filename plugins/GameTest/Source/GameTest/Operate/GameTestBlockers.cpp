// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/GameTestBlockers.h"

#include "Core/GameTestTypes.h"
#include "GameTest.h"
#include "GameTestCompat.h" // FStringOutputDevice 的头在 5.4 和之后的版本里位置不同
#include "Observe/GameTestSlate.h"

#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/** 扫描间隔：界面弹出来到被处理晚 0.2 秒没关系，每帧遍历所有 UserWidget 不值得。 */
	constexpr double BlockerScanInterval = 0.2;

	/** 处理后过多久复查还在不在。 */
	constexpr double BlockerRecheckSeconds = 1.0;

	FString StripClassSuffix(const FString& InName)
	{
		return InName.EndsWith(TEXT("_C")) ? InName.LeftChop(2) : InName;
	}

	/** 规则里的 widget 和这个类对得上：类名（带不带 _C）或资产包路径（`/Game/UI/WBP_X`，可带 `.WBP_X_C`）。 */
	bool BlockerClassMatches(const UClass& InClass, const FString& InPattern)
	{
		if (InPattern.StartsWith(TEXT("/")))
		{
			FString Package = InPattern;
			int32 Dot = INDEX_NONE;
			if (Package.FindChar(TEXT('.'), Dot))
			{
				Package.LeftInline(Dot);
			}
			return InClass.GetPackage()->GetName().Equals(Package, ESearchCase::IgnoreCase);
		}
		return StripClassSuffix(InClass.GetName()).Equals(StripClassSuffix(InPattern), ESearchCase::IgnoreCase);
	}

	/** 真在屏上：在游戏界面里、整条父链可见、有尺寸（对象池里藏着的实例也有 Slate 控件，不算）。 */
	bool IsBlockerOnScreen(const UUserWidget& InWidget)
	{
		const TSharedPtr<SWidget> Slate = InWidget.GetCachedWidget();
		if (!Slate.IsValid() || !GameTestSlate::IsInGameUi(Slate))
		{
			return false;
		}
		for (TSharedPtr<SWidget> Current = Slate; Current.IsValid(); Current = Current->GetParentWidget())
		{
			if (!Current->GetVisibility().IsVisible())
			{
				return false;
			}
		}
		const FVector2D Size = FVector2D(Slate->GetCachedGeometry().GetLocalSize());
		return Size.X >= 1.0 && Size.Y >= 1.0;
	}

	/** 正在等的这一步要操作的就是这个界面（选择器里写了它的类名）：不抢着处理。 */
	bool StepTargetsWidget(const FGameTestStep* InStep, const UUserWidget& InWidget)
	{
		if (InStep == nullptr || InStep->Target.IsEmpty())
		{
			return false;
		}
		return InStep->Target.Contains(StripClassSuffix(InWidget.GetClass()->GetName()), ESearchCase::IgnoreCase);
	}

	/** 调界面上的无参函数（有返回值可以，有输入参数不行：规则里没法给参数）。 */
	bool CallBlockerFunction(UUserWidget& InWidget, const FString& InName, FString& OutMessage)
	{
		UFunction* Function = InWidget.FindFunction(FName(*InName));
		if (Function == nullptr)
		{
			OutMessage = FString::Printf(TEXT("%s 上没有函数 %s（看看规则里的函数名，蓝图函数要用蓝图里的名字）"),
				*InWidget.GetClass()->GetName(), *InName);
			return false;
		}
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				OutMessage = FString::Printf(TEXT("函数 %s 要参数（%s），阻挡界面规则只能调无参函数；可以在蓝图里包一个无参的"),
					*InName, *It->GetName());
				return false;
			}
		}

		if (Function->ParmsSize == 0)
		{
			InWidget.ProcessEvent(Function, nullptr);
		}
		else
		{
			// 只有返回值：给它一块初始化好的参数区，调完析构
			uint8* Params = static_cast<uint8*>(FMemory_Alloca_Aligned(Function->ParmsSize, Function->GetMinAlignment()));
			FMemory::Memzero(Params, Function->ParmsSize);
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->InitializeValue_InContainer(Params);
			}
			InWidget.ProcessEvent(Function, Params);
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->DestroyValue_InContainer(Params);
			}
		}
		OutMessage = FString::Printf(TEXT("调用了 %s"), *InName);
		return true;
	}

	/**
	 * 执行指令：先交给本地玩家控制器（GM、作弊管理器、PlayerController 上的 exec 都在这条路上），
	 * 没有玩家控制器再交给引擎。
	 */
	bool RunBlockerCommand(UWorld* InWorld, const FString& InCommand, FString& OutMessage)
	{
		if (InWorld != nullptr)
		{
			if (APlayerController* Controller = InWorld->GetFirstPlayerController())
			{
				const FString Output = Controller->ConsoleCommand(InCommand, /*bWriteToLog=*/true);
				OutMessage = Output.IsEmpty()
					? FString::Printf(TEXT("执行了 %s"), *InCommand)
					: FString::Printf(TEXT("执行了 %s：%s"), *InCommand, *Output.Left(200));
				return true;
			}
		}
		if (GEngine == nullptr)
		{
			OutMessage = TEXT("引擎还没起来，执行不了指令");
			return false;
		}
		FStringOutputDevice Output;
		const bool bHandled = GEngine->Exec(InWorld, *InCommand, Output);
		OutMessage = FString::Printf(TEXT("执行了 %s（%s）%s"), *InCommand, bHandled ? TEXT("有命令认领") : TEXT("没有命令认领"),
			*FString(*Output).Left(200));
		return bHandled;
	}
}

void FGameTestBlockerGuard::Begin(const TArray<FGameTestBlockerRule>& InRules)
{
	Rules.Reset();
	for (const FGameTestBlockerRule& Rule : InRules)
	{
		if (Rule.bEnabled && !Rule.Widget.IsEmpty())
		{
			Rules.Add(Rule);
		}
	}
	Handled.Reset();
	FirstSeen.Reset();
	ReportedStuck.Reset();
	NextScanAt = 0.0;
}

void FGameTestBlockerGuard::End()
{
	Rules.Reset();
	Handled.Reset();
	FirstSeen.Reset();
	ReportedStuck.Reset();
}

int32 FGameTestBlockerGuard::FindRule(const UUserWidget& InWidget) const
{
	for (const UClass* Class = InWidget.GetClass(); Class != nullptr && Class != UUserWidget::StaticClass(); Class = Class->GetSuperClass())
	{
		for (int32 Index = 0; Index < Rules.Num(); ++Index)
		{
			if (BlockerClassMatches(*Class, Rules[Index].Widget))
			{
				return Index;
			}
		}
	}
	return INDEX_NONE;
}

void FGameTestBlockerGuard::Tick(double InNowSeconds, UWorld* InWorld, const FGameTestStep* InStepInFlight, TArray<TSharedPtr<FJsonObject>>& OutEvents)
{
	if (Rules.Num() == 0 || InWorld == nullptr || InNowSeconds < NextScanAt)
	{
		return;
	}
	NextScanAt = InNowSeconds + BlockerScanInterval;

	for (TObjectIterator<UUserWidget> It; It; ++It)
	{
		UUserWidget* Widget = *It;
		if (Widget == nullptr || Widget->IsTemplate() || Widget->GetWorld() != InWorld)
		{
			continue;
		}
		const int32 RuleIndex = FindRule(*Widget);
		if (RuleIndex == INDEX_NONE || !IsBlockerOnScreen(*Widget))
		{
			continue;
		}
		const TWeakObjectPtr<UUserWidget> Key(Widget);

		if (const double* HandledAt = Handled.Find(Key))
		{
			// 处理过还在屏上：记一次给人看，不反复处理
			if (InNowSeconds - *HandledAt >= BlockerRecheckSeconds && !ReportedStuck.Contains(Key))
			{
				ReportedStuck.Add(Key);
				TSharedPtr<FJsonObject> Event = MakeShared<FJsonObject>();
				Event->SetStringField(TEXT("widget"), Widget->GetClass()->GetName());
				Event->SetStringField(TEXT("rule"), Rules[RuleIndex].Widget);
				Event->SetStringField(TEXT("action"), GameTestRunConfig::ActionName(Rules[RuleIndex].Action));
				Event->SetBoolField(TEXT("ok"), false);
				Event->SetStringField(TEXT("message"), TEXT("处理后 1 秒还在屏上：规则没把它关掉（换成 close，或换一个真能关掉它的函数 / 指令）"));
				UE_LOG(LogGameTest, Warning, TEXT("阻挡界面 %s 处理后还在屏上"), *Widget->GetClass()->GetName());
				OutEvents.Add(Event);
			}
			continue;
		}
		if (StepTargetsWidget(InStepInFlight, *Widget))
		{
			continue;
		}

		const double SeenAt = FirstSeen.FindOrAdd(Key, InNowSeconds);
		if (InNowSeconds - SeenAt < Rules[RuleIndex].DelayMs / 1000.0)
		{
			continue;
		}

		Handled.Add(Key, InNowSeconds);
		OutEvents.Add(Handle(*Widget, Rules[RuleIndex], InWorld));
	}
}

TSharedPtr<FJsonObject> FGameTestBlockerGuard::Handle(UUserWidget& InWidget, const FGameTestBlockerRule& InRule, UWorld* InWorld) const
{
	const FString ClassName = InWidget.GetClass()->GetName();
	FString Message;
	bool bOk = true;
	switch (InRule.Action)
	{
	case EGameTestBlockerAction::Call:
		bOk = CallBlockerFunction(InWidget, InRule.Function, Message);
		break;
	case EGameTestBlockerAction::Command:
		bOk = RunBlockerCommand(InWorld, InRule.Command, Message);
		break;
	default:
		// RemoveFromParent 对挂在视口或 UMG 面板里的都管用；直接挂在 Slate 上的（没有 UMG 父面板）移不掉，再折叠兜底
		InWidget.RemoveFromParent();
		if (IsBlockerOnScreen(InWidget))
		{
			InWidget.SetVisibility(ESlateVisibility::Collapsed);
			Message = TEXT("移除后还在 Slate 上，已折叠");
		}
		else
		{
			Message = TEXT("已移除");
		}
		break;
	}

	TSharedPtr<FJsonObject> Event = MakeShared<FJsonObject>();
	Event->SetStringField(TEXT("widget"), ClassName);
	Event->SetStringField(TEXT("rule"), InRule.Widget);
	Event->SetStringField(TEXT("action"), GameTestRunConfig::ActionName(InRule.Action));
	Event->SetBoolField(TEXT("ok"), bOk);
	Event->SetStringField(TEXT("message"), Message);
	if (!InRule.Note.IsEmpty())
	{
		Event->SetStringField(TEXT("note"), InRule.Note);
	}
	if (bOk)
	{
		UE_LOG(LogGameTest, Display, TEXT("阻挡界面 %s：%s %s"), *ClassName, GameTestRunConfig::ActionName(InRule.Action), *Message);
	}
	else
	{
		UE_LOG(LogGameTest, Warning, TEXT("阻挡界面 %s 处理失败：%s"), *ClassName, *Message);
	}
	return Event;
}
