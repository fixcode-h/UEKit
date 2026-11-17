// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonValue;

/**
 * 移动轨迹的纯数据算法：录制时简化轨迹、回放时找「从哪接上轨迹」、点和 JSON 互转。
 *
 * 放在 Core 而不是录制器或 move 动作里：录制（写 `path`）和回放（读 `path`）都要用，
 * 而且它不碰引擎世界，可以单测（`GameTest.Trajectory`）。
 */
class GAMETEST_API GameTestTrajectory
{
public:
	/**
	 * Ramer–Douglas–Peucker 简化：去掉偏离前后连线不超过 `InTolerance`（厘米）的点。
	 * `InKeep` 里的下标一定保留（起跳点、游泳 / 飞行的起止点）；首尾总是保留。
	 * 返回保留下来的点在原数组里的下标（升序）。
	 */
	static TArray<int32> Simplify(const TArray<FVector>& InPoints, double InTolerance, const TSet<int32>& InKeep);

	/**
	 * 简化到不超过 `InMaxPoints` 个点：先按 `InTolerance` 简化，超了就把容差放大 1.5 倍再来，直到够少。
	 */
	static TArray<int32> SimplifyToLimit(const TArray<FVector>& InPoints, double InTolerance, int32 InMaxPoints, const TSet<int32>& InKeep);

	/**
	 * 回放时角色从哪接上轨迹：找离 `InPosition` 最近的一段（只比水平距离），返回这一段终点的下标
	 * （角色往前接着走，不倒回去），`OutDistance` 是到这一段的水平距离，`OutProjected` 是投影点。
	 * 轨迹少于 2 个点时返回 INDEX_NONE。
	 */
	static int32 FindJoinIndex(const TArray<FVector>& InPath, const FVector& InPosition, double& OutDistance, FVector& OutProjected);

	/** 轨迹总长（厘米，三维）。 */
	static double Length(const TArray<FVector>& InPath);

	/** 点 → `[x, y, z]`（保留 1 位小数，文件里不出现一长串小数）。 */
	static TSharedPtr<FJsonValue> PointToJson(const FVector& InPoint);

	/** `[x, y, z]` → 点；不是三个数字时返回 false。 */
	static bool PointFromJson(const TSharedPtr<FJsonValue>& InValue, FVector& OutPoint);

	static TArray<TSharedPtr<FJsonValue>> PathToJson(const TArray<FVector>& InPath);

	/** 读 `[[x,y,z], ...]`；有一项不对就返回 false 并写清楚第几项。 */
	static bool PathFromJson(const TArray<TSharedPtr<FJsonValue>>& InValues, TArray<FVector>& OutPath, FString& OutError);

	/** 下标数组（`jumps`、`fly`）→ JSON / 从 JSON 读（越界的丢掉）。 */
	static TArray<TSharedPtr<FJsonValue>> IndicesToJson(const TArray<int32>& InIndices);
	static TSet<int32> IndicesFromJson(const TArray<TSharedPtr<FJsonValue>>* InValues, int32 InCount);
};
