// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

class APawn;
class UWorld;

/**
 * 按需探路：项目没有导航网格时，现场用物理查询在角色和目标之间做 A*。
 *
 * **为什么不现场生成引擎的导航网格**：网格默认是静态的（只在编辑器里烘焙），游戏里重新生成要把
 * RecastNavMesh 改成 Dynamic，那是项目设置，GameTest 不改项目；PIE 和独立进程表现也不一致。
 *
 * **怎么探**：地面按 50 厘米一格（概念上的格子，不预先建）。A* 展开到一格才检查它——
 * 往下打射线找地面、看坡度，再用**角色自己的胶囊**在那个高度做一次重叠检查；两格之间能不能走
 * 再扫一次胶囊、看台阶高度。台阶高度、可行走坡度、胶囊尺寸、碰撞通道都从角色读，换角色自动适配。
 * 开阔地带走直线时只查一条窄带；每帧最多查 `Step` 给的格数，分摊到几帧，不卡游戏。
 *
 * **只认在地上走**：跳沟、爬梯子、游泳、要先开门的路算不出来——这些靠录制轨迹补。
 * 只在游戏线程上用。
 */
class GAMETEST_API FGameTestPathProbe
{
public:
	enum class EResult : uint8
	{
		Running,
		Found,
		Failed,
	};

	/** 角色的体型和走路能力（从角色和它的移动组件读）。 */
	struct FAgent
	{
		float Radius = 34.0f;
		float HalfHeight = 88.0f;
		float StepHeight = 45.0f;
		/** 可行走地面法线 Z 的下限（cos(可行走坡度)）。 */
		float WalkableFloorZ = 0.71f;
		ECollisionChannel Channel = ECC_Pawn;
	};

	/** 从角色读体型：有胶囊用胶囊，没有用碰撞包围盒估；有角色移动组件就读台阶高度和坡度。 */
	static FAgent MakeAgent(const APawn& InPawn);

	/**
	 * 开始一次搜索。`InFrom` / `InTo` 都是角色的位置（胶囊中心），不是脚底。
	 * `InGoalRadius`：离终点水平距离在这个范围内（竖直差 250 以内）就算到了。
	 */
	void Start(UWorld& InWorld, const APawn& InPawn, const FVector& InFrom, const FVector& InTo, double InGoalRadius);

	/** 往前算一点：最多检查 `InBudget` 次（每次一条射线或一次胶囊查询）。 */
	EResult Step(int32 InBudget);

	/** 找到时的路径（胶囊中心的位置，已拉直，最后一个点是终点）。 */
	const TArray<FVector>& GetPath() const
	{
		return Path;
	}

	/** 失败原因（中文，进步骤结果）。 */
	const FString& GetWhy() const
	{
		return Why;
	}

	/** 至今检查过的格子数（进结果，帮判断探了多大一片）。 */
	int32 GetEvaluated() const
	{
		return Evaluated;
	}

private:
	struct FCell
	{
		/** 地面高度（脚底）。 */
		double FloorZ = 0.0;
		bool bChecked = false;
		bool bStandable = false;
	};

	struct FOpen
	{
		FIntPoint Key;
		double F = 0.0;
		double G = 0.0;

		bool operator<(const FOpen& InOther) const
		{
			return F < InOther.F;
		}
	};

	/** 检查一格能不能站：射线找地面 + 胶囊重叠。`InReferenceFloorZ` 是从哪个高度往下找（上一格的地面）。 */
	bool CheckCell(const FIntPoint& InKey, double InReferenceFloorZ, FCell& OutCell);

	/** 两格之间能不能走：台阶高度、胶囊扫过去不撞。 */
	bool CanWalk(const FVector& InFromFloor, const FVector& InToFloor);

	/** 拉直用：远处两点之间胶囊沿连线扫得通（不限台阶高度，坡道可以）。 */
	bool CanWalkStraight(const FVector& InFromFloor, const FVector& InToFloor);

	FVector CellCenter(const FIntPoint& InKey, double InFloorZ) const;
	FIntPoint ToKey(const FVector& InPoint) const;
	bool IsGoal(const FVector& InFloor) const;
	void BuildPath(const FIntPoint& InLast);
	/** 拉直：两点之间胶囊扫得通就去掉中间的点。 */
	void Straighten(TArray<FVector>& InOutFloors);

	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<const APawn> Pawn;
	FAgent Agent;
	FVector From = FVector::ZeroVector;
	FVector To = FVector::ZeroVector;
	FBox2D Bounds;
	double GoalRadius = 100.0;

	TMap<FIntPoint, FCell> Cells;
	TMap<FIntPoint, FIntPoint> CameFrom;
	TMap<FIntPoint, double> BestG;
	TArray<FOpen> Open;
	FIntPoint StartKey;

	TArray<FVector> Path;
	FString Why;
	int32 Evaluated = 0;
	EResult Result = EResult::Failed;
};
