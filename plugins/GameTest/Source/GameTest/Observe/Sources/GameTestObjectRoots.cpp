// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Sources/GameTestObjectRoots.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Misc/Paths.h"
#include "Subsystems/Subsystem.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/** 报歧义时最多列几个候选：够人认出是哪几个，又不至于把返回撑爆。 */
	constexpr int32 MaxListed = 8;

	FString StripClassDecor(FString InName)
	{
		InName.TrimStartAndEndInline();
		// 类路径：取最后一段（/Game/A/BP_X.BP_X_C → BP_X_C）
		int32 Dot = INDEX_NONE;
		if (InName.FindLastChar(TEXT('.'), Dot))
		{
			InName.RightChopInline(Dot + 1);
		}
		if (InName.EndsWith(TEXT("_C")))
		{
			InName.LeftChopInline(2);
		}
		return InName;
	}

	bool NameEquals(const FString& InClassName, const FString& InWanted)
	{
		const FString Have = StripClassDecor(InClassName);
		if (Have.Equals(InWanted, ESearchCase::IgnoreCase))
		{
			return true;
		}
		// 反射名去掉了 A / U 前缀：写 ANpc 也要认得 Npc
		return InWanted.Len() == Have.Len() + 1
			&& (InWanted[0] == TEXT('A') || InWanted[0] == TEXT('U') || InWanted[0] == TEXT('a') || InWanted[0] == TEXT('u'))
			&& InWanted.RightChop(1).Equals(Have, ESearchCase::IgnoreCase);
	}

	APlayerController* LocalController(UWorld& InWorld)
	{
		for (FConstPlayerControllerIterator It = InWorld.GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* Controller = It->Get();
			if (Controller != nullptr && Controller->IsLocalController())
			{
				return Controller;
			}
		}
		return nullptr;
	}
}

bool GameTestObjectRoots::ClassMatches(const UClass* InClass, const FString& InName)
{
	const FString Wanted = StripClassDecor(InName);
	if (Wanted.IsEmpty())
	{
		return false;
	}
	for (const UClass* Class = InClass; Class != nullptr; Class = Class->GetSuperClass())
	{
		if (NameEquals(Class->GetName(), Wanted))
		{
			return true;
		}
	}
	return false;
}

UClass* GameTestObjectRoots::FindClass(const FString& InNameOrPath)
{
	const FString Trimmed = InNameOrPath.TrimStartAndEnd();
	if (Trimmed.StartsWith(TEXT("/")))
	{
		FString Path = Trimmed;
		// 蓝图资产路径（/Game/A/BP_X）补成生成类路径（/Game/A/BP_X.BP_X_C）
		if (!Path.Contains(TEXT(".")))
		{
			const FString Short = FPaths::GetBaseFilename(Path);
			Path += TEXT(".") + Short + TEXT("_C");
		}
		if (UClass* Found = FindObject<UClass>(nullptr, *Path))
		{
			return Found;
		}
	}
	const FString Wanted = StripClassDecor(Trimmed);
	UClass* Best = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		// 热重载、蓝图重编留下的旧类不要
		if (Class == nullptr || Class->HasAnyClassFlags(CLASS_NewerVersionExists) || Class->GetName().StartsWith(TEXT("SKEL_"))
			|| Class->GetName().StartsWith(TEXT("REINST_")))
		{
			continue;
		}
		if (NameEquals(Class->GetName(), Wanted))
		{
			Best = Class;
			// 原生类优先：同名时 C++ 类比蓝图生成类更像作者想要的
			if (Class->HasAnyClassFlags(CLASS_Native))
			{
				break;
			}
		}
	}
	return Best;
}

const TArray<FString>& GameTestObjectRoots::PlayerRootNames()
{
	static const TArray<FString> Names = { TEXT("gi"), TEXT("gs"), TEXT("gm"), TEXT("ps"), TEXT("pc"), TEXT("pawn"), TEXT("hud") };
	return Names;
}

UObject* GameTestObjectRoots::Player(UWorld& InWorld, const FString& InWhich, FString& OutError)
{
	const FString Which = InWhich.ToLower();
	if (Which == TEXT("gi"))
	{
		UGameInstance* Instance = InWorld.GetGameInstance();
		if (Instance == nullptr)
		{
			OutError = TEXT("这个世界没有 GameInstance");
		}
		return Instance;
	}
	if (Which == TEXT("gs"))
	{
		AGameStateBase* State = InWorld.GetGameState();
		if (State == nullptr)
		{
			OutError = TEXT("还没有 GameState（世界还在加载？）");
		}
		return State;
	}
	if (Which == TEXT("gm"))
	{
		AGameModeBase* Mode = InWorld.GetAuthGameMode();
		if (Mode == nullptr)
		{
			OutError = TEXT("GameMode 只在服务器和单机上有，客户端读不到；客户端读 @gs");
		}
		return Mode;
	}

	APlayerController* Controller = LocalController(InWorld);
	if (Controller == nullptr)
	{
		OutError = TEXT("还没有本地 PlayerController（游戏没开、或还在加载）");
		return nullptr;
	}
	if (Which == TEXT("pc"))
	{
		return Controller;
	}
	if (Which == TEXT("ps"))
	{
		APlayerState* State = Controller->PlayerState;
		if (State == nullptr)
		{
			OutError = TEXT("本地玩家还没有 PlayerState（联网时还没同步下来）");
		}
		return State;
	}
	if (Which == TEXT("pawn"))
	{
		APawn* Pawn = Controller->GetPawn();
		if (Pawn == nullptr)
		{
			OutError = TEXT("本地玩家还没有 Pawn（没出生、或在观战）");
		}
		return Pawn;
	}
	if (Which == TEXT("hud"))
	{
		AHUD* Hud = Controller->GetHUD();
		if (Hud == nullptr)
		{
			OutError = TEXT("本地玩家没有 HUD");
		}
		return Hud;
	}
	OutError = FString::Printf(TEXT("没有 @%s，能用的是 %s"), *InWhich, *FString::Join(PlayerRootNames(), TEXT(" ")));
	return nullptr;
}

void GameTestObjectRoots::Subsystems(UWorld& InWorld, TArray<UObject*>& OutSubsystems)
{
	OutSubsystems.Reset();
	TSet<const UObject*> Owners;
	Owners.Add(&InWorld);
	if (GEngine != nullptr)
	{
		Owners.Add(GEngine);
	}
	if (UGameInstance* Instance = InWorld.GetGameInstance())
	{
		Owners.Add(Instance);
		for (ULocalPlayer* Local : Instance->GetLocalPlayers())
		{
			if (Local != nullptr)
			{
				Owners.Add(Local);
			}
		}
	}
	// 按 Outer 认属于谁：各类子系统集合的访问接口在不同引擎版本上不一样，Outer 是一直不变的
	for (TObjectIterator<USubsystem> It; It; ++It)
	{
		USubsystem* Subsystem = *It;
		if (Subsystem != nullptr && !Subsystem->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
			&& Owners.Contains(Subsystem->GetOuter()))
		{
			OutSubsystems.Add(Subsystem);
		}
	}
	OutSubsystems.Sort([](const UObject& A, const UObject& B) { return A.GetClass()->GetName() < B.GetClass()->GetName(); });
}

UObject* GameTestObjectRoots::Subsystem(UWorld& InWorld, const FString& InClassName, FString& OutError)
{
	TArray<UObject*> All;
	Subsystems(InWorld, All);
	TArray<UObject*> Hits;
	for (UObject* Candidate : All)
	{
		if (ClassMatches(Candidate->GetClass(), InClassName))
		{
			Hits.Add(Candidate);
		}
	}
	if (Hits.Num() == 0)
	{
		OutError = FString::Printf(TEXT("没有子系统 %s（类名写错了？这个子系统可能 ShouldCreateSubsystem 返回了 false）"), *InClassName);
		return nullptr;
	}
	// 本地玩家子系统每个本地玩家一份：分屏时取第一个本地玩家的，单人游戏就只有一份
	return Hits[0];
}

AActor* GameTestObjectRoots::UniqueActor(UWorld& InWorld, const FString& InBy, const FString& InValue, FString& OutError)
{
	const FString By = InBy.ToLower();
	if (By != TEXT("tag") && By != TEXT("class") && By != TEXT("name"))
	{
		OutError = FString::Printf(TEXT("@actor[%s=…] 不认识：只能按 tag、class、name 找"), *InBy);
		return nullptr;
	}
	TArray<AActor*> Hits;
	for (TActorIterator<AActor> It(&InWorld); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor == nullptr || !IsValid(Actor))
		{
			continue;
		}
		bool bHit = false;
		if (By == TEXT("tag"))
		{
			bHit = Actor->Tags.ContainsByPredicate([&InValue](const FName& Tag) { return Tag.ToString().Equals(InValue, ESearchCase::IgnoreCase); });
		}
		else if (By == TEXT("class"))
		{
			bHit = ClassMatches(Actor->GetClass(), InValue);
		}
		else
		{
			bHit = DisplayName(Actor).Equals(InValue, ESearchCase::IgnoreCase) || Actor->GetName().Equals(InValue, ESearchCase::IgnoreCase);
		}
		if (bHit)
		{
			Hits.Add(Actor);
		}
	}
	if (Hits.Num() == 1)
	{
		return Hits[0];
	}
	if (Hits.Num() == 0)
	{
		OutError = FString::Printf(TEXT("世界里没有 %s=%s 的 Actor"), *By, *InValue);
		return nullptr;
	}
	TArray<FString> Names;
	for (int32 Index = 0; Index < Hits.Num() && Index < MaxListed; ++Index)
	{
		Names.Add(DisplayName(Hits[Index]));
	}
	OutError = FString::Printf(TEXT("%s=%s 命中 %d 个 Actor（要唯一）：%s%s；改用对象定位器（objects）加标识或 near"),
		*By, *InValue, Hits.Num(), *FString::Join(Names, TEXT("、")), Hits.Num() > MaxListed ? TEXT("…") : TEXT(""));
	return nullptr;
}

FString GameTestObjectRoots::DisplayName(const UObject* InObject)
{
	if (InObject == nullptr)
	{
		return FString();
	}
#if WITH_EDITOR
	if (const AActor* Actor = Cast<AActor>(InObject))
	{
		return Actor->GetActorLabel();
	}
#endif
	return InObject->GetName();
}
