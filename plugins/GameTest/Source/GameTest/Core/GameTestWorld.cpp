// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestWorld.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"

UWorld* GameTestWorld::FindGameWorld()
{
	if (GEngine == nullptr)
	{
		return nullptr;
	}

	UWorld* GameWorld = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (World == nullptr)
		{
			continue;
		}

		if (Context.WorldType == EWorldType::PIE)
		{
			return World;
		}

		if (Context.WorldType == EWorldType::Game && GameWorld == nullptr)
		{
			GameWorld = World;
		}
	}

	return GameWorld;
}

bool GameTestWorld::IsPlayable(const UWorld* InWorld)
{
	return InWorld != nullptr && InWorld->HasBegunPlay();
}

UGameViewportClient* GameTestWorld::FindGameViewport()
{
	UWorld* World = FindGameWorld();
	return World != nullptr ? World->GetGameViewport() : nullptr;
}

void GameTestWorld::FindGameWorlds(TArray<UWorld*>& OutWorlds)
{
	OutWorlds.Reset();
	if (GEngine == nullptr)
	{
		return;
	}

	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (World == nullptr)
		{
			continue;
		}

		if (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game)
		{
			OutWorlds.Add(World);
		}
	}
}

UWorld* GameTestWorld::FindServerWorld()
{
	TArray<UWorld*> Worlds;
	FindGameWorlds(Worlds);
	for (UWorld* World : Worlds)
	{
		const ENetMode NetMode = World->GetNetMode();
		if (NetMode == NM_ListenServer || NetMode == NM_DedicatedServer)
		{
			return World;
		}
	}
	return nullptr;
}

UWorld* GameTestWorld::FindClientWorld(int32 InIndex)
{
	TArray<UWorld*> Worlds;
	FindGameWorlds(Worlds);

	int32 ClientIndex = 0;
	for (UWorld* World : Worlds)
	{
		if (World->GetNetMode() != NM_Client)
		{
			continue;
		}

		if (ClientIndex == InIndex)
		{
			return World;
		}
		++ClientIndex;
	}
	return nullptr;
}

UWorld* GameTestWorld::ResolveWorld(const FString& InClientText, bool bInSpecified, FString& OutError)
{
	if (!bInSpecified || InClientText.IsEmpty())
	{
		if (UWorld* Server = FindServerWorld())
		{
			return Server;
		}
		return FindGameWorld();
	}

	if (InClientText.Equals(TEXT("server"), ESearchCase::IgnoreCase))
	{
		UWorld* Server = FindServerWorld();
		if (Server == nullptr)
		{
			OutError = TEXT("当前会话里没有服务器世界（standalone 没有 server/client 之分）");
		}
		return Server;
	}

	if (!InClientText.IsNumeric())
	{
		OutError = FString::Printf(TEXT("client 只能是 \"server\" 或从 0 起的序号，收到 '%s'"), *InClientText);
		return nullptr;
	}

	const int32 Index = FCString::Atoi(*InClientText);
	UWorld* Client = FindClientWorld(Index);
	if (Client == nullptr)
	{
		OutError = FString::Printf(
			TEXT("没有第 %d 个客户端世界（client 从 0 起；多客户端会话用 pie.start netMode=listenServer 启动）"), Index);
	}
	return Client;
}

bool GameTestWorld::AreAllGameWorldsPlayable()
{
	TArray<UWorld*> Worlds;
	FindGameWorlds(Worlds);
	if (Worlds.Num() == 0)
	{
		return false;
	}

	for (UWorld* World : Worlds)
	{
		if (!IsPlayable(World))
		{
			return false;
		}
	}
	return true;
}

FString GameTestWorld::DescribeWorld(const UWorld* InWorld)
{
	if (InWorld == nullptr)
	{
		return TEXT("（未找到）");
	}

	switch (InWorld->GetNetMode())
	{
	case NM_ListenServer:
	case NM_DedicatedServer:
		return TEXT("server");

	case NM_Client:
	{
		TArray<UWorld*> Worlds;
		FindGameWorlds(Worlds);

		int32 ClientIndex = 0;
		for (UWorld* World : Worlds)
		{
			if (World->GetNetMode() != NM_Client)
			{
				continue;
			}
			if (World == InWorld)
			{
				return FString::Printf(TEXT("client%d"), ClientIndex);
			}
			++ClientIndex;
		}
		return TEXT("client?");
	}

	default:
		return TEXT("standalone");
	}
}
