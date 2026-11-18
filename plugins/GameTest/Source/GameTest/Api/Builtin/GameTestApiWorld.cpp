// Copyright Epic Games, Inc. All Rights Reserved.

// 自带接口：世界和网络（协议第 8.1 节 World.* / Net.*）。

#include "Api/Builtin/GameTestApiArgs.h"
#include "Api/Builtin/GameTestApiLoading.h"
#include "Api/GameTestApi.h"
#include "Core/GameTestWorld.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Observe/Sources/GameTestObjectRoots.h"
#include "UObject/UObjectGlobals.h"

namespace GA = GameTestApiArgs;
using GameTestApiArgs::Opt;
using GameTestApiArgs::Req;

namespace GameTestApiLoading
{
	bool IsDone(const UWorld* InWorld, FString& OutWhy)
	{
		if (InWorld == nullptr || !GameTestWorld::IsPlayable(InWorld))
		{
			OutWhy = TEXT("世界还没开始玩（切图中）");
			return false;
		}
		if (IsAsyncLoading())
		{
			OutWhy = TEXT("还有包在异步加载");
			return false;
		}
		for (const ULevelStreaming* Level : InWorld->GetStreamingLevels())
		{
			if (Level == nullptr)
			{
				continue;
			}
			// 只等「应该加载 / 应该显示」的：远处没触发的流送关卡本来就不该加载
			if ((Level->ShouldBeLoaded() && !Level->IsLevelLoaded()) || (Level->ShouldBeVisible() && !Level->IsLevelVisible()))
			{
				OutWhy = FString::Printf(TEXT("流送关卡 %s 还没加载完"), *FPackageName::GetShortName(Level->GetWorldAssetPackageName()));
				return false;
			}
		}
		return true;
	}
}

namespace
{
	using FCall = FGameTestApiCall;
	using FCtx = FGameTestFlowContext;
	using EState = EGameTestCallState;

	FString MapName(const UWorld* InWorld)
	{
		// PIE 的地图名带 UEDPIE_0_ 前缀：去掉，脚本里写的是地图本名
		FString Name = InWorld ? InWorld->GetMapName() : FString();
		if (InWorld != nullptr)
		{
			Name.RemoveFromStart(InWorld->StreamingLevelsPrefix);
		}
		return Name;
	}

	EState Streaming(FCall& C, FCtx& X)
	{
		const UWorld* World = GA::World(X, C);
		if (World == nullptr)
		{
			return EState::Failed;
		}
		const FString Want = GA::Text(C.Arg(0));
		int32 Total = 0, Loaded = 0, Visible = 0;
		for (const ULevelStreaming* Level : World->GetStreamingLevels())
		{
			if (Level == nullptr)
			{
				continue;
			}
			const FString Name = FPackageName::GetShortName(Level->GetWorldAssetPackageName());
			if (!Want.IsEmpty() && !Name.Contains(Want))
			{
				continue;
			}
			++Total;
			Loaded += Level->IsLevelLoaded() ? 1 : 0;
			Visible += Level->IsLevelVisible() ? 1 : 0;
		}
		if (!Want.IsEmpty() && Total == 0)
		{
			return C.Fail(TEXT("object_not_found"), FString::Printf(TEXT("没有名字含 %s 的流送关卡"), *Want));
		}
		TMap<FString, FGameTestValue> Out;
		Out.Add(TEXT("loaded"), FGameTestValue::FromBool(Loaded == Total));
		Out.Add(TEXT("visible"), FGameTestValue::FromBool(Visible == Total));
		Out.Add(TEXT("count"), FGameTestValue::FromNumber(Total));
		return C.Done(FGameTestValue::FromMap(MoveTemp(Out)));
	}

	EState Open(FCall& C, FCtx& X)
	{
		UWorld* World = GA::World(X, C);
		if (World == nullptr)
		{
			return EState::Failed;
		}
		const FString Map = GA::Text(C.Arg(0));
		if (Map.IsEmpty())
		{
			GA::ArgError(C, 0, TEXT("map"), TEXT("要写地图名或路径（/Game/Maps/Town）"));
			return EState::Failed;
		}
		const TWeakObjectPtr<UWorld> Old = World;
		UGameplayStatics::OpenLevel(World, FName(*Map), true, GA::Text(C.Arg(1)));
		FCall* Call = &C;
		// 等到换了一个世界、并且加载完。超时由步骤的 timeoutMs 管。
		C.Poll = [Call, Old](double)
		{
			UWorld* Now = GameTestWorld::FindGameWorld();
			FString Why;
			if (Now == nullptr || Now == Old.Get() || !GameTestApiLoading::IsDone(Now, Why))
			{
				return EState::Pending;
			}
			return Call->Done(FGameTestValue::FromString(MapName(Now)));
		};
		return EState::Pending;
	}

	EState NetMode(FCall& C, FCtx& X)
	{
		const UWorld* World = GA::World(X, C);
		if (World == nullptr)
		{
			return EState::Failed;
		}
		switch (World->GetNetMode())
		{
		case NM_ListenServer:
			return C.Done(FGameTestValue::FromString(TEXT("listen")));
		case NM_DedicatedServer:
			return C.Done(FGameTestValue::FromString(TEXT("dedicated")));
		case NM_Client:
			return C.Done(FGameTestValue::FromString(TEXT("client")));
		default:
			return C.Done(FGameTestValue::FromString(TEXT("standalone")));
		}
	}
}

namespace GameTestBuiltin
{
	void RegisterApiWorld()
	{
		const TCHAR* Cat = TEXT("世界");
		GameTestApis::Add(TEXT("World.Map"), Cat, TEXT("当前地图名（去掉 PIE 前缀）"), {}, TEXT("string"), true,
			[](FCall& C, FCtx& X)
			{
				const UWorld* World = GA::World(X, C);
				return World ? C.Done(FGameTestValue::FromString(MapName(World))) : EState::Failed;
			});
		GameTestApis::Add(TEXT("World.IsLoaded"), Cat, TEXT("地图、该加载的流送关卡都加载完，且没有异步加载中的包"), {}, TEXT("bool"), true,
			[](FCall& C, FCtx& X)
			{
				FString Why;
				return C.Done(FGameTestValue::FromBool(GameTestApiLoading::IsDone(X.World, Why)));
			});
		GameTestApis::Add(TEXT("World.Streaming"), Cat, TEXT("流送关卡加载、显示了没有（不写 level 看全部）"),
			{ Opt(TEXT("level"), TEXT("string"), TEXT("关卡名的一部分")) }, TEXT("{loaded, visible, count}"), true, &Streaming);
		GameTestApis::Add(TEXT("World.TimeSeconds"), Cat, TEXT("游戏时间（受暂停、时间缩放影响）"), {}, TEXT("number"), true,
			[](FCall& C, FCtx& X)
			{
				const UWorld* World = GA::World(X, C);
				return World ? C.Done(FGameTestValue::FromNumber(World->GetTimeSeconds())) : EState::Failed;
			});
		GameTestApis::Add(TEXT("World.RealSeconds"), Cat, TEXT("真实时间（不受暂停、时间缩放影响）"), {}, TEXT("number"), true,
			[](FCall& C, FCtx& X)
			{
				const UWorld* World = GA::World(X, C);
				return World ? C.Done(FGameTestValue::FromNumber(World->GetRealTimeSeconds())) : EState::Failed;
			});
		GameTestApis::Add(TEXT("World.IsPaused"), Cat, TEXT("游戏暂停了没有"), {}, TEXT("bool"), true,
			[](FCall& C, FCtx& X)
			{
				const UWorld* World = GA::World(X, C);
				return World ? C.Done(FGameTestValue::FromBool(World->IsPaused())) : EState::Failed;
			});
		GameTestApis::Add(TEXT("World.TimeDilation"), Cat, TEXT("全局时间缩放"), {}, TEXT("number"), true,
			[](FCall& C, FCtx& X)
			{
				const UWorld* World = GA::World(X, C);
				const AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
				return Settings ? C.Done(FGameTestValue::FromNumber(Settings->TimeDilation)) : EState::Failed;
			});
		GameTestApis::Add(TEXT("World.Open"), Cat, TEXT("打开地图，等到新地图加载完（返回新地图名）"),
			{ Req(TEXT("map"), TEXT("string")), Opt(TEXT("options"), TEXT("string"), TEXT("?listen 这类 URL 选项")) }, TEXT("string"), false, &Open, true);
		GameTestApis::Add(TEXT("World.SetPaused"), Cat, TEXT("暂停 / 继续游戏"), { Req(TEXT("paused"), TEXT("bool")) }, TEXT(""), false,
			[](FCall& C, FCtx& X)
			{
				UWorld* World = GA::World(X, C);
				if (World == nullptr)
				{
					return EState::Failed;
				}
				return UGameplayStatics::SetGamePaused(World, C.Arg(0).Truthy()) ? C.Done(FGameTestValue::Null())
					: C.Fail(TEXT("unresolved"), TEXT("暂停被拒绝了（GameMode 的 CanPause / 客户端不能暂停）"));
			});
		GameTestApis::Add(TEXT("World.SetTimeDilation"), Cat, TEXT("全局时间缩放（0.0001–20）"), { Req(TEXT("x"), TEXT("number")) }, TEXT(""), false,
			[](FCall& C, FCtx& X)
			{
				UWorld* World = GA::World(X, C);
				double Value = 1.0;
				if (World == nullptr)
				{
					return EState::Failed;
				}
				if (!C.Arg(0).TryNumber(Value) || Value <= 0.0)
				{
					GA::ArgError(C, 0, TEXT("x"), TEXT("要正数"));
					return EState::Failed;
				}
				UGameplayStatics::SetGlobalTimeDilation(World, static_cast<float>(Value));
				return C.Done(FGameTestValue::Null());
			});

		const TCHAR* NetCat = TEXT("网络");
		GameTestApis::Add(TEXT("Net.Mode"), NetCat, TEXT("standalone / listen / dedicated / client"), {}, TEXT("string"), true, &NetMode);
		GameTestApis::Add(TEXT("Net.IsServer"), NetCat, TEXT("当前世界是不是服务器（单机也算）"), {}, TEXT("bool"), true,
			[](FCall& C, FCtx& X)
			{
				const UWorld* World = GA::World(X, C);
				return World ? C.Done(FGameTestValue::FromBool(World->GetNetMode() != NM_Client)) : EState::Failed;
			});
		GameTestApis::Add(TEXT("Net.PlayerCount"), NetCat, TEXT("GameState 里的玩家数"), {}, TEXT("int"), true,
			[](FCall& C, FCtx& X)
			{
				const UWorld* World = GA::World(X, C);
				const AGameStateBase* State = World ? World->GetGameState() : nullptr;
				return State ? C.Done(FGameTestValue::FromNumber(State->PlayerArray.Num()))
					: C.Fail(TEXT("engine_unavailable"), TEXT("还没有 GameState"));
			});
		GameTestApis::Add(TEXT("Net.Ping"), NetCat, TEXT("本地玩家的延迟（毫秒）"), { Opt(TEXT("client"), TEXT("int")) }, TEXT("number"), true,
			[](FCall& C, FCtx& X)
			{
				FString Error;
				UWorld* World = GA::ClientWorld(C.Arg(0), X, Error);
				const APlayerState* State = World ? Cast<APlayerState>(GameTestObjectRoots::Player(*World, TEXT("ps"), Error)) : nullptr;
				return State ? C.Done(FGameTestValue::FromNumber(State->GetPingInMilliseconds())) : C.Fail(TEXT("object_not_found"), Error);
			});
	}
}
