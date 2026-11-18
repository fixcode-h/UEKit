// Copyright Epic Games, Inc. All Rights Reserved.

#include "Integration/GameTestRpc.h"
#include "Integration/GameTestRpcKeys.h"
#include "Integration/GameTestRpcScript.h"
#include "GameTestCompat.h"

#include "AgentPanelBridge.h"
#include "Core/GameTestHistory.h"
#include "Core/GameTestHooks.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestRunConfig.h"
#include "Core/GameTestScenario.h"
#include "Core/GameTestWorld.h"
#include "Observe/GameTestInspect.h"
#include "Observe/GameTestNav.h"
#include "Observe/GameTestResolve.h"
#include "Integration/Live/GameTestLive.h"
#include "Core/GameTestAction.h"
#include "Operate/GameTestSkipIf.h"
#include "Observe/GameTestCondition.h"
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
	FDelegateHandle FlowHandle;

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
				 TEXT("name"), TEXT("scenario"), TEXT("succeeded"), TEXT("total"), TEXT("failed"), TEXT("planned"), TEXT("busy"), TEXT("outputDir") })
		{
			if (const TSharedPtr<FJsonValue> Value = InJson->TryGetField(Field))
			{
				Out->SetField(Field, Value);
			}
		}
		// 批量的计数留在摘要里（每项的明细在文件里）：Agent 不读文件也知道批量过没过
		const TSharedPtr<FJsonObject>* Batch = nullptr;
		if (InJson->TryGetObjectField(TEXT("batch"), Batch) && Batch != nullptr)
		{
			const TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>(**Batch);
			Counts->RemoveField(TEXT("items"));
			Out->SetObjectField(TEXT("batch"), Counts);
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
		// 导航网格：move 步骤有网格时寻路，没有时按轨迹或探路（面板据此提示）。
		Out->SetObjectField(TEXT("nav"), GameTestNav::Describe(GameTestWorld::FindGameWorld()));
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

	/** 字符串数组参数（scenarios、targets）；不是数组或没有时为空。 */
	TArray<FString> Strings(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Args.IsValid() && Args->TryGetArrayField(Name, Values) && Values != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Text;
				if (Value.IsValid() && Value->TryGetString(Text) && !Text.TrimStartAndEnd().IsEmpty())
				{
					Out.Add(Text.TrimStartAndEnd());
				}
			}
		}
		return Out;
	}

	/** `gt.run tag=` / `scenarios=[…]`：批量运行，马上返回批量编号，进度在 gt.job 的 batch 里。 */
	bool RunBatch(UGameTestSubsystem* Subsystem, const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		const FString Tag = Str(Args, TEXT("tag"));
		TArray<FString> Scenarios = Strings(Args, TEXT("scenarios"));
		FString Label = Str(Args, TEXT("name"));
		if (!Tag.IsEmpty())
		{
			Scenarios = GameTestScenario::FindByTag(ScenarioRootDir(), Tag);
			if (Scenarios.Num() == 0)
			{
				Error = FString::Printf(TEXT("场景根里没有带 tag「%s」的场景（场景文件顶层的 tags 数组；gt.scenarios 能看到每个场景的 tags）"), *Tag);
				return false;
			}
			Label = Label.IsEmpty() ? FString::Printf(TEXT("tag:%s"), *Tag) : Label;
		}

		TSharedPtr<FJsonObject> Result;
		if (!Subsystem->BeginBatch(Scenarios, Label.IsEmpty() ? TEXT("batch") : Label, Result))
		{
			FString Reason;
			Error = Result.IsValid() && Result->TryGetStringField(TEXT("error"), Reason) ? Reason : TEXT("批量运行没能开始");
			return false;
		}
		Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("batchId"), Result->GetStringField(TEXT("batchId")));
		Out->SetNumberField(TEXT("scenarios"), Scenarios.Num());
		Out->SetStringField(TEXT("hint"), TEXT("批量在后台一个接一个跑：进度和每个场景的结果在 gt.job 的 batch 里（也推送 gt.job 事件），某个场景的细节用 gt.report 查它的 runId"));
		Out = Capped(Out, TEXT("run"), DefaultMaxChars);
		return true;
	}

	bool Run(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		UGameTestSubsystem* Subsystem = RequireSubsystem(Error);
		if (Subsystem == nullptr)
		{
			return false;
		}

		if (!Str(Args, TEXT("tag")).IsEmpty() || (Args.IsValid() && Args->HasField(TEXT("scenarios"))))
		{
			return RunBatch(Subsystem, Args, Out, Error);
		}

		FGameTestScenarioRun ToRun;
		TArray<TSharedPtr<FJsonObject>>& Steps = ToRun.Steps;

		const FString ScenarioPath = Str(Args, TEXT("scenario"));
		if (!ScenarioPath.IsEmpty())
		{
			if (!GameTestScenario::LoadForRun(ScenarioRootDir(), ScenarioPath, ToRun, Error))
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
			ToRun.Name = Str(Args, TEXT("name"));
			ToRun.StepDelayMs = Int(Args, TEXT("stepDelayMs"), -1);
		}

		// 1.9 调试：params、from / to、断点、失败时暂停
		if (!GameTestRpcScript::ReadRunOptions(Args, ToRun, Error))
		{
			return false;
		}

		// 「全部照做」：忽略所有已达成就跳过的条件，每一步都真做（测第一次领取、新号这类路径）
		bool bIgnoreSkipIf = false;
		if (Args.IsValid() && Args->TryGetBoolField(TEXT("ignoreSkipIf"), bIgnoreSkipIf))
		{
			ToRun.bIgnoreSkipIf = bIgnoreSkipIf;
		}

		FString RunId;
		TSharedPtr<FJsonObject> Result;
		if (!Subsystem->BeginScenarioFileRun(ToRun, RunId, Result))
		{
			FString Reason;
			Error = Result.IsValid() && Result->TryGetStringField(TEXT("error"), Reason) ? Reason : TEXT("run 没能开始");
			return false;
		}

		Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("runId"), RunId);
		Out->SetNumberField(TEXT("steps"), Steps.Num());
		Out->SetNumberField(TEXT("revision"), ToRun.Revision);
		if (!ToRun.Scenario.IsEmpty())
		{
			Out->SetStringField(TEXT("scenario"), ToRun.Scenario);
		}
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

		// 块（if / loop / group / call…）或带了 vars / params：包成一份临时场景在后台跑，回 runId
		const TSharedPtr<FJsonObject>* Vars = nullptr;
		const bool bHasVars = Args.IsValid() && Args->TryGetObjectField(TEXT("vars"), Vars) && Vars != nullptr;
		const bool bHasParams = Args.IsValid() && Args->HasField(TEXT("params"));
		if (GameTestRpcScript::IsScriptStep(*StepJson) || bHasVars || bHasParams)
		{
			FGameTestScenarioRun ToRun;
			ToRun.Name = TEXT("step");
			ToRun.Steps.Add(StepJson);
			ToRun.Json = MakeShared<FJsonObject>();
			ToRun.Json->SetStringField(TEXT("kind"), GameTestScenario::GetKind());
			ToRun.Json->SetNumberField(TEXT("schemaVersion"), GameTestScenario::SupportedSchemaVersion);
			ToRun.Json->SetStringField(TEXT("name"), ToRun.Name);
			if (bHasVars)
			{
				ToRun.Json->SetObjectField(TEXT("vars"), *Vars);
			}
			TArray<TSharedPtr<FJsonValue>> StepValues;
			StepValues.Add(MakeShared<FJsonValueObject>(StepJson));
			ToRun.Json->SetArrayField(TEXT("steps"), StepValues);
			if (!GameTestRpcScript::ReadRunOptions(Args, ToRun, Error))
			{
				return false;
			}
			FString RunId;
			TSharedPtr<FJsonObject> Started;
			if (!Subsystem->BeginScenarioFileRun(ToRun, RunId, Started))
			{
				FString Reason;
				Error = Started.IsValid() && Started->TryGetStringField(TEXT("error"), Reason) ? Reason : TEXT("步骤没能开始");
				return false;
			}
			Out = Capped(Started, TEXT("step"), StructureMaxChars);
			return true;
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
		if (!Result.IsValid() || !Result->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
		{
			if (Result.IsValid())
			{
				Result->TryGetStringField(TEXT("error"), Error);
			}
			if (Error.IsEmpty())
			{
				Error = FString::Printf(TEXT("读不到运行 %s 的结果"), *RunId);
			}
			return false;
		}
		// 缺 report 字段时 GetObjectField 会返回空指针，交出去会被解引用，必须先判空
		const TSharedPtr<FJsonObject>* ReportJson = nullptr;
		if (!Result->TryGetObjectField(TEXT("report"), ReportJson) || ReportJson == nullptr || !ReportJson->IsValid())
		{
			Error = FString::Printf(TEXT("运行 %s 还没有报告，稍后再查"), *RunId);
			return false;
		}
		Out = Capped(*ReportJson, TEXT("report"), StructureMaxChars);
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
		// 录制截图参数（1.3.2）：shots=false 关掉，对象时覆盖外扩、尺寸（面板「设置 → 插件 → UE」里改）
		FGameTestShotOptions ShotOptions;
		if (!FGameTestShotOptions::FromJson(Args.IsValid() ? Args->TryGetField(TEXT("shots")) : TSharedPtr<FJsonValue>(), ShotOptions, Error))
		{
			return false;
		}
		FGameTestRecorder::Get().SetShotOptions(ShotOptions);
		// 点到容器条目时怎么问（1.4.0）：askKey = unsure（默认）/ always / never；askTimeoutSec 停多久自动继续，0 = 一直等
		double AskTimeout = 0.0;
		if (Args.IsValid())
		{
			GameTestJson::TryGetNumber(*Args, TEXT("askTimeoutSec"), AskTimeout);
		}
		FGameTestRecorder::Get().SetKeyAskOptions(Str(Args, TEXT("askKey")), AskTimeout);
		// 推断跳过条件（1.6.0）：inferSkip = suggest（默认，1.8.0 起：只列候选，人来加）/ auto（高把握的直接写 skipIf）/ off；stateWindowMs 点完看多久
		const FString InferSkip = Str(Args, TEXT("inferSkip"));
		EGameTestInferSkip InferMode = EGameTestInferSkip::Suggest;
		if (InferSkip == TEXT("auto"))
		{
			InferMode = EGameTestInferSkip::Auto;
		}
		else if (InferSkip == TEXT("off"))
		{
			InferMode = EGameTestInferSkip::Off;
		}
		else if (!InferSkip.IsEmpty() && InferSkip != TEXT("suggest"))
		{
			Error = TEXT("inferSkip 只能是 suggest（默认）、auto 或 off");
			return false;
		}
		FGameTestRecorder::Get().SetStateOptions(InferMode, Int(Args, TEXT("stateWindowMs"), FGameTestRecordState::DefaultWindowMs));
		// 录成步骤段、从这里接着录（1.10.0）：group=true 时录到的步骤装进一个 group；
		// insert={scenario, after} 时录到的段插进这个场景（相对场景根）里 id 为 after 的步骤后面（after 省略 = 主流程最后），不另存新文件
		FString InsertScenario;
		FString InsertAfter;
		if (Args.IsValid() && Args->HasField(TEXT("insert")))
		{
			const TSharedPtr<FJsonObject>* Insert = nullptr;
			if (!Args->TryGetObjectField(TEXT("insert"), Insert) || Insert == nullptr || !Insert->IsValid() || !(*Insert)->TryGetStringField(TEXT("scenario"), InsertScenario) || InsertScenario.IsEmpty())
			{
				Error = TEXT("insert 要写成 {\"scenario\": \"相对场景根的路径\", \"after\": \"步骤 id（可省）\"}");
				return false;
			}
			(*Insert)->TryGetStringField(TEXT("after"), InsertAfter);
		}
		FGameTestRecorder::Get().SetSegmentOptions(Bool(Args, TEXT("group"), false), InsertScenario, InsertAfter);
		if (!FGameTestRecorder::Get().Start(Str(Args, TEXT("name")), Bool(Args, TEXT("startPie"), true), Bool(Args, TEXT("keepPace"), false), Error))
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

	/** 录制快捷操作：录移动开始 / 结束、放弃、记下位置、在这附近、下一次点击录成断言（和游戏里的工具栏、Alt+数字同一个入口）。 */
	bool RecordMove(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		double Radius = 0.0;
		if (Args.IsValid())
		{
			GameTestJson::TryGetNumber(*Args, TEXT("radius"), Radius);
		}
		const FString Phase = Str(Args, TEXT("phase"));
		if (!FGameTestRecorder::Get().Quick(Phase.IsEmpty() ? FString(TEXT("toggle")) : Phase, Radius, Error))
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

	bool Inspect(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		TOptional<FVector2D> Point;
		if (Args.IsValid() && Args->HasField(TEXT("x")) && Args->HasField(TEXT("y")))
		{
			Point = FVector2D(Args->GetNumberField(TEXT("x")), Args->GetNumberField(TEXT("y")));
		}
		TSharedPtr<FJsonObject> Result;
		if (!GameTestInspect::Inspect(Point, Result, Error))
		{
			return false;
		}
		Out = Capped(Result, TEXT("inspect"), DefaultMaxChars);
		return true;
	}

	/**
	 * `gt.resolve checks`：步骤的跳过条件此刻成立吗。和回放用同一个判断（GameTestSkipIf::Holds），
	 * 每项回 `{index, holds, say, actual?, error?}`；步骤写错的那项带 error，不影响别的项。
	 */
	bool CheckSkipIf(UGameTestSubsystem* Subsystem, const TArray<TSharedPtr<FJsonValue>>& InChecks, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		if (InChecks.Num() == 0 || InChecks.Num() > 50)
		{
			Error = TEXT("checks 要 1–50 个步骤（每个带 skipIf）");
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Items;
		for (int32 Index = 0; Index < InChecks.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Index);
			const TSharedPtr<FJsonObject>* StepJson = nullptr;
			FGameTestStep Step;
			TArray<FString> StepErrors;
			if (!InChecks[Index].IsValid() || !InChecks[Index]->TryGetObject(StepJson) || StepJson == nullptr || !StepJson->IsValid())
			{
				Item->SetStringField(TEXT("error"), TEXT("这一项不是步骤对象"));
			}
			else if (!GameTestActions::ParseStep(*StepJson, Index, Step, StepErrors))
			{
				Item->SetStringField(TEXT("error"), FString::Join(StepErrors, TEXT("；")));
			}
			else
			{
				const TSharedPtr<FJsonObject> SkipIf = GameTestSkipIf::Get(Step.Params);
				if (!SkipIf.IsValid())
				{
					Item->SetStringField(TEXT("error"), TEXT("这一步没写 skipIf"));
				}
				else
				{
					FString Actual;
					Item->SetBoolField(TEXT("holds"), GameTestSkipIf::Holds(*SkipIf, Step, Subsystem->GetHandles(), Actual));
					Item->SetStringField(TEXT("say"), GameTestCondition::Describe(*SkipIf));
					if (!Actual.IsEmpty())
					{
						Item->SetStringField(TEXT("actual"), Actual);
					}
				}
			}
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		Out = MakeShared<FJsonObject>();
		Out->SetArrayField(TEXT("checks"), Items);
		Out = Capped(Out, TEXT("resolve"), DefaultMaxChars);
		return true;
	}

	/** `gt.resolve`：核对选择器（targets 列表，或 scenario 里每一步的 target / toTarget），只解析不执行。 */
	bool Resolve(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		UGameTestSubsystem* Subsystem = RequireSubsystem(Error);
		if (Subsystem == nullptr)
		{
			return false;
		}

		// 1.9：条件和表达式此刻的值（调试面板、条件编辑器的「试一下」）
		if (GameTestRpcScript::HasScriptResolve(Args))
		{
			if (!GameTestRpcScript::ResolveScript(Args, Out, Error))
			{
				return false;
			}
			Out = Capped(Out, TEXT("resolve"), StructureMaxChars);
			return true;
		}

		// 「现在核对」（1.6.0）：checks 是带 skipIf 的步骤数组，回每一步此刻跳不跳（只判断，不动手）
		const TArray<TSharedPtr<FJsonValue>>* CheckValues = nullptr;
		if (Args.IsValid() && Args->TryGetArrayField(TEXT("checks"), CheckValues) && CheckValues != nullptr)
		{
			return CheckSkipIf(Subsystem, *CheckValues, Out, Error);
		}

		TArray<GameTestResolve::FTarget> Targets;
		const FString ScenarioPath = Str(Args, TEXT("scenario"));
		if (!ScenarioPath.IsEmpty())
		{
			FGameTestScenarioRun Run;
			if (!GameTestScenario::LoadForRun(ScenarioRootDir(), ScenarioPath, Run, Error))
			{
				return false;
			}
			Targets = GameTestResolve::CollectTargets(Run.Steps);
		}
		for (const FString& Text : Strings(Args, TEXT("targets")))
		{
			GameTestResolve::FTarget Target;
			Target.Target = Text;
			Targets.Add(MoveTemp(Target));
		}
		if (Targets.Num() == 0)
		{
			Error = ScenarioPath.IsEmpty()
				? TEXT("gt.resolve 要 targets（选择器数组）或 scenario（核对场景里每一步的 target）")
				: TEXT("这个场景里没有写选择器的步骤（target / toTarget）");
			return false;
		}

		TSharedPtr<FJsonObject> Result;
		if (!GameTestResolve::Resolve(Targets, Subsystem->GetHandles(), Result, Error))
		{
			return false;
		}
		Out = Capped(Result, TEXT("resolve"), StructureMaxChars);
		return true;
	}

	/**
	 * `gt.history`：按场景文件的运行历史（最近 10 次）。给 scenario 回这个场景的每一次（时间、成败、第几步失败、产物目录）；
	 * 不给回所有场景的通过 / 失败串（最新在前，p 通过 f 失败），列表上画历史条用。
	 */
	bool History(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		const TSharedPtr<FJsonObject> All = GameTestHistory::Load();
		const FString Scenario = Str(Args, TEXT("scenario")).Replace(TEXT("\\"), TEXT("/"));
		Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("keep"), GameTestHistory::Keep);
		if (!Scenario.IsEmpty())
		{
			Out->SetStringField(TEXT("scenario"), Scenario);
			const TSharedPtr<FJsonObject> Scenarios = All->GetObjectField(TEXT("scenarios"));
			const TArray<TSharedPtr<FJsonValue>>* Runs = nullptr;
			Out->SetArrayField(TEXT("runs"),
				Scenarios->TryGetArrayField(Scenario, Runs) && Runs != nullptr ? *Runs : TArray<TSharedPtr<FJsonValue>>());
			Out = Capped(Out, TEXT("history"), DefaultMaxChars);
			return true;
		}
		Out->SetObjectField(TEXT("scenarios"), GameTestHistory::Compact(*All));
		Out = Capped(Out, TEXT("history"), StructureMaxChars);
		return true;
	}

	/**
	 * 项目运行配置（场景根的 _settings.json：守护、UI 等待上限、脚本开关）。
	 * 不带 config 是读；带 config 是校验后整份写回（dryRun 只校验不落盘），面板 GameTest 页「运行设置」用。
	 */
	bool Config(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		const FString Path = GameTestRunConfig::GetPath();
		FGameTestRunConfig RunConfig;
		const TSharedPtr<FJsonObject>* Incoming = nullptr;
		const bool bWrite = Args.IsValid() && Args->TryGetObjectField(TEXT("config"), Incoming) && Incoming != nullptr;
		const bool bDryRun = Bool(Args, TEXT("dryRun"), false);
		if (bWrite)
		{
			// 面板只发它认得的字段：文件里别的字段（api、objectKeys…，面板别的页或手写的）原样留着，不被这次写回抹掉。
			const TSharedRef<FJsonObject> Merged = MakeShared<FJsonObject>(**Incoming);
			if (IFileManager::Get().FileExists(*Path))
			{
				FString ReadError;
				if (const TSharedPtr<FJsonObject> Existing = GameTestJson::ParseObjectFromFile(Path, ReadError))
				{
					GameTestRunConfig::PreserveUnknown(*Merged, *Existing);
				}
			}
			if (!GameTestRunConfig::FromJson(*Merged, RunConfig, Error))
			{
				return false;
			}
			if (!bDryRun && !GameTestRunConfig::Save(RunConfig, Error))
			{
				return false;
			}
		}
		else if (!GameTestRunConfig::Load(RunConfig, Error))
		{
			return false;
		}

		Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("path"), GameTestShot::ToProjectRelative(Path));
		Out->SetBoolField(TEXT("exists"), IFileManager::Get().FileExists(*Path));
		Out->SetObjectField(TEXT("config"), GameTestRunConfig::ToJson(RunConfig));
		// 项目设置里的默认值：配置没写 uiWaitTimeoutMs 时按它，面板显示「默认：…」
		const UGameTestSettings* Settings = GetDefault<UGameTestSettings>();
		Out->SetNumberField(TEXT("settingsUiWaitTimeoutMs"), Settings != nullptr ? Settings->UiWaitTimeoutMs : 0);
		if (bWrite)
		{
			Out->SetBoolField(TEXT("dryRun"), bDryRun);
			Out->SetStringField(TEXT("message"), bDryRun ? TEXT("演练：配置有效，没有写盘") : TEXT("已保存，下次运行生效"));
		}
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
		for (const auto& Pair : InJob->Values)
		{
			const FString PairKey = GAMETEST_JSON_KEY(Pair.Key);
			if (PairKey != TEXT("steps"))
			{
				Event->SetField(PairKey, Pair.Value);
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
	Add(TEXT("gt.record.move"), &RecordMove);
	Add(TEXT("gt.record.stop"), &RecordStop);
	Add(TEXT("gt.record.status"), &RecordStatus);
	Add(TEXT("gt.record.key"), &GameTestRpcKeys::RecordKey);
	Add(TEXT("gt.keys"), &GameTestRpcKeys::Keys);
	Add(TEXT("gt.keys.preview"), &GameTestRpcKeys::Preview);
	Add(TEXT("gt.inspect"), &Inspect);
	Add(TEXT("gt.resolve"), &Resolve);
	Add(TEXT("gt.history"), &History);
	Add(TEXT("gt.config"), &Config);
	Add(TEXT("gt.api"), &GameTestRpcScript::Api);
	Add(TEXT("gt.vars"), &GameTestRpcScript::Vars);
	Add(TEXT("gt.debug"), &GameTestRpcScript::Debug);
	Add(TEXT("gt.validate"), &GameTestRpcScript::Validate);
	// 1.10.0：连着游戏编辑（抓控件、此刻的结果实时推送）
	Add(TEXT("gt.pick"), &GameTestLive::Pick);
	Add(TEXT("gt.watch"), &GameTestLive::Watch);

	JobHandle = GameTestHooks::OnJobChanged().AddStatic(&PublishJob);
	RecordHandle = GameTestHooks::OnRecordChanged().AddLambda([](const TSharedRef<FJsonObject>& InEvent)
	{
		FAgentPanelBridge::Get().PublishEvent(TEXT("gt.record"), InEvent);
	});
	FlowHandle = GameTestHooks::OnFlowEvent().AddLambda([](const TSharedRef<FJsonObject>& InEvent)
	{
		FAgentPanelBridge::Get().PublishEvent(TEXT("gt.flow"), InEvent);
	});
}

void Unregister()
{
	GameTestLive::Shutdown();
	GameTestHooks::OnJobChanged().Remove(JobHandle);
	GameTestHooks::OnRecordChanged().Remove(RecordHandle);
	GameTestHooks::OnFlowEvent().Remove(FlowHandle);
	for (const FString& Name : RegisteredOps)
	{
		FAgentPanelBridge::Get().RemoveOp(Name);
	}
	RegisteredOps.Reset();
}
}
