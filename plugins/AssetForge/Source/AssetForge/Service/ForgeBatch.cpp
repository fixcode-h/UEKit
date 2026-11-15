// Copyright Epic Games, Inc. All Rights Reserved.

#include "Service/ForgeBatch.h"

#include "Misc/Paths.h"

FForgeBatchRunner FForgeBatchRunner::MakeImport(const TArray<FString>& InConfigPaths, const bool bInDryRun)
{
	FForgeBatchRunner Runner;
	Runner.Mode = EMode::Import;
	Runner.bDryRun = bInDryRun;
	Runner.Pending = InConfigPaths;
	Runner.Total = InConfigPaths.Num();
	Runner.bDone = InConfigPaths.Num() == 0;
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
