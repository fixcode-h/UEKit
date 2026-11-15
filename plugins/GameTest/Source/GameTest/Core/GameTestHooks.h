// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"

class FJsonObject;

/**
 * 主模块往外抛的几个时机：可选模块和对外接口（UELink 操作）挂在这里，主模块不反过来认识它们。
 *
 * 为什么不让主模块直接调：可选模块（CommonUI / GAS / EnhancedInput）可能没编进来，
 * UELink 侧的推送也不该是执行器关心的事。全部只在游戏线程上广播。
 */
class GAMETEST_API GameTestHooks
{
public:
	/**
	 * 一次 run 收尾（成功 / 失败 / 取消都广播）。可选模块在这里清掉自己留下的状态，
	 * 例如 EnhancedInput 模块停掉本次 run 启动过的输入注入。
	 */
	static FSimpleMulticastDelegate& OnRunFinished();

	/** `gt.health` / `ping` 组装时广播：可选模块往里加自己的字段（如 `activeInjections`）。 */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnHealth, FJsonObject& /*Health*/);
	static FOnHealth& OnHealth();

	/** 作业快照变了（开始、每完成一步、结束）：对外接口据此推 `gt.job` 事件。 */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnJobChanged, const TSharedRef<FJsonObject>& /*Job*/);
	static FOnJobChanged& OnJobChanged();

	/** 录制状态变了（开始、录到一步、停止）：对外接口据此推 `gt.record` 事件，面板实时显示录到的步骤。 */
	static FOnJobChanged& OnRecordChanged();
};
