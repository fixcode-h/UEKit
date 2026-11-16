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

TArray<FString> FGameTestLogTap::ImportantSince(double InFromTime) const
{
	FScopeLock Guard(&Lock);
	TArray<FString> Out;
	const uint64 Oldest = ImportantSeq > static_cast<uint64>(Important.Num()) ? ImportantSeq - Important.Num() : 0;
	for (uint64 Index = Oldest; Index < ImportantSeq; ++Index)
	{
		const FImportantLine& Entry = Important[static_cast<int32>(Index % ImportantCapacity)];
		if (Entry.Time >= InFromTime)
		{
			Out.Add(Entry.Line);
		}
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

	static const FName GameTestCategory(TEXT("LogGameTest"));
	// 级别里可能带 SetColor / BreakOnLog 这类标志位，先取出真正的级别再比。
	const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(InVerbosity & ELogVerbosity::VerbosityMask);
	if (InCategory == GameTestCategory || (Level != ELogVerbosity::NoLogging && Level <= ELogVerbosity::Warning))
	{
		FImportantLine Entry;
		Entry.Time = FPlatformTime::Seconds();
		Entry.Line = Line;
		if (Important.Num() < ImportantCapacity)
		{
			Important.Add(MoveTemp(Entry));
		}
		else
		{
			Important[static_cast<int32>(ImportantSeq % ImportantCapacity)] = MoveTemp(Entry);
		}
		++ImportantSeq;
	}
}
