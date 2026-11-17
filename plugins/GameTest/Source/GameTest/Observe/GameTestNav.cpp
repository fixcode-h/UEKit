// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestNav.h"

#include "Core/GameTestWorld.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "NavigationData.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"

namespace GameTestNavDetail
{
	/** 投影到网格的容差：水平 50 厘米、竖直 200 厘米（角色的位置在胶囊中心，比地面高半个身位）。 */
	const FVector ProjectExtent(50.0, 50.0, 200.0);

	/** 这个世界的导航系统；没有时退到同进程的服务器世界（PIE 监听服务器的客户端世界通常没有导航数据）。 */
	UNavigationSystemV1* FindNavSystem(UWorld& InWorld, UWorld*& OutNavWorld)
	{
		OutNavWorld = &InWorld;
		if (UNavigationSystemV1* Own = FNavigationSystem::GetCurrent<UNavigationSystemV1>(&InWorld))
		{
			return Own;
		}
		UWorld* Server = GameTestWorld::FindServerWorld();
		if (Server != nullptr && Server != &InWorld)
		{
			if (UNavigationSystemV1* ServerNav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(Server))
			{
				OutNavWorld = Server;
				return ServerNav;
			}
		}
		return nullptr;
	}

	/** 角色对应的那份导航数据（按导航体型挑）；没有角色时取默认的那份。 */
	ANavigationData* FindNavData(UNavigationSystemV1& InNav, const APawn* InPawn)
	{
		if (InPawn != nullptr)
		{
			if (ANavigationData* ForPawn = InNav.GetNavDataForProps(InPawn->GetNavAgentPropertiesRef()))
			{
				return ForPawn;
			}
		}
		return InNav.GetNavDataForProps(FNavAgentProperties::DefaultProperties);
	}
}

GameTestNav::EState GameTestNav::GetState(UWorld& InWorld, const APawn* InPawn)
{
	UWorld* NavWorld = nullptr;
	UNavigationSystemV1* Nav = GameTestNavDetail::FindNavSystem(InWorld, NavWorld);
	if (Nav == nullptr || GameTestNavDetail::FindNavData(*Nav, InPawn) == nullptr)
	{
		return EState::Missing;
	}
	return Nav->IsNavigationBuildInProgress() ? EState::Building : EState::Ready;
}

bool GameTestNav::FindPath(UWorld& InWorld, const APawn* InPawn, const FVector& InFrom, const FVector& InTo, TArray<FVector>& OutPoints, FString& OutWhy)
{
	OutPoints.Reset();
	UWorld* NavWorld = nullptr;
	UNavigationSystemV1* Nav = GameTestNavDetail::FindNavSystem(InWorld, NavWorld);
	if (Nav == nullptr)
	{
		OutWhy = TEXT("这个世界没开导航系统");
		return false;
	}
	ANavigationData* Data = GameTestNavDetail::FindNavData(*Nav, InPawn);
	if (Data == nullptr)
	{
		OutWhy = TEXT("没有导航网格（关卡里没放 NavMeshBoundsVolume 或没烘焙）");
		return false;
	}

	FNavLocation Start;
	if (!Nav->ProjectPointToNavigation(InFrom, Start, GameTestNavDetail::ProjectExtent, Data))
	{
		OutWhy = FString::Printf(TEXT("角色所在的位置 (%.0f, %.0f, %.0f) 不在导航网格上"), InFrom.X, InFrom.Y, InFrom.Z);
		return false;
	}
	FNavLocation End;
	if (!Nav->ProjectPointToNavigation(InTo, End, GameTestNavDetail::ProjectExtent, Data))
	{
		OutWhy = FString::Printf(TEXT("终点 (%.0f, %.0f, %.0f) 不在导航网格上"), InTo.X, InTo.Y, InTo.Z);
		return false;
	}

	// 寻路上下文给角色：多体型项目里按角色的体型选网格和过滤器。角色在客户端世界、网格在服务器世界时不给（跨世界对象没意义）。
	AActor* Context = (InPawn != nullptr && InPawn->GetWorld() == NavWorld) ? const_cast<APawn*>(InPawn) : nullptr;
	UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(NavWorld, Start.Location, End.Location, Context);
	if (Path == nullptr || !Path->IsValid() || Path->PathPoints.Num() == 0)
	{
		OutWhy = TEXT("导航网格上找不到从角色到终点的路");
		return false;
	}
	if (Path->IsPartial())
	{
		OutWhy = TEXT("导航网格只能走到半路（两边的网格不连通）");
		return false;
	}
	OutPoints = Path->PathPoints;
	return true;
}

TSharedRef<FJsonObject> GameTestNav::Describe(UWorld* InWorld)
{
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	if (InWorld == nullptr)
	{
		Out->SetStringField(TEXT("state"), StateName(EState::Missing));
		return Out;
	}
	Out->SetStringField(TEXT("state"), StateName(GetState(*InWorld, nullptr)));
	Out->SetStringField(TEXT("world"), GameTestWorld::DescribeWorld(InWorld));
	return Out;
}

const TCHAR* GameTestNav::StateName(EState InState)
{
	switch (InState)
	{
	case EState::Building:
		return TEXT("building");
	case EState::Ready:
		return TEXT("ready");
	default:
		return TEXT("missing");
	}
}
