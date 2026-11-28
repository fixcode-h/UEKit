// GameTest 的计时（2.9.0）：实际时间减去「被打断」的时间。
#pragma once

#include "CoreMinimal.h"

/**
 * 游戏线程被断点、同步加载、长帧卡住时，GameTest 自己不在跑，这段时间不该算进步骤超时、总时限、稳住时长
 * （以前断点停一分钟再继续，当前步骤直接判超时失败）。
 * 游戏线程上每次取时间时看和上一次隔了多久：超过 StallSeconds 的部分记作被打断，从时间里扣掉。
 * GameTest 里所有计时都走 Now()，彼此一致；GameTestSubsystem 每帧都调一次，所以平常两次之间只隔一帧。
 * 非游戏线程（日志回调）只读不更新，卡住那一帧里的日志时间会略偏大，不影响判断。
 */
namespace GameTestClock
{
	/** 两次取时间隔这么久以上才算被打断（秒）；正常长帧（加载一个界面）不算 */
	constexpr double StallSeconds = 2.0;

	GAMETEST_API double Now();
	/** 一共被打断了多少秒（报告里每步的 pausedMs 用开始、结束时的差） */
	GAMETEST_API double PausedSeconds();
}
