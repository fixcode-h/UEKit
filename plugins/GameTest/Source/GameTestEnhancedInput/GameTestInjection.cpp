// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameTestInjection.h"

#include "Core/GameTestWorld.h"
#include "GameTestCompat.h"

#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/** 注入/光标都需要"游戏世界的本地玩家"——拿不到就是没起 PIE 或没接管。 */
	ULocalPlayer* FindLocalPlayer(UWorld& InWorld, FString& OutError)
	{
		ULocalPlayer* LocalPlayer = InWorld.GetFirstLocalPlayerFromController();
		if (LocalPlayer == nullptr)
		{
			OutError = TEXT("游戏世界里没有本地玩家（PIE 没跑起来，或玩家还没接管）");
		}
		return LocalPlayer;
	}

	UEnhancedInputLocalPlayerSubsystem* FindInputSubsystem(UWorld& InWorld, FString& OutError)
	{
		ULocalPlayer* LocalPlayer = FindLocalPlayer(InWorld, OutError);
		if (LocalPlayer == nullptr)
		{
			return nullptr;
		}

		UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
		if (Subsystem == nullptr)
		{
			OutError = TEXT("本地玩家上没有 EnhancedInput 子系统（项目的输入栈不是 EnhancedInput？）");
		}
		return Subsystem;
	}

#if !GAMETEST_HAS_INJECTION_QUERY
	/** 没有引擎查询接口的版本（5.4）：自己记每个子系统上哪些动作正在持续注入，键用弱引用，PIE 结束对象没了自然失效 */
	TMap<TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem>, TSet<TWeakObjectPtr<const UInputAction>>>& InjectedActions()
	{
		static TMap<TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem>, TSet<TWeakObjectPtr<const UInputAction>>> Map;
		return Map;
	}
#endif

	bool HasInjection(UEnhancedInputLocalPlayerSubsystem& InSubsystem, const UInputAction& InAction)
	{
#if GAMETEST_HAS_INJECTION_QUERY
		return InSubsystem.HasContinuousInputInjectionForAction(&InAction);
#else
		const TSet<TWeakObjectPtr<const UInputAction>>* Actions = InjectedActions().Find(&InSubsystem);
		return Actions != nullptr && Actions->Contains(&InAction);
#endif
	}

	void MarkInjection(UEnhancedInputLocalPlayerSubsystem& InSubsystem, const UInputAction& InAction, bool bInjecting)
	{
#if !GAMETEST_HAS_INJECTION_QUERY
		if (bInjecting)
		{
			InjectedActions().FindOrAdd(&InSubsystem).Add(&InAction);
		}
		else if (TSet<TWeakObjectPtr<const UInputAction>>* Actions = InjectedActions().Find(&InSubsystem))
		{
			Actions->Remove(&InAction);
		}
#endif
	}

	/** 把标量注入值填进该动作 ValueType 的各分量。 */
	FInputActionValue MakeInjectionValue(const UInputAction& InAction, double InValue)
	{
		const float Value = static_cast<float>(InValue);
		switch (InAction.ValueType)
		{
		case EInputActionValueType::Boolean:
			return FInputActionValue(Value != 0.0f);
		case EInputActionValueType::Axis1D:
			return FInputActionValue(EInputActionValueType::Axis1D, FVector(Value, 0.0, 0.0));
		case EInputActionValueType::Axis2D:
			// 只给一个标量时，两个轴都取该值——需要分别控制时给动作加 x/y 字段，别在这里猜。
			return FInputActionValue(EInputActionValueType::Axis2D, FVector(Value, Value, 0.0));
		case EInputActionValueType::Axis3D:
			return FInputActionValue(EInputActionValueType::Axis3D, FVector(Value, Value, Value));
		default:
			return FInputActionValue(EInputActionValueType::Axis1D, FVector(Value, 0.0, 0.0));
		}
	}
}

UInputAction* GameTestInjection::ResolveInputAction(const FString& InActionPath, FString& OutError)
{
	if (InActionPath.IsEmpty())
	{
		OutError = TEXT("需要 action（输入资产路径，例如 /Game/Input/IA_Throttle）");
		return nullptr;
	}

	UInputAction* Action = nullptr;

	if (InActionPath.StartsWith(TEXT("/")))
	{
		FString FullPath = InActionPath;
		if (!FullPath.Contains(TEXT(".")))
		{
			// 资产路径补上对象名：/Game/Input/IA_Throttle → /Game/Input/IA_Throttle.IA_Throttle
			FullPath = FString::Printf(TEXT("%s.%s"), *InActionPath, *FPaths::GetCleanFilename(InActionPath));
		}

		Action = Cast<UInputAction>(StaticLoadObject(UInputAction::StaticClass(), nullptr, *FullPath));
	}
	else
	{
		// 裸资产名：在已加载对象里找（场景里也可以直接写 /Game/... 的完整路径）。
		for (TObjectIterator<UInputAction> Iterator; Iterator; ++Iterator)
		{
			UInputAction* Candidate = *Iterator;
			if (IsValid(Candidate) && Candidate->GetName().Equals(InActionPath, ESearchCase::IgnoreCase))
			{
				Action = Candidate;
				break;
			}
		}
	}

	if (Action == nullptr)
	{
		TArray<FString> Candidates;
		for (TObjectIterator<UInputAction> Iterator; Iterator; ++Iterator)
		{
			if (IsValid(*Iterator))
			{
				Candidates.Add((*Iterator)->GetName());
			}
		}
		Candidates.Sort();

		OutError = FString::Printf(TEXT("找不到输入动作 '%s'（要写 /Game/... 的资产路径）；已加载的候选：%s"),
			*InActionPath,
			Candidates.Num() > 0 ? *FString::Join(Candidates, TEXT("、")) : TEXT("（一个都没有）"));
	}

	return Action;
}

bool GameTestInjection::StartInjection(UWorld& InWorld, const UInputAction& InAction, double InValue, FString& OutMessage)
{
	FString Error;
	UEnhancedInputLocalPlayerSubsystem* Subsystem = FindInputSubsystem(InWorld, Error);
	if (Subsystem == nullptr)
	{
		OutMessage = Error;
		return false;
	}

	Subsystem->StartContinuousInputInjectionForAction(&InAction, MakeInjectionValue(InAction, InValue), TArray<UInputModifier*>(), TArray<UInputTrigger*>());
	MarkInjection(*Subsystem, InAction, true);

	OutMessage = FString::Printf(TEXT("已开始持续注入 %s（值 %.3f，每 tick 重发）"), *InAction.GetName(), InValue);
	return true;
}

bool GameTestInjection::StopInjection(UWorld& InWorld, const UInputAction& InAction, FString& OutMessage)
{
	FString Error;
	UEnhancedInputLocalPlayerSubsystem* Subsystem = FindInputSubsystem(InWorld, Error);
	if (Subsystem == nullptr)
	{
		OutMessage = Error;
		return false;
	}

	// 对没在注入的动作停一次是静默无操作（引擎的 Remove），所以先查再停：
	// "我明明没注入却报成功"会让 AI 误判自己刚才那步输入生效过。
	if (!HasInjection(*Subsystem, InAction))
	{
		OutMessage = FString::Printf(TEXT("%s 当前没有在注入（先 input.start 再停）"), *InAction.GetName());
		return false;
	}

	Subsystem->StopContinuousInputInjectionForAction(&InAction);
	MarkInjection(*Subsystem, InAction, false);
	OutMessage = FString::Printf(TEXT("已停止注入 %s"), *InAction.GetName());
	return true;
}

bool GameTestInjection::IsInjecting(UWorld& InWorld, const UInputAction& InAction)
{
	FString Error;
	UEnhancedInputLocalPlayerSubsystem* Subsystem = FindInputSubsystem(InWorld, Error);
	return Subsystem != nullptr && HasInjection(*Subsystem, InAction);
}
