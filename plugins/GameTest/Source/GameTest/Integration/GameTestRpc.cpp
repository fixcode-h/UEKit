// Copyright Epic Games, Inc. All Rights Reserved.

#include "Integration/GameTestRpc.h"

#include "AgentPanelBridge.h"
#include "Core/GameTestHooks.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestScenario.h"
#include "Record/GameTestRecorder.h"
#include "Shot/GameTestShot.h"
#include "GameTestSettings.h"
#include "GameTestSubsystem.h"
#include "GameTestVersion.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace GameTestRpc
{
namespace
{
	// 返回长度的上限（和面板 ADR 0067 一致）：默认 4000 字，读结构的（快照、报告）最多 12000 字，
	// 超了把完整内容写进 Saved/GameTest/responses/，只回路径。
	constexpr int32 DefaultMaxChars = 4000;
	constexpr int32 StructureMaxChars = 12000;
	constexpr int32 DefaultPageSize = 200;
	constexpr int32 DefaultJobSteps = 50;

	TArray<FString> RegisteredOps;
	FDelegateHandle JobHandle;
	FDelegateHandle RecordHandle;

	FString Str(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name)
	{
		FString Value;
		if (Args.IsValid())
		{
			Args->TryGetStringField(Name, Value);
		}
		return Value.TrimStartAndEnd();
	}

	bool Bool(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name, const bool bDefault)
	{
		bool bValue = bDefault;
		if (Args.IsValid())
		{
			Args->TryGetBoolField(Name, bValue);
		}
		return bValue;
	}

	int32 Int(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name, const int32 Default)
	{
		int32 Value = Default;
		if (Args.IsValid())
		{
			Args->TryGetNumberField(Name, Value);
		}
		return Value;
	}

	UGameTestSubsystem* RequireSubsystem(FString& OutError)
	{
		UGameTestSubsystem* Subsystem = UGameTestSubsystem::Get();
		if (Subsystem == nullptr)
		{
			OutError = TEXT("GameTest 子系统还没就绪（引擎在启动或退出中），稍后再试");
		}
		return Subsystem;
	}

	FString ScenarioRootDir()
	{
		return UGameTestSettings::ResolveScenarioRootDir();
	}

	/**
	 * 返回体超长时整份写进文件，只回路径和几项摘要。
	 * 摘要挑的是调用方最先要看的字段（成没成、状态、说了什么），不用打开文件就能判断下一步。
	 */
	TSharedPtr<FJsonObject> Capped(const TSharedPtr<FJsonObject>& InJson, const TCHAR* InLabel, const int32 InMaxChars)
	{
		const FString Text = GameTestJson::ToCompactString(InJson);
		if (Text.Len() <= InMaxChars)
		{
			return InJson;
		}

		const FString Path = FPaths::Combine(GameTestShot::GetOutputRootDir(), TEXT("responses"),
			FString::Printf(TEXT("%s-%s.json"), InLabel, *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S-%s"))));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
		FFileHelper::SaveStringToFile(GameTestJson::ToPrettyString(InJson), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

		const TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const TCHAR* Field : { TEXT("ok"), TEXT("finished"), TEXT("status"), TEXT("message"), TEXT("action"), TEXT("runId"),
				 TEXT("name"), TEXT("succeeded"), TEXT("total"), TEXT("failed"), TEXT("planned"), TEXT("busy"), TEXT("outputDir") })
		{
			if (const TSharedPtr<FJsonValue> Value = InJson->TryGetField(Field))
			{
				Out->SetField(Field, Value);
			}
		}
		Out->SetBoolField(TEXT("truncated"), true);
		Out->SetStringField(TEXT("file"), GameTestShot::ToProjectRelative(Path));
		Out->SetNumberField(TEXT("chars"), Text.Len());
		Out->SetStringField(TEXT("hint"), FString::Printf(
			TEXT("完整结果有 %d 字，超过 %d 字的上限，已写进 file（相对项目根），需要细节时读这个文件"), Text.Len(), InMaxChars));
		return Out;
	}

	/** 报告里的 steps 只留最后 N 步（作业刚开始时前面的步骤面板已经通过事件拿到了）。 */
	TSharedPtr<FJsonObject> TrimSteps(const TSharedPtr<FJsonObject>& InJson, const int32 InKeep)
	{
		const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
		if (!InJson.IsValid() || !InJson->TryGetArrayField(TEXT("steps"), Steps) || Steps == nullptr || Steps->Num() <= InKeep)
		{
			return InJson;
		}

		const TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>(*InJson);
		TArray<TSharedPtr<FJsonValue>> Kept;
		for (int32 Index = Steps->Num() - InKeep; Index < Steps->Num(); ++Index)
		{
			Kept.Add((*Steps)[Index]);
		}
		Out->SetArrayField(TEXT("steps"), Kept);
		Out->SetNumberField(TEXT("stepsOmitted"), Steps->Num() - InKeep);
		return Out;
	}

	bool Health(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		UGameTestSubsystem* Subsystem = RequireSubsystem(Error);
		if (Subsystem == nullptr)
		{
			return false;
		}
		Out = Subsystem->BuildHealthJson();
		Out->SetStringField(TEXT("version"), GAMETEST_VERSION);
		Out->SetStringField(TEXT("scenarioRoot"), ScenarioRootDir());
		Out->SetStringField(TEXT("outputRoot"), GameTestShot::GetOutputRootDir());
		Out->SetObjectField(TEXT("record"), FGameTestRecorder::Get().DescribeStatus(/*InLastSteps=*/0));
		return true;
	}

	bool Actions(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		UGameTestSubsystem* Subsystem = RequireSubsystem(Error);
		if (Subsystem == nullptr)
		{
			return false;
		}
		Out = Capped(Subsystem->BuildActionsJson(), TEXT("actions"), StructureMaxChars);
		return true;
	}

	bool Scenarios(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		UGameTestSubsystem* Subsystem = RequireSubsystem(Error);
		if (Subsystem == nullptr)
		{
			return false;
		}

		const TSharedPtr<FJsonObject> All = Subsystem->BuildScenariosJson();
		const FString Query = Str(Args, TEXT("query"));
		const int32 Offset = FMath::Max(0, Int(Args, TEXT("offset"), 0));
		const int32 Limit = FMath::Clamp(Int(Args, TEXT("limit"), DefaultPageSize), 1, 1000);

		TArray<TSharedPtr<FJsonValue>> Matched;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (All->TryGetArrayField(TEXT("scenarios"), Items) && Items != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (!Item->TryGetObject(Object) || Object == nullptr)
				{
					continue;
				}
				if (Query.IsEmpty() || (*Object)->GetStringField(TEXT("name")).Contains(Query)
					|| (*Object)->GetStringField(TEXT("relativePath")).Contains(Query))
				{
					Matched.Add(Item);
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> Page;
		for (int32 Index = Offset; Index < FMath::Min(Matched.Num(), Offset + Limit); ++Index)
		{
			Page.Add(Matched[Index]);
		}

		Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("rootDir"), All->GetStringField(TEXT("rootDir")));
		Out->SetStringField(TEXT("absoluteRootDir"), All->GetStringField(TEXT("absoluteRootDir")));
		Out->SetArrayField(TEXT("scenarios"), Page);
		Out->SetNumberField(TEXT("total"), Matched.Num());
		Out->SetNumberField(TEXT("offset"), Offset);
		Out = Capped(Out, TEXT("scenarios"), StructureMaxChars);
		return true;
	}

	/** `scenario`（相对场景根的路径）→ 步骤、名字、版本、场景自己的步间隔。 */
	bool LoadScenario(
		const FString& InRelativePath,
		TArray<TSharedPtr<FJsonObject>>& OutSteps,
		FString& OutName,
		int32& OutRevision,
		int32& OutStepDelayMs,
		FString& OutError)
	{
		FString FullPath = InRelativePath;
		if (FPaths::IsRelative(FullPath))
		{
			FullPath = FPaths::Combine(ScenarioRootDir(), InRelativePath);
		}

		const TSharedPtr<FJsonObject> Scenario = GameTestJson::ParseObjectFromFile(FullPath, OutError);
		if (!Scenario.IsValid())
		{
			return false;
		}

		TArray<FString> Errors;
		if (!GameTestScenario::CollectSteps(Scenario, OutSteps, Errors))
		{
			OutError = FString::Printf(TEXT("%s：%s"), *InRelativePath, *FString::Join(Errors, TEXT("；")));
			return false;
		}

		OutName = GameTestScenario::GetName(Scenario, FPaths::GetBaseFilename(InRelativePath));
		OutRevision = GameTestScenario::GetRevision(Scenario);
		OutStepDelayMs = -1;
		GameTestJson::TryGetInt(*Scenario, TEXT("stepDelayMs"), OutStepDelayMs);
		return true;
	}

	bool Run(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		UGameTestSubsystem* Subsystem = RequireSubsystem(Error);
		if (Subsystem == nullptr)
		{
			return false;
		}

		TArray<TSharedPtr<FJsonObject>> Steps;
		FString RunName;
		int32 Revision = 0;
		int32 StepDelayMs = -1;

		const FString ScenarioPath = Str(Args, TEXT("scenario"));
		if (!ScenarioPath.IsEmpty())
		{
			if (!LoadScenario(ScenarioPath, Steps, RunName, Revision, StepDelayMs, Error))
			{
				return false;
			}
		}
		else
		{
			const TArray<TSharedPtr<FJsonValue>>* StepValues = nullptr;
			if (!Args.IsValid() || !Args->TryGetArrayField(TEXT("steps"), StepValues) || StepValues == nullptr || StepValues->Num() == 0)
			{
				Error = TEXT("gt.run 要么给 scenario（场景文件相对场景根的路径），要么给非空的 steps 数组");
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *StepValues)
			{
				const TSharedPtr<FJsonObject>* Step = nullptr;
				if (Value.IsValid() && Value->TryGetObject(Step) && Step != nullptr)
				{
					Steps.Add(*Step);
				}
			}
			RunName = Str(Args, TEXT("name"));
			StepDelayMs = Int(Args, TEXT("stepDelayMs"), -1);
		}

		FString RunId;
		TSharedPtr<FJsonObject> Result;
		if (!Subsystem->BeginScenarioRun(Steps, RunName, Revision, RunId, Result, StepDelayMs))
		{
			FString Reason;
			Error = Result.IsValid() && Result->TryGetStringField(TEXT("error"), Reason) ? Reason : TEXT("run 没能开始");
			return false;
		}

		Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("runId"), RunId);
		Out->SetNumberField(TEXT("steps"), Steps.Num());
		Out->SetNumberField(TEXT("revision"), Revision);
		bool bFinished = false;
		if (Result.IsValid() && Result->TryGetBoolField(TEXT("finished"), bFinished))
		{
			Out->SetBoolField(TEXT("finished"), bFinished);
		}
		Out->SetStringField(TEXT("hint"), TEXT("run 在后台跑：进度推送 gt.job 事件，结果用 gt.job 或 gt.report 查"));
		return true;
	}

	bool Step(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		UGameTestSubsystem* Subsystem = RequireSubsystem(Error);
		if (Subsystem == nullptr)
		{
			return false;
		}

		// 两种写法都收：{ step: {...} }，或者直接把步骤字段平铺在参数里。
		TSharedPtr<FJsonObject> StepJson;
		const TSharedPtr<FJsonObject>* Nested = nullptr;
		if (Args.IsValid() && Args->TryGetObjectField(TEXT("step"), Nested) && Nested != nullptr)
		{
			StepJson = *Nested;
		}
		else
		{
			StepJson = Args;
		}
		if (!StepJson.IsValid() || !StepJson->HasField(TEXT("action")))
		{
			Error = TEXT("gt.step 需要一个步骤：{ step: { action, target?, ... } }，动作清单见 gt.actions");
			return false;
		}

		TSharedPtr<FJsonObject> Result;
		Subsystem->RunStepInline(StepJson, Result);
		// 步骤本身失败（断言没过）是业务结论，照常返回 ok=false 和原因；超长的（快照）写文件。
		Out = Capped(Result, TEXT("step"), StructureMaxChars);
		return true;
	}

	bool Job(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		UGameTestSubsystem* Subsystem = RequireSubsystem(Error);
		if (Subsystem == nullptr)
		{
			return false;
		}
		const int32 Keep = FMath::Clamp(Int(Args, TEXT("steps"), DefaultJobSteps), 0, 500);
		Out = Capped(TrimSteps(Subsystem->BuildJobJson(), Keep), TEXT("job"), StructureMaxChars);
		return true;
	}

	bool Runs(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		UGameTestSubsystem* Subsystem = RequireSubsystem(Error);
		if (Subsystem == nullptr)
		{
			return false;
		}
		Out = Capped(Subsystem->BuildRunsJson(), TEXT("runs"), DefaultMaxChars);
		return true;
	}

	bool Report(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		UGameTestSubsystem* Subsystem = RequireSubsystem(Error);
		if (Subsystem == nullptr)
		{
			return false;
		}
		const FString RunId = Str(Args, TEXT("runId"));
		if (RunId.IsEmpty())
		{
			Error = TEXT("gt.report 需要 runId（gt.runs 列出最近的 run）");
			return false;
		}

		const TSharedPtr<FJsonObject> Result = Subsystem->BuildRunJson(RunId);
		bool bOk = false;
		if (!Result->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
		{
			Error = Result->GetStringField(TEXT("error"));
			return false;
		}
		Out = Capped(Result->GetObjectField(TEXT("report")), TEXT("report"), StructureMaxChars);
		return true;
	}

	bool Stop(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		UGameTestSubsystem* Subsystem = RequireSubsystem(Error);
		if (Subsystem == nullptr)
		{
			return false;
		}
		Subsystem->CancelCurrentRun();
		Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("message"), TEXT("已请求取消当前 run（当前步标失败，余下步骤跳过）"));
		return true;
	}

	bool RecordStart(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		if (!FGameTestRecorder::Get().Start(Str(Args, TEXT("name")), Bool(Args, TEXT("startPie"), true), Error))
		{
			return false;
		}
		Out = FGameTestRecorder::Get().DescribeStatus(/*InLastSteps=*/5);
		return true;
	}

	bool RecordMark(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		if (!FGameTestRecorder::Get().Mark(Str(Args, TEXT("text")), Bool(Args, TEXT("screenshot"), false), Error))
		{
			return false;
		}
		Out = FGameTestRecorder::Get().DescribeStatus(/*InLastSteps=*/3);
		return true;
	}

	bool RecordStop(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		if (!FGameTestRecorder::Get().Stop(Bool(Args, TEXT("save"), true), Out, Error))
		{
			return false;
		}
		Out = Capped(Out, TEXT("record"), DefaultMaxChars);
		return true;
	}

	bool RecordStatus(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		const int32 Last = FMath::Clamp(Int(Args, TEXT("last"), 10), 0, 50);
		Out = Capped(FGameTestRecorder::Get().DescribeStatus(Last), TEXT("record"), DefaultMaxChars);
		return true;
	}

	void Add(const TCHAR* Name, bool (*Op)(const TSharedPtr<FJsonObject>&, TSharedPtr<FJsonObject>&, FString&))
	{
		if (FAgentPanelBridge::Get().AddExtensionOp(Name, Op))
		{
			RegisteredOps.Add(Name);
		}
	}

	/** 作业快照 → `gt.job` 事件：只带计数和最后一步，完整报告用 gt.job / gt.report 取。 */
	void PublishJob(const TSharedRef<FJsonObject>& InJob)
	{
		const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : InJob->Values)
		{
			if (Pair.Key != TEXT("steps"))
			{
				Event->SetField(Pair.Key, Pair.Value);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
		if (InJob->TryGetArrayField(TEXT("steps"), Steps) && Steps != nullptr && Steps->Num() > 0)
		{
			Event->SetField(TEXT("lastStep"), Steps->Last());
		}

		bool bBusy = false;
		InJob->TryGetBoolField(TEXT("busy"), bBusy);
		Event->SetStringField(TEXT("phase"), bBusy ? TEXT("progress") : TEXT("finished"));
		FAgentPanelBridge::Get().PublishEvent(TEXT("gt.job"), Event);
	}
}

void Register()
{
	Add(TEXT("gt.health"), &Health);
	Add(TEXT("gt.actions"), &Actions);
	Add(TEXT("gt.scenarios"), &Scenarios);
	Add(TEXT("gt.run"), &Run);
	Add(TEXT("gt.step"), &Step);
	Add(TEXT("gt.job"), &Job);
	Add(TEXT("gt.runs"), &Runs);
	Add(TEXT("gt.report"), &Report);
	Add(TEXT("gt.stop"), &Stop);
	Add(TEXT("gt.record.start"), &RecordStart);
	Add(TEXT("gt.record.mark"), &RecordMark);
	Add(TEXT("gt.record.stop"), &RecordStop);
	Add(TEXT("gt.record.status"), &RecordStatus);

	JobHandle = GameTestHooks::OnJobChanged().AddStatic(&PublishJob);
	RecordHandle = GameTestHooks::OnRecordChanged().AddLambda([](const TSharedRef<FJsonObject>& InEvent)
	{
		FAgentPanelBridge::Get().PublishEvent(TEXT("gt.record"), InEvent);
	});
}

void Unregister()
{
	GameTestHooks::OnJobChanged().Remove(JobHandle);
	GameTestHooks::OnRecordChanged().Remove(RecordHandle);
	for (const FString& Name : RegisteredOps)
	{
		FAgentPanelBridge::Get().RemoveOp(Name);
	}
	RegisteredOps.Reset();
}
}
