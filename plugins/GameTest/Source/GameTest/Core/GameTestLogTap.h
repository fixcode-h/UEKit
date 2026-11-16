// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

/**
 * 最近的日志行（环形，最多 1000 行），给失败证据取「失败前后 50 行」用。
 *
 * 为什么不读日志文件：日志文件有缓冲、可能被别的进程占着，而且游戏进程不一定写文件；挂一个输出设备拿到的
 * 就是这一刻的行，带一个递增序号，「失败之后又来了多少行」也能数出来。
 * 日志可能从任何线程来，读写都加锁；只存字符串，不碰引擎对象。
 */
class GAMETEST_API FGameTestLogTap : public FOutputDevice
{
public:
	static constexpr int32 Capacity = 1000;

	/** 挂到 GLog 上（重复调用无害）。 */
	void Attach();

	/** 从 GLog 上摘下来。 */
	void Detach();

	/** 到目前为止一共收到了多少行（单调递增，不因环形淘汰而减少）。 */
	uint64 GetSeq() const;

	/** 序号 [InFrom, InTo) 里还留在环里的行（被淘汰的跳过）。 */
	TArray<FString> Range(uint64 InFrom, uint64 InTo) const;

	//~ Begin FOutputDevice interface
	virtual void Serialize(const TCHAR* InData, ELogVerbosity::Type InVerbosity, const FName& InCategory) override;
	virtual bool CanBeUsedOnAnyThread() const override
	{
		return true;
	}
	virtual bool CanBeUsedOnMultipleThreads() const override
	{
		return true;
	}
	//~ End FOutputDevice interface

private:
	mutable FCriticalSection Lock;
	TArray<FString> Lines;
	uint64 Seq = 0;
	bool bAttached = false;
};
