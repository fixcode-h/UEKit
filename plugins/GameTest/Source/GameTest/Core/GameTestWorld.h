// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UGameViewportClient;
class UWorld;

/**
 * "当前正在跑的游戏世界"的唯一判定点。
 *
 * PIE 与独立进程（PlayMode_InNewProcess）在这里不需要分叉：两者都是
 * `EWorldType::PIE` 或 `EWorldType::Game` 的世界，而编辑器空闲时两种都不存在。
 * 所以这一步不需要 `#if WITH_EDITOR`，也不需要编辑器模块——它只读 GEngine。
 */
class GAMETEST_API GameTestWorld
{
public:
	/** 优先 PIE、其次 Game；编辑器空闲态返回 nullptr。 */
	static UWorld* FindGameWorld();

	/** 世界是否已经跑起来（PostBeginPlay 之后）。 */
	static bool IsPlayable(const UWorld* InWorld);

	/** 全部正在跑的游戏世界（PIE/Game，按引擎枚举顺序；多客户端时是服务器 + 各客户端）。 */
	static void FindGameWorlds(TArray<UWorld*>& OutWorlds);

	/** 多客户端会话里的服务器世界（NM_ListenServer / NM_DedicatedServer）；没有时返回 nullptr。 */
	static UWorld* FindServerWorld();

	/** 多客户端会话里的第 N 个客户端世界（NM_Client，从 0 起）；没有时返回 nullptr。 */
	static UWorld* FindClientWorld(int32 InIndex);

	/**
	 * 按步骤里的 `client` 字段寻址世界：`"server"` 或从 0 起的客户端序号。
	 * 缺省（bInSpecified=false）取服务器世界（多世界）或唯一的世界（单机）。
	 */
	static UWorld* ResolveWorld(const FString& InClientText, bool bInSpecified, FString& OutError);

	/** 全部游戏世界都已可玩且至少有一个——多客户端 pie.start 的等待条件。 */
	static bool AreAllGameWorldsPlayable();

	/** 世界的可读标签（`server` / `client0` / `standalone` / `（未找到）`），进消息与 payload。 */
	static FString DescribeWorld(const UWorld* InWorld);

	static UGameViewportClient* FindGameViewport();
};
