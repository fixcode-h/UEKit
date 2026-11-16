// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/GameTestRunConfig.h"

class FJsonObject;
class UUserWidget;
class UWorld;
struct FGameTestStep;

/**
 * 跑测试时自动处理「阻挡界面」：项目运行配置里标记过的 WBP 一出现在屏上，就按规则关掉、调它的函数或执行一条指令，
 * 让后面的步骤不被公告、引导、活动弹窗这类不定时弹出的界面挡住。
 *
 * 每次处理都交给调用方记进报告（进当前步骤的 `payload.blockers`）和 LogGameTest：自动做掉的事必须看得见，
 * 否则测试「通过」了也不知道中间被弹窗打断过几次。
 *
 * 同一个界面实例一次 run 只处理一次：处理后它没消失（规则写错、函数不关自己），再处理也一样，
 * 记一条「处理后还在」交给人看，不在每帧上反复调。只在游戏线程上用，由子系统持有。
 */
class GAMETEST_API FGameTestBlockerGuard
{
public:
	/** 新的一次 run 开始：换上这次的规则（只留启用的），清掉上次处理过的记录。 */
	void Begin(const TArray<FGameTestBlockerRule>& InRules);

	/** run 结束：不再处理。 */
	void End();

	bool IsActive() const
	{
		return Rules.Num() > 0;
	}

	/**
	 * 每帧调（节流到每 0.2 秒扫一次）。`InStepInFlight` 是正在等的那一步：它要操作的正是某个阻挡界面时不处理那个界面
	 * （场景里明写了「点公告的确定」，就不该被规则抢先关掉）。处理掉的界面写进 OutEvents。
	 */
	void Tick(double InNowSeconds, UWorld* InWorld, const FGameTestStep* InStepInFlight, TArray<TSharedPtr<FJsonObject>>& OutEvents);

	/** 这个界面是不是某条规则标记的（规则下标，不是返回 INDEX_NONE）。单测和面板「试一下」用。 */
	int32 FindRule(const UUserWidget& InWidget) const;

private:
	TSharedPtr<FJsonObject> Handle(UUserWidget& InWidget, const FGameTestBlockerRule& InRule, UWorld* InWorld) const;

	TArray<FGameTestBlockerRule> Rules;
	/** 已处理过的实例，和处理的时刻（过一会儿复查它还在不在）。 */
	TMap<TWeakObjectPtr<UUserWidget>, double> Handled;
	/** 第一次在屏上看到的时刻（规则的 delayMs 从这里算）。 */
	TMap<TWeakObjectPtr<UUserWidget>, double> FirstSeen;
	TSet<TWeakObjectPtr<UUserWidget>> ReportedStuck;
	double NextScanAt = 0.0;
};
