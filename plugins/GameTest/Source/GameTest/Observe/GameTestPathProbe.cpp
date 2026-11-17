// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestPathProbe.h"

#include "Algo/Reverse.h"
#include "Components/PrimitiveComponent.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"

namespace GameTestPathProbeDetail
{
	/** 格子边长（厘米）：比常见角色胶囊直径（约 70）小，窄门也能找到格子穿过去。 */
	constexpr double CellSize = 50.0;

	/** 最多检查这么多格：超了说明目标太远或根本走不到，算失败，不无限探下去。 */
	constexpr int32 MaxCells = 20000;

	/** 搜索范围：起终点连线的包围盒外扩这么多（厘米）。绕路超过 30 米的情况交给录制轨迹。 */
	constexpr double SearchMargin = 3000.0;

	/** 往下找地面的最大落差（厘米）：再深就算悬崖，不往下跳。 */
	constexpr double MaxDrop = 150.0;

	/** 拉直时最多做这么多次查询：路径很长时剩下的不拉直，照原样走也能到。 */
	constexpr int32 MaxStraightenQueries = 600;

	const FIntPoint Neighbors[8] = {
		FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1),
		FIntPoint(1, 1), FIntPoint(1, -1), FIntPoint(-1, 1), FIntPoint(-1, -1)};

	/** 查询参数：忽略角色自己，碰撞响应照角色根组件的来（和角色移动时扫胶囊用的同一套）。 */
	void MakeQuery(const APawn& InPawn, FCollisionQueryParams& OutParams, FCollisionResponseParams& OutResponse)
	{
		OutParams = FCollisionQueryParams(SCENE_QUERY_STAT(GameTestPathProbe), /*bTraceComplex=*/false, &InPawn);
		if (const UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(InPawn.GetRootComponent()))
		{
			Root->InitSweepCollisionParams(OutParams, OutResponse);
		}
	}
}

FGameTestPathProbe::FAgent FGameTestPathProbe::MakeAgent(const APawn& InPawn)
{
	FAgent Agent;
	float Radius = 0.0f;
	float HalfHeight = 0.0f;
	InPawn.GetSimpleCollisionCylinder(Radius, HalfHeight);
	if (Radius > 1.0f && HalfHeight > 1.0f)
	{
		Agent.Radius = Radius;
		Agent.HalfHeight = HalfHeight;
	}
	if (const UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(InPawn.GetRootComponent()))
	{
		Agent.Channel = Root->GetCollisionObjectType();
	}
	if (const UCharacterMovementComponent* Movement = Cast<UCharacterMovementComponent>(InPawn.GetMovementComponent()))
	{
		Agent.StepHeight = FMath::Max(Movement->MaxStepHeight, 5.0f);
		Agent.WalkableFloorZ = Movement->GetWalkableFloorZ();
	}
	// 台阶不能高过半个身子：下面「抬高半个台阶的胶囊」要留出高度。
	Agent.StepHeight = FMath::Min(Agent.StepHeight, Agent.HalfHeight);
	return Agent;
}

void FGameTestPathProbe::Start(UWorld& InWorld, const APawn& InPawn, const FVector& InFrom, const FVector& InTo, double InGoalRadius)
{
	World = &InWorld;
	Pawn = &InPawn;
	Agent = MakeAgent(InPawn);
	From = InFrom;
	To = InTo;
	GoalRadius = FMath::Max(InGoalRadius, GameTestPathProbeDetail::CellSize);
	Cells.Reset();
	CameFrom.Reset();
	BestG.Reset();
	Open.Reset();
	Path.Reset();
	Why.Reset();
	Evaluated = 0;
	Result = EResult::Running;

	Bounds = FBox2D(FVector2D(InFrom), FVector2D(InFrom));
	Bounds += FVector2D(InTo);
	Bounds = Bounds.ExpandBy(GameTestPathProbeDetail::SearchMargin);

	// 起点格不检查：角色就站在那里，一定站得住。
	StartKey = ToKey(InFrom);
	FCell& StartCell = Cells.Add(StartKey);
	StartCell.FloorZ = InFrom.Z - Agent.HalfHeight;
	StartCell.bChecked = true;
	StartCell.bStandable = true;
	BestG.Add(StartKey, 0.0);
	Open.HeapPush(FOpen{StartKey, FVector2D::Distance(FVector2D(InFrom), FVector2D(InTo)) / GameTestPathProbeDetail::CellSize, 0.0});
}

FGameTestPathProbe::EResult FGameTestPathProbe::Step(int32 InBudget)
{
	if (Result != EResult::Running)
	{
		return Result;
	}
	if (!World.IsValid() || !Pawn.IsValid())
	{
		Why = TEXT("探路时角色或世界没了");
		Result = EResult::Failed;
		return Result;
	}

	int32 Budget = InBudget;
	while (Budget > 0)
	{
		if (Open.Num() == 0)
		{
			Why = FString::Printf(TEXT("探路找不到能走到终点的路（查了 %d 格；周围被挡住，或要跳、爬、开门才过得去）"), Evaluated);
			Result = EResult::Failed;
			return Result;
		}

		FOpen Current;
		Open.HeapPop(Current);
		const double* KnownG = BestG.Find(Current.Key);
		if (KnownG != nullptr && Current.G > *KnownG + KINDA_SMALL_NUMBER)
		{
			continue;
		}

		const FCell CurrentCell = Cells.FindRef(Current.Key);
		const FVector CurrentFloor = Current.Key == StartKey
			? FVector(From.X, From.Y, CurrentCell.FloorZ)
			: CellCenter(Current.Key, CurrentCell.FloorZ);
		if (IsGoal(CurrentFloor))
		{
			BuildPath(Current.Key);
			Result = EResult::Found;
			return Result;
		}

		for (const FIntPoint& Offset : GameTestPathProbeDetail::Neighbors)
		{
			const FIntPoint Next = Current.Key + Offset;
			const FVector2D NextXY = FVector2D(CellCenter(Next, 0.0));
			if (!Bounds.IsInside(NextXY))
			{
				continue;
			}

			FCell* Cell = Cells.Find(Next);
			if (Cell == nullptr)
			{
				FCell Checked;
				CheckCell(Next, CurrentCell.FloorZ, Checked);
				Budget -= 2;
				++Evaluated;
				Cell = &Cells.Add(Next, Checked);
				if (Evaluated > GameTestPathProbeDetail::MaxCells)
				{
					Why = FString::Printf(TEXT("探路查了 %d 格还没找到路，目标太远或走不到"), GameTestPathProbeDetail::MaxCells);
					Result = EResult::Failed;
					return Result;
				}
			}
			if (!Cell->bStandable)
			{
				continue;
			}

			const double StepCost = (Offset.X != 0 && Offset.Y != 0) ? UE_SQRT_2 : 1.0;
			const double Tentative = Current.G + StepCost;
			const double* OldG = BestG.Find(Next);
			if (OldG != nullptr && Tentative >= *OldG)
			{
				continue;
			}

			const FVector NextFloor = CellCenter(Next, Cell->FloorZ);
			--Budget;
			if (!CanWalk(CurrentFloor, NextFloor))
			{
				continue;
			}

			BestG.Add(Next, Tentative);
			CameFrom.Add(Next, Current.Key);
			const double Heuristic = FVector2D::Distance(FVector2D(NextFloor), FVector2D(To)) / GameTestPathProbeDetail::CellSize;
			Open.HeapPush(FOpen{Next, Tentative + Heuristic, Tentative});
		}
	}
	return Result;
}

bool FGameTestPathProbe::CheckCell(const FIntPoint& InKey, double InReferenceFloorZ, FCell& OutCell)
{
	OutCell = FCell();
	OutCell.bChecked = true;
	UWorld* WorldPtr = World.Get();
	const APawn* PawnPtr = Pawn.Get();
	if (WorldPtr == nullptr || PawnPtr == nullptr)
	{
		return false;
	}

	FCollisionQueryParams Params;
	FCollisionResponseParams Response;
	GameTestPathProbeDetail::MakeQuery(*PawnPtr, Params, Response);

	// 从比上一格地面高一个台阶的地方往下找：高过一个台阶的台面本来就上不去，不用找到它。
	const FVector Center = CellCenter(InKey, 0.0);
	const FVector TraceStart(Center.X, Center.Y, InReferenceFloorZ + Agent.StepHeight + 5.0);
	const FVector TraceEnd(Center.X, Center.Y, InReferenceFloorZ - GameTestPathProbeDetail::MaxDrop);
	FHitResult Hit;
	if (!WorldPtr->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, Agent.Channel, Params, Response)
		|| Hit.bStartPenetrating || Hit.ImpactNormal.Z < Agent.WalkableFloorZ)
	{
		return false;
	}
	OutCell.FloorZ = Hit.ImpactPoint.Z;

	// 胶囊抬高半个台阶再查重叠：贴着坡面、台阶边缘不算挡住。
	const float LiftedHalfHeight = Agent.HalfHeight - Agent.StepHeight * 0.5f;
	const FVector CapsuleCenter(Center.X, Center.Y, OutCell.FloorZ + Agent.HalfHeight + 2.0);
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(Agent.Radius * 0.9f, FMath::Max(LiftedHalfHeight, Agent.Radius));
	OutCell.bStandable = !WorldPtr->OverlapBlockingTestByChannel(CapsuleCenter, FQuat::Identity, Agent.Channel, Shape, Params, Response);
	return OutCell.bStandable;
}

bool FGameTestPathProbe::CanWalk(const FVector& InFromFloor, const FVector& InToFloor)
{
	UWorld* WorldPtr = World.Get();
	const APawn* PawnPtr = Pawn.Get();
	if (WorldPtr == nullptr || PawnPtr == nullptr)
	{
		return false;
	}
	const double Rise = InToFloor.Z - InFromFloor.Z;
	if (Rise > Agent.StepHeight + 1.0 || Rise < -GameTestPathProbeDetail::MaxDrop)
	{
		return false;
	}

	FCollisionQueryParams Params;
	FCollisionResponseParams Response;
	GameTestPathProbeDetail::MakeQuery(*PawnPtr, Params, Response);

	// 在两边较高的地面上方水平扫：胶囊底部比地面高半个台阶，跨台阶不会被台阶边缘挡住。
	const double Z = FMath::Max(InFromFloor.Z, InToFloor.Z) + Agent.HalfHeight + 2.0;
	const float LiftedHalfHeight = Agent.HalfHeight - Agent.StepHeight * 0.5f;
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(Agent.Radius * 0.9f, FMath::Max(LiftedHalfHeight, Agent.Radius));
	FHitResult Hit;
	return !WorldPtr->SweepSingleByChannel(Hit, FVector(InFromFloor.X, InFromFloor.Y, Z), FVector(InToFloor.X, InToFloor.Y, Z),
		FQuat::Identity, Agent.Channel, Shape, Params, Response);
}

FVector FGameTestPathProbe::CellCenter(const FIntPoint& InKey, double InFloorZ) const
{
	return FVector(InKey.X * GameTestPathProbeDetail::CellSize, InKey.Y * GameTestPathProbeDetail::CellSize, InFloorZ);
}

FIntPoint FGameTestPathProbe::ToKey(const FVector& InPoint) const
{
	return FIntPoint(
		FMath::RoundToInt(InPoint.X / GameTestPathProbeDetail::CellSize),
		FMath::RoundToInt(InPoint.Y / GameTestPathProbeDetail::CellSize));
}

bool FGameTestPathProbe::IsGoal(const FVector& InFloor) const
{
	const double Horizontal = FVector2D::Distance(FVector2D(InFloor), FVector2D(To));
	const double Vertical = FMath::Abs(InFloor.Z + Agent.HalfHeight - To.Z);
	return Horizontal <= GoalRadius && Vertical <= 250.0;
}

void FGameTestPathProbe::BuildPath(const FIntPoint& InLast)
{
	TArray<FVector> Floors;
	FIntPoint Key = InLast;
	while (Key != StartKey)
	{
		Floors.Add(CellCenter(Key, Cells.FindRef(Key).FloorZ));
		const FIntPoint* Previous = CameFrom.Find(Key);
		if (Previous == nullptr)
		{
			break;
		}
		Key = *Previous;
	}
	Floors.Add(FVector(From.X, From.Y, From.Z - Agent.HalfHeight));
	Algo::Reverse(Floors);
	// 终点用真正的目标点：格子中心离它最多半格，到达判定按真目标算。
	Floors.Add(FVector(To.X, To.Y, To.Z - Agent.HalfHeight));

	Straighten(Floors);
	Path.Reset();
	for (const FVector& Floor : Floors)
	{
		Path.Add(FVector(Floor.X, Floor.Y, Floor.Z + Agent.HalfHeight));
	}
}

void FGameTestPathProbe::Straighten(TArray<FVector>& InOutFloors)
{
	UWorld* WorldPtr = World.Get();
	const APawn* PawnPtr = Pawn.Get();
	if (WorldPtr == nullptr || PawnPtr == nullptr || InOutFloors.Num() <= 2)
	{
		return;
	}
	FCollisionQueryParams Params;
	FCollisionResponseParams Response;
	GameTestPathProbeDetail::MakeQuery(*PawnPtr, Params, Response);

	int32 Queries = 0;
	// 两点之间每半格看一次地面：胶囊扫得通还不够，中间可能是坑。
	auto HasGround = [&](const FVector& InA, const FVector& InB) -> bool
	{
		const double Distance = FVector2D::Distance(FVector2D(InA), FVector2D(InB));
		const int32 Samples = FMath::Max(1, FMath::CeilToInt(Distance / (GameTestPathProbeDetail::CellSize * 0.5)));
		double PreviousZ = InA.Z;
		for (int32 Sample = 1; Sample < Samples; ++Sample)
		{
			const FVector Point = FMath::Lerp(InA, InB, double(Sample) / Samples);
			FHitResult Hit;
			++Queries;
			if (!WorldPtr->LineTraceSingleByChannel(Hit, FVector(Point.X, Point.Y, PreviousZ + Agent.StepHeight + 5.0),
					FVector(Point.X, Point.Y, PreviousZ - GameTestPathProbeDetail::MaxDrop), Agent.Channel, Params, Response)
				|| Hit.ImpactNormal.Z < Agent.WalkableFloorZ)
			{
				return false;
			}
			PreviousZ = Hit.ImpactPoint.Z;
		}
		return true;
	};

	TArray<FVector> Out;
	Out.Add(InOutFloors[0]);
	int32 Anchor = 0;
	while (Anchor < InOutFloors.Num() - 1)
	{
		// 从最远的点往回试：第一个扫得通、中间有地面的就是下一个保留点。
		int32 Next = Anchor + 1;
		for (int32 Candidate = InOutFloors.Num() - 1; Candidate > Anchor + 1 && Queries < GameTestPathProbeDetail::MaxStraightenQueries; --Candidate)
		{
			++Queries;
			if (CanWalkStraight(InOutFloors[Anchor], InOutFloors[Candidate]) && HasGround(InOutFloors[Anchor], InOutFloors[Candidate]))
			{
				Next = Candidate;
				break;
			}
		}
		Out.Add(InOutFloors[Next]);
		Anchor = Next;
	}
	InOutFloors = MoveTemp(Out);
}

bool FGameTestPathProbe::CanWalkStraight(const FVector& InFromFloor, const FVector& InToFloor)
{
	UWorld* WorldPtr = World.Get();
	const APawn* PawnPtr = Pawn.Get();
	if (WorldPtr == nullptr || PawnPtr == nullptr)
	{
		return false;
	}
	FCollisionQueryParams Params;
	FCollisionResponseParams Response;
	GameTestPathProbeDetail::MakeQuery(*PawnPtr, Params, Response);

	// 长距离的高度差不受「一个台阶」限制（坡道），只要求胶囊沿两点连线（抬高半个台阶）扫得通。
	const float LiftedHalfHeight = Agent.HalfHeight - Agent.StepHeight * 0.5f;
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(Agent.Radius * 0.9f, FMath::Max(LiftedHalfHeight, Agent.Radius));
	const FVector Lift(0.0, 0.0, Agent.HalfHeight + 2.0);
	FHitResult Hit;
	return !WorldPtr->SweepSingleByChannel(Hit, InFromFloor + Lift, InToFloor + Lift, FQuat::Identity, Agent.Channel, Shape, Params, Response);
}
