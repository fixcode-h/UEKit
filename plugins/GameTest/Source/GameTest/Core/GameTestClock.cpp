#include "Core/GameTestClock.h"

#include "HAL/PlatformTime.h"
#include <atomic>

namespace
{
	/** 只在游戏线程上读写 */
	double LastReal = 0.0;
	/** 日志回调在别的线程上读 */
	std::atomic<double> Offset{0.0};
}

double GameTestClock::Now()
{
	const double Real = FPlatformTime::Seconds();
	if (IsInGameThread())
	{
		const double Gap = LastReal > 0.0 ? Real - LastReal : 0.0;
		if (Gap > StallSeconds)
		{
			// 留一帧的量：卡住前后那一帧本来就要算进去
			Offset.store(Offset.load() + Gap - 0.1);
		}
		LastReal = Real;
	}
	return Real - Offset.load();
}

double GameTestClock::PausedSeconds()
{
	return Offset.load();
}
