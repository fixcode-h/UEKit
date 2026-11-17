// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class APawn;
class UWorld;

/**
 * 一段正在录的移动：从「录移动」到「结束移动」之间每帧取角色的位置。
 *
 * - 离上一个记下的点超过 50 厘米才记，同时记移动模式（走 / 下落 / 游泳·飞）；
 * - 从「站在地上」变成「下落且向上走」记一次起跳（回放时在这个点之后调 Jump）；
 * - 角色被换掉（上车、变身、重生）、地图换了、进了过场动画（`bCinematicMode`）就作废：
 *   这些位移不是角色自己走出来的，回放推摇杆复现不了。
 * 结束时由录制器简化成 `move` 步骤的 `path`（见 `GameTestRecorderMove.cpp`）。只在游戏线程上用。
 */
struct FGameTestMoveSegment
{
	enum class EMode : uint8
	{
		Walk,
		Fall,
		/** 游泳或飞行：回放时按三维方向推。 */
		Fly,
	};

	TWeakObjectPtr<APawn> Pawn;
	TWeakObjectPtr<UWorld> World;
	double StartedAt = 0.0;
	FVector Start = FVector::ZeroVector;

	TArray<FVector> Points;
	TArray<EMode> Modes;
	/** 起跳点（Points 的下标）：离开这个点时要跳。 */
	TSet<int32> Jumps;
	double Meters = 0.0;

	/** 上一次推送录制状态的时刻：移动中每秒推一次，面板上的「已走多少米」跟着动。 */
	double LastBroadcastAt = 0.0;

	/** 开始录移动时按着的键（已经在步骤里补了松开），结束时还按着就补按下。 */
	TArray<FName> HeldAtStart;

	/** 开始一段：记下角色、世界和起点。角色不在时返回 false。 */
	bool Begin(APawn& InPawn, double InNow);

	/** 每帧取一次。返回 false 表示这段要作废，`OutWhy` 写原因（给工具栏和面板显示）。 */
	bool Sample(FString& OutWhy);

	/** 当前角色的位置模式（没有角色移动组件的当成走）。 */
	static EMode ModeOf(const APawn& InPawn);
};
