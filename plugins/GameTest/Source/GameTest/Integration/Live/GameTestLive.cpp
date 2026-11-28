// Copyright Epic Games, Inc. All Rights Reserved.

#include "Integration/Live/GameTestLive.h"
#include "Core/GameTestClock.h"

#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "CoreGlobals.h"

namespace GameTestLiveDetail
{
	FTSTicker::FDelegateHandle& TickerHandle()
	{
		static FTSTicker::FDelegateHandle Handle;
		return Handle;
	}

	/** 抓取的画面和监视都在这里推进；两边都没有活儿时摘掉定时器，平时零开销。 */
	bool Tick(float InDeltaTime)
	{
		// 退出、回收垃圾时不去碰游戏对象（CallOp 里也会拦，这里先省掉截图）
		if (IsEngineExitRequested())
		{
			return true;
		}
		const double Now = GameTestClock::Now();
		const bool bPick = GameTestLive::PickTick(Now);
		const bool bWatch = GameTestLive::WatchTick(Now);
		if (!bPick && !bWatch)
		{
			TickerHandle().Reset();
			return false;
		}
		return true;
	}
}

void GameTestLive::EnsureTicker()
{
	FTSTicker::FDelegateHandle& Handle = GameTestLiveDetail::TickerHandle();
	if (!Handle.IsValid())
	{
		// 50ms 一次：监视默认 200ms、画面默认 2 帧 / 秒，各自按自己的间隔跳过
		Handle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&GameTestLiveDetail::Tick), 0.05f);
	}
}

void GameTestLive::Shutdown()
{
	FTSTicker::FDelegateHandle& Handle = GameTestLiveDetail::TickerHandle();
	if (Handle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Handle);
		Handle.Reset();
	}
	PickShutdown();
	WatchShutdown();
}
