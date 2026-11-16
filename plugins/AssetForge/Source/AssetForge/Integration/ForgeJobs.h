// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Service/ForgeBatch.h"

class FJsonObject;

/**
 * 面板发起的批量导入 / 导出作业：`forge.import`、`forge.export` 马上返回作业号，这里每帧走一步，
 * 编辑器不会卡住，面板也能取消。进度经 UELink 推送事件 `forge.job` 实时推给面板，
 * 最终结果留在内存里给 `forge.job` 查（面板可能中途才连上）。
 *
 * 同一时间只跑一个作业：导入会保存资产，两个作业交叉写同一批资产没有意义，还会互相打乱多轮推进。
 * 全部在游戏线程上。
 */
class FForgeJobs
{
public:
	static FForgeJobs& Get();

	/** 起一个作业；已经有作业在跑时返回空并写 OutError。 */
	FString Start(FForgeBatchRunner InRunner, const FString& InTitle, FString& OutError);

	/** 作业的状态和（做完后的）结果；`InId` 为空时取最近一个。找不到返回空。 */
	TSharedPtr<FJsonObject> Describe(const FString& InId, int32 InMaxReports) const;

	/** 取消；作业不存在或已经结束返回 false。 */
	bool Stop(const FString& InId);

	bool IsRunning() const;

	/** 模块关闭时：停掉 ticker，丢掉作业。 */
	void Shutdown();

private:
	struct FJob
	{
		FString Id;
		FString Title;
		FDateTime StartedAt;
		FDateTime EndedAt;
		FForgeBatchRunner Runner;
	};

	bool Tick(float InDeltaTime);
	void Publish(const FJob& InJob, const TCHAR* InPhase) const;
	static TSharedRef<FJsonObject> Summary(const FJob& InJob);

	TArray<FJob> Jobs;
	FTSTicker::FDelegateHandle TickHandle;

	/** 正在 Tick 里做一步：导入时引擎可能又驱动 ticker，这时不重入。 */
	bool bInTick = false;
	int32 Serial = 0;
};
