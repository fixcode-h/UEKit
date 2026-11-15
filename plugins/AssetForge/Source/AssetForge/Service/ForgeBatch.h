// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Service/ForgeService.h"

/**
 * 批量导入 / 导出的分步执行器：一次 `Step()` 只处理一项。
 *
 * 同步的批量（右键菜单、命令行，`ForgeService::ImportConfigs`）和异步作业（面板的 `forge.import`，
 * 每帧走一步、不卡编辑器）共用这一份逻辑，「多轮推进」「不支持的资产算跳过」只写一遍。
 *
 * 多轮推进：引用另一份配置的 UMG 要等被引用者成为资产后才能解析，所以一轮里能过的先过，
 * 没过的留到下一轮；某一轮毫无进展就停，剩下的记失败（报告取最后一次失败的那份）。
 *
 * 全部在游戏线程调用。
 */
class ASSETFORGE_API FForgeBatchRunner
{
public:
	enum class EMode : uint8
	{
		Import,
		Export,
	};

	static FForgeBatchRunner MakeImport(const TArray<FString>& InConfigPaths, bool bInDryRun);
	static FForgeBatchRunner MakeExport(const TArray<FString>& InAssetPaths);

	bool IsDone() const { return bDone; }

	/** 处理下一项；已经做完时什么都不做。 */
	void Step();

	/** 取消：已经做完的保留，剩下的不再处理。 */
	void Cancel();

	EMode GetMode() const { return Mode; }
	bool IsDryRun() const { return bDryRun; }
	int32 GetTotal() const { return Total; }
	/** 已经有结论的项数（成功、跳过；失败要等多轮推进结束才算）。 */
	int32 GetFinished() const { return Result.Succeeded + Result.Failed + Result.Skipped; }
	/** 正在处理（下一步要处理）的那一项，给进度显示用。 */
	FString GetCurrent() const;
	/** 第几轮（导入才有多轮，从 1 起）。 */
	int32 GetRound() const { return Round; }

	const FForgeBatchResult& GetResult() const { return Result; }

private:
	void StepImport();
	void StepExport();
	void FinishImport();

	EMode Mode = EMode::Import;
	bool bDryRun = false;
	bool bDone = false;
	int32 Total = 0;
	int32 Round = 1;

	/** 这一轮要处理的项和下标。 */
	TArray<FString> Pending;
	int32 Index = 0;

	/** 导入：这一轮没过、留到下一轮的；以及每项最后一次失败的报告。 */
	TArray<FString> StillPending;
	TMap<FString, FForgeReport> LastFailure;
	bool bMadeProgress = false;

	FForgeBatchResult Result;
};
