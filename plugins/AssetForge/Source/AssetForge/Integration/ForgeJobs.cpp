// Copyright Epic Games, Inc. All Rights Reserved.

#include "Integration/ForgeJobs.h"

#include "AgentPanelBridge.h"
#include "Core/ForgePipeline.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "State/ForgeState.h"

namespace
{
	/** 内存里留几个已经结束的作业：面板只关心最近的，留多了只是占内存。 */
	constexpr int32 MaxKeptJobs = 10;

	/** 一帧里最多用多少毫秒：一项导入可能就超过它，所以至少做一项。 */
	constexpr double FrameBudgetMs = 30.0;

	const TCHAR* ModeName(const FForgeBatchRunner& InRunner)
	{
		if (InRunner.GetMode() == FForgeBatchRunner::EMode::Export)
		{
			return TEXT("export");
		}
		return InRunner.IsDryRun() ? TEXT("plan") : TEXT("import");
	}
}

FForgeJobs& FForgeJobs::Get()
{
	static FForgeJobs Instance;
	return Instance;
}

bool FForgeJobs::IsRunning() const
{
	return Jobs.Num() > 0 && !Jobs.Last().Runner.IsDone();
}

FString FForgeJobs::Start(FForgeBatchRunner InRunner, const FString& InTitle, FString& OutError)
{
	if (IsRunning())
	{
		OutError = FString::Printf(TEXT("已经有作业在跑（%s，%s）：等它做完，或先用 forge.stop 取消"), *Jobs.Last().Id, *Jobs.Last().Title);
		return FString();
	}

	FJob& Job = Jobs.AddDefaulted_GetRef();
	Job.Id = FString::Printf(TEXT("forge-%d"), ++Serial);
	Job.Title = InTitle;
	Job.StartedAt = FDateTime::UtcNow();
	Job.Runner = MoveTemp(InRunner);
	if (Jobs.Num() > MaxKeptJobs)
	{
		Jobs.RemoveAt(0, Jobs.Num() - MaxKeptJobs);
	}

	if (!TickHandle.IsValid())
	{
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FForgeJobs::Tick));
	}
	Publish(Jobs.Last(), TEXT("started"));
	return Jobs.Last().Id;
}

bool FForgeJobs::Tick(float InDeltaTime)
{
	if (!IsRunning())
	{
		TickHandle.Reset();
		return false;
	}

	FJob& Job = Jobs.Last();
	const double Start = FPlatformTime::Seconds();
	do
	{
		Job.Runner.Step();
	}
	while (!Job.Runner.IsDone() && (FPlatformTime::Seconds() - Start) * 1000.0 < FrameBudgetMs);

	if (!Job.Runner.IsDone())
	{
		Publish(Job, TEXT("progress"));
		return true;
	}

	Job.EndedAt = FDateTime::UtcNow();
	// 导入和导出都会改清单和资产：让下次 forge.entries 重新判定，而不是用缓存里的旧状态
	ForgeState::InvalidateCache();
	Publish(Job, TEXT("finished"));
	TickHandle.Reset();
	return false;
}

bool FForgeJobs::Stop(const FString& InId)
{
	for (FJob& Job : Jobs)
	{
		if (Job.Id == InId && !Job.Runner.IsDone())
		{
			Job.Runner.Cancel();
			Job.EndedAt = FDateTime::UtcNow();
			ForgeState::InvalidateCache();
			Publish(Job, TEXT("finished"));
			return true;
		}
	}
	return false;
}

void FForgeJobs::Shutdown()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	Jobs.Reset();
}

TSharedRef<FJsonObject> FForgeJobs::Summary(const FJob& InJob)
{
	const FForgeBatchRunner& Runner = InJob.Runner;
	const FForgeBatchResult& Result = Runner.GetResult();
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("id"), InJob.Id);
	Out->SetStringField(TEXT("title"), InJob.Title);
	Out->SetStringField(TEXT("mode"), ModeName(Runner));
	Out->SetStringField(TEXT("status"), !Runner.IsDone() ? TEXT("running") : Result.bCancelled ? TEXT("cancelled") : Result.HasFailures() ? TEXT("failed") : TEXT("done"));
	Out->SetNumberField(TEXT("total"), Runner.GetTotal());
	Out->SetNumberField(TEXT("finished"), Runner.GetFinished());
	Out->SetNumberField(TEXT("succeeded"), Result.Succeeded);
	Out->SetNumberField(TEXT("failed"), Result.Failed);
	Out->SetNumberField(TEXT("skipped"), Result.Skipped);
	if (Runner.GetMode() == FForgeBatchRunner::EMode::Import)
	{
		Out->SetNumberField(TEXT("round"), Runner.GetRound());
	}
	if (!Runner.IsDone())
	{
		Out->SetStringField(TEXT("current"), Runner.GetCurrent());
	}
	Out->SetStringField(TEXT("startedAt"), InJob.StartedAt.ToIso8601());
	if (Runner.IsDone())
	{
		Out->SetStringField(TEXT("endedAt"), InJob.EndedAt.ToIso8601());
	}
	return Out;
}

void FForgeJobs::Publish(const FJob& InJob, const TCHAR* InPhase) const
{
	const TSharedRef<FJsonObject> Event = Summary(InJob);
	Event->SetStringField(TEXT("phase"), InPhase);
	FAgentPanelBridge::Get().PublishEvent(TEXT("forge.job"), Event);
}

TSharedPtr<FJsonObject> FForgeJobs::Describe(const FString& InId, const int32 InMaxReports) const
{
	const FJob* Found = nullptr;
	for (const FJob& Job : Jobs)
	{
		if (InId.IsEmpty() || Job.Id == InId)
		{
			Found = &Job;
		}
	}
	if (Found == nullptr)
	{
		return nullptr;
	}

	const TSharedRef<FJsonObject> Out = Summary(*Found);
	if (!Found->Runner.IsDone())
	{
		return Out;
	}

	// 报告可能很多（整个项目的配置）：失败的排前面，只回前 InMaxReports 份，动作清单只留计数
	TArray<const FForgeReport*> Reports;
	for (const FForgeReport& Report : Found->Runner.GetResult().Reports)
	{
		Reports.Add(&Report);
	}
	Reports.StableSort([](const FForgeReport& A, const FForgeReport& B) { return !A.bSucceeded && B.bSucceeded; });

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FForgeReport* Report : Reports)
	{
		if (Items.Num() >= InMaxReports)
		{
			break;
		}
		const TSharedPtr<FJsonObject> Json = ForgePipeline::ReportToJson(*Report);
		Json->RemoveField(TEXT("actions"));
		Items.Add(MakeShared<FJsonValueObject>(Json));
	}
	Out->SetArrayField(TEXT("reports"), Items);
	if (Reports.Num() > Items.Num())
	{
		Out->SetNumberField(TEXT("reportsOmitted"), Reports.Num() - Items.Num());
	}
	return Out;
}
