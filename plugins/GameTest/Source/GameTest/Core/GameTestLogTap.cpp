// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestLogTap.h"

#include "Misc/OutputDeviceRedirector.h"

void FGameTestLogTap::Attach()
{
	if (bAttached || GLog == nullptr)
	{
		return;
	}
	GLog->AddOutputDevice(this);
	bAttached = true;
}

void FGameTestLogTap::Detach()
{
	if (!bAttached)
	{
		return;
	}
	if (GLog != nullptr)
	{
		GLog->RemoveOutputDevice(this);
	}
	bAttached = false;
}

uint64 FGameTestLogTap::GetSeq() const
{
	FScopeLock Guard(&Lock);
	return Seq;
}

TArray<FString> FGameTestLogTap::Range(uint64 InFrom, uint64 InTo) const
{
	FScopeLock Guard(&Lock);
	TArray<FString> Out;
	// 环里最旧的一行的序号：比它更早的已经被淘汰
	const uint64 Oldest = Seq > static_cast<uint64>(Lines.Num()) ? Seq - Lines.Num() : 0;
	const uint64 From = FMath::Max(InFrom, Oldest);
	const uint64 To = FMath::Min(InTo, Seq);
	for (uint64 Index = From; Index < To; ++Index)
	{
		Out.Add(Lines[static_cast<int32>(Index % Capacity)]);
	}
	return Out;
}

void FGameTestLogTap::Serialize(const TCHAR* InData, ELogVerbosity::Type InVerbosity, const FName& InCategory)
{
	if (InData == nullptr)
	{
		return;
	}

	// 和日志文件里的写法接近（类别: 级别: 内容），人和 Agent 读起来不用换脑子
	const FString Line = InVerbosity == ELogVerbosity::Log || InVerbosity == ELogVerbosity::Display
		? FString::Printf(TEXT("%s: %s"), *InCategory.ToString(), InData)
		: FString::Printf(TEXT("%s: %s: %s"), *InCategory.ToString(), ToString(InVerbosity), InData);

	FScopeLock Guard(&Lock);
	if (Lines.Num() < Capacity)
	{
		Lines.Add(Line);
	}
	else
	{
		Lines[static_cast<int32>(Seq % Capacity)] = Line;
	}
	++Seq;
}
