// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class APawn;
class FJsonObject;
class UWorld;

/**
 * 引擎导航网格的只读查询：有没有、是不是还在生成、从 A 到 B 能不能算出完整路径。
 *
 * 只查不建：项目没烘焙导航网格时不去现场生成（导航网格默认是静态的，游戏里重新生成要改项目设置，
 * PIE 和独立进程表现也不一样），没有网格的情况交给 `FGameTestPathProbe` 按需探路。
 *
 * 联机：导航网格默认只在服务器有。查不到时退到同进程里的服务器世界（PIE 监听服务器）——
 * 两边坐标一样，算出的路径交给客户端的角色走。
 */
class GAMETEST_API GameTestNav
{
public:
	enum class EState : uint8
	{
		/** 这个世界（和同进程的服务器世界）都没有导航系统或导航数据。 */
		Missing,
		/** 有导航数据，但还在生成（动态生成、World Partition 刚加载）。 */
		Building,
		Ready,
	};

	/** 给 `InPawn` 用的导航数据现在的状态（按角色的导航体型找对应的那份网格）。 */
	static EState GetState(UWorld& InWorld, const APawn* InPawn);

	/**
	 * 从 `InFrom` 到 `InTo` 算一条完整路径（点是网格表面上的，按角色的导航体型）。
	 * 起点或终点投影不到网格上、路径是 partial（只能走到半路）都算失败，`OutWhy` 写清楚是哪种。
	 */
	static bool FindPath(UWorld& InWorld, const APawn* InPawn, const FVector& InFrom, const FVector& InTo, TArray<FVector>& OutPoints, FString& OutWhy);

	/** `gt.health` 的 `nav`：`{state, world}`（state = missing / building / ready）。 */
	static TSharedRef<FJsonObject> Describe(UWorld* InWorld);

	static const TCHAR* StateName(EState InState);
};
