// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sample/ForgeSampleGameMode.h"

#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogForgeSampleMode, Log, All);

const TCHAR* AForgeSampleGameMode::DefaultScreenClassPath = TEXT("/Game/AgentTest/AssetForge/WBP_ForgeSample.WBP_ForgeSample_C");

namespace
{
	const TCHAR* const MissingHint = TEXT("AssetForge 示例界面还没生成：在 Agent 面板 UE 页的 AssetForge 页导入 Test/ 下的配置（WBP_ForgeSample）");

	UUserWidget* ShowScreen(APlayerController* InPlayer, const TSoftClassPtr<UUserWidget>& InClass)
	{
		if (InPlayer == nullptr)
		{
			return nullptr;
		}
		UClass* Class = InClass.LoadSynchronous();
		if (Class == nullptr)
		{
			UE_LOG(LogForgeSampleMode, Warning, TEXT("%s"), MissingHint);
			if (GEngine != nullptr)
			{
				GEngine->AddOnScreenDebugMessage(INDEX_NONE, 15.f, FColor::Orange, MissingHint);
			}
			return nullptr;
		}
		UUserWidget* Widget = CreateWidget<UUserWidget>(InPlayer, Class);
		if (Widget != nullptr)
		{
			Widget->AddToViewport(10);
		}
		return Widget;
	}

	/** 手动打开 / 关掉示例界面：在别的地图里 PIE 时，控制台敲 AssetForge.Sample */
	TWeakObjectPtr<UUserWidget> ConsoleScreen;

	FAutoConsoleCommandWithWorld SampleCommand(
		TEXT("AssetForge.Sample"),
		TEXT("在当前游戏世界里打开 / 关掉 AssetForge 示例测试界面（WBP_ForgeSample），用来练习 GameTest 录制"),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* InWorld)
		{
			if (UUserWidget* Existing = ConsoleScreen.Get())
			{
				Existing->RemoveFromParent();
				ConsoleScreen.Reset();
				return;
			}
			APlayerController* Player = InWorld != nullptr ? InWorld->GetFirstPlayerController() : nullptr;
			if (Player == nullptr)
			{
				UE_LOG(LogForgeSampleMode, Warning, TEXT("AssetForge.Sample 要在游戏世界里用（先 PIE）"));
				return;
			}
			ConsoleScreen = ShowScreen(Player, TSoftClassPtr<UUserWidget>(FSoftObjectPath(AForgeSampleGameMode::DefaultScreenClassPath)));
		}));
}

AForgeSampleGameMode::AForgeSampleGameMode()
{
	DefaultPawnClass = nullptr;
	ScreenClass = TSoftClassPtr<UUserWidget>(FSoftObjectPath(DefaultScreenClassPath));
}

void AForgeSampleGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	Super::HandleStartingNewPlayer_Implementation(NewPlayer);
	if (NewPlayer != nullptr && NewPlayer->IsLocalController())
	{
		ShowScreen(NewPlayer, ScreenClass);
	}
}
