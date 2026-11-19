// Copyright Epic Games, Inc. All Rights Reserved.

#include "Service/ForgeBatch.h"

#include "AssetForge.h"
#include "Core/ForgePipeline.h"

#include "Misc/Paths.h"

namespace
{
	/**
	 * 把每份配置引用的子界面配置（`ref`，可以多层）排到它前面：导入父界面时子界面跟着生成。
	 * 用户 2026-11-19：「子 json 在导入父 json 生成 UMG 的时候也会生成子 UMG」。
	 * 子界面已经在列表里的不重复加；引用成环的由导入时的 ref 解析报错，这里只防死循环。
	 */
	void AddWithRefs(const FString& InConfigPath, TArray<FString>& OutOrdered, TSet<FString>& InOutSeen, TSet<FString>& InOutVisiting)
	{
		const FString Key = FPaths::ConvertRelativePathToFull(ForgePipeline::ResolveConfigPath(InConfigPath));
		if (InOutSeen.Contains(Key) || InOutVisiting.Contains(Key))
		{
			return;
		}
		InOutVisiting.Add(Key);
		TArray<FString> Refs;
		if (ForgePipeline::CollectRefConfigs(Key, Refs))
		{
			for (const FString& Ref : Refs)
			{
				if (FPaths::FileExists(Ref))
				{
					AddWithRefs(Ref, OutOrdered, InOutSeen, InOutVisiting);
				}
			}
		}
		InOutVisiting.Remove(Key);
		InOutSeen.Add(Key);
		OutOrdered.Add(Key);
	}
}

FForgeBatchRunner FForgeBatchRunner::MakeImport(const TArray<FString>& InConfigPaths, const bool bInDryRun)
{
	FForgeBatchRunner Runner;
	Runner.Mode = EMode::Import;
	Runner.bDryRun = bInDryRun;

	TArray<FString> Ordered;
	TSet<FString> Seen;
	TSet<FString> Visiting;
	for (const FString& ConfigPath : InConfigPaths)
	{
		AddWithRefs(ConfigPath, Ordered, Seen, Visiting);
	}
	if (Ordered.Num() > InConfigPaths.Num())
	{
		UE_LOG(LogAssetForge, Log, TEXT("[导入] 连带导入 %d 份被引用的子界面配置"), Ordered.Num() - InConfigPaths.Num());
	}

	Runner.Pending = MoveTemp(Ordered);
	Runner.Total = Runner.Pending.Num();
	Runner.bDone = Runner.Pending.Num() == 0;
	return Runner;
}

FForgeBatchRunner FForgeBatchRunner::MakeExport(const TArray<FString>& InAssetPaths)
{
	FForgeBatchRunner Runner;
	Runner.Mode = EMode::Export;
	Runner.Pending = InAssetPaths;
	Runner.Total = InAssetPaths.Num();
	Runner.bDone = InAssetPaths.Num() == 0;
	return Runner;
}

FString FForgeBatchRunner::GetCurrent() const
{
	if (bDone || !Pending.IsValidIndex(Index))
	{
		return FString();
	}
	return Mode == EMode::Import ? FPaths::GetCleanFilename(Pending[Index]) : FPaths::GetBaseFilename(Pending[Index]);
}

void FForgeBatchRunner::Step()
{
	if (bDone)
	{
		return;
	}
	if (Mode == EMode::Import)
	{
		StepImport();
	}
	else
	{
		StepExport();
	}
}

void FForgeBatchRunner::Cancel()
{
	if (bDone)
	{
		return;
	}
	Result.bCancelled = true;
	bDone = true;
}

void FForgeBatchRunner::StepImport()
{
	const FString ConfigPath = Pending[Index++];
	FForgeReport Report = ForgeService::ImportConfig(ConfigPath, bDryRun);
	if (Report.bSucceeded)
	{
		bMadeProgress = true;
		++Result.Succeeded;
		LastFailure.Remove(ConfigPath);
		Result.Reports.Add(MoveTemp(Report));
	}
	else
	{
		StillPending.Add(ConfigPath);
		LastFailure.Add(ConfigPath, MoveTemp(Report));
	}

	if (Index < Pending.Num())
	{
		return;
	}

	// 一轮结束：全过了、或这一轮毫无进展（再来一轮结果也一样）就收尾，否则拿没过的再来一轮
	if (StillPending.Num() == 0 || !bMadeProgress)
	{
		Pending = MoveTemp(StillPending);
		FinishImport();
		return;
	}
	Pending = MoveTemp(StillPending);
	StillPending.Reset();
	Index = 0;
	bMadeProgress = false;
	++Round;
}

void FForgeBatchRunner::FinishImport()
{
	for (const FString& ConfigPath : Pending)
	{
		if (FForgeReport* Report = LastFailure.Find(ConfigPath))
		{
			Result.Reports.Add(MoveTemp(*Report));
		}
		++Result.Failed;
	}
	Pending.Reset();
	LastFailure.Reset();
	bDone = true;
}

void FForgeBatchRunner::StepExport()
{
	const FString AssetPath = Pending[Index++];
	// 先用资产注册表判类型：不支持的跳过、不加载（文件夹批量纳管时这一步省掉成百上千次加载）
	if (!ForgeService::IsExportable(AssetPath))
	{
		++Result.Skipped;
	}
	else
	{
		FForgeReport Report = ForgeService::ExportAsset(AssetPath);
		if (Report.bSucceeded)
		{
			++Result.Succeeded;
		}
		else
		{
			++Result.Failed;
		}
		Result.Reports.Add(MoveTemp(Report));
	}
	bDone = Index >= Pending.Num();
}
