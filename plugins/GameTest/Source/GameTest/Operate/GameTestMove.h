// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class APawn;
class FJsonObject;
class UWorld;

/**
 * `move` 步骤（让本地玩家的角色走到一个点）的公共部分：设置校验、找角色。
 * 动作实现在 `GameTestMove.cpp` 的 `GameTestHandlers::HandleMove`。
 *
 * **怎么走**：不管路线是导航网格算的、按需探路算的还是录制的轨迹，都交给同一个驱动——
 * 每帧对角色调 `AddMovementInput`（「推摇杆」），和玩家输入走同一个移动组件入口：
 * 碰撞、台阶、走速、动画、联机的客户端预测都和真人一样，也不用知道项目的按键绑定。
 * 项目的移动不读 `AddMovementInput` 时可以换成 `drive: "keys"`：按住前进键、把镜头转向下一个路点。
 *
 * **`via: "auto"` 的顺序**：导航网格（覆盖起终点、路径完整）→ 按录制轨迹（离轨迹远时先探路接上）
 * → 整段探路 → 瞬移（只有 `fallback: "teleport"` 时）。不核对角色是不是站在录制时的起点。
 */
class GAMETEST_API GameTestMove
{
public:
	/**
	 * 校验运行设置 `_settings.json` 和场景顶层的 `move` 段：
	 * `fallback`（none / teleport）、`teleportCommand`、`radius`、`drive`（input / keys）、`forwardKey`、`hotkeys`。
	 * 不认识的字段报错，写错了不静默忽略。
	 */
	static bool ValidateDefaults(const FJsonObject& InJson, FString& OutError);

	/** 这个世界里本地玩家（第一个玩家控制器）的角色；没有时返回 nullptr。 */
	static APawn* FindLocalPawn(UWorld& InWorld);

	/** 两份 move 段合在一起：`InOverride` 里有的字段盖掉 `InBase` 的（场景盖运行设置）。都为空时返回空。 */
	static TSharedPtr<FJsonObject> MergeDefaults(const TSharedPtr<FJsonObject>& InBase, const TSharedPtr<FJsonObject>& InOverride);
};
