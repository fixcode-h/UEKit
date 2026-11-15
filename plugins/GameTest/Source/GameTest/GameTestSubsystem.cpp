// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameTestSubsystem.h"

#include "Core/GameTestAction.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestScenario.h"
#include "Core/GameTestWorld.h"
#include "Editor/GameTestEditorApi.h"
#include "Core/GameTestHooks.h"
#include "Input/GameTestInput.h"
#include "Shot/GameTestShot.h"
#include "GameTest.h"
#include "GameTestSettings.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWindow.h"

namespace
{
	/** 最后一步之后高亮还留多久：run 结束时不会"啪"地消失，让人来得及看最后一眼。 */
	constexpr double HighlightLingerSeconds = 2.5;

	/** 高亮层的 Z 序：盖在游戏自己的 UMG 之上（UMG 的 zOrder 通常是个位数）。 */
	constexpr int32 HighlightZOrder = 1000;

	const FLinearColor HighlightSuccessColor(0.35f, 1.0f, 0.45f, 1.0f);
	const FLinearColor HighlightFailureColor(1.0f, 0.35f, 0.35f, 1.0f);
}

/** 高亮覆盖层的持有者：Slate 类型只出现在本翻译单元里，头文件不必把 Slate 拖给每个包含它的模块。 */
struct FGameTestHighlightHolder
{
	/** 加进视口的那一层：外层 SOverlay 负责铺满，内层只画。 */
	TSharedPtr<SWidget> Overlay;

	/**
	 * 要画的框（**本层局部坐标**，单位是物理像素）。由执行器给的快照矩形减去窗口位置得到——
	 * 别在画的时候读控件几何：那条路上有视口 DPI 与窗口坐标两层换算，实测会把框画歪。
	 */
	bool bHasRect = false;
	FVector2D RectMin = FVector2D::ZeroVector;
	FVector2D RectMax = FVector2D::ZeroVector;

	/** 这一步成没成（决定框的颜色）。 */
	bool bSucceeded = true;

	/** 已经应用过的 touch 序号：相同就不重建覆盖层。 */
	int64 AppliedSerial = -1;

	/** 到这个时刻还没新动作就把覆盖层摘掉。 */
	double ExpiresAt = 0.0;
};

void FGameTestHighlightHolderDeleter::operator()(FGameTestHighlightHolder* InHolder) const
{
	delete InHolder;
}

namespace
{
	/**
	 * 线框层：把目标控件在屏幕上的矩形换算进本层坐标后描一个框。
	 *
	 * 它自己不下判断——框谁、成没成由持有者给（源头是执行器的 touch 记录）。用
	 * `AbsoluteToLocal` 而不是自己减窗口位置：窗口缩放/DPI 变化时它仍然对得上。
	 */
	class SGameTestHighlightOverlay : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SGameTestHighlightOverlay) {}
			SLATE_ARGUMENT(FGameTestHighlightHolder*, Holder)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			Holder = InArgs._Holder;
		}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			// 尺寸由外层 SOverlay 铺满决定，本层只负责画。
			return FVector2D::ZeroVector;
		}

		virtual int32 OnPaint(
			const FPaintArgs& InArgs,
			const FGeometry& InAllottedGeometry,
			const FSlateRect& InMyCullingRect,
			FSlateWindowElementList& OutDrawElements,
			const int32 InLayerId,
			const FWidgetStyle& InWidgetStyle,
			const bool bInParentEnabled) const override
		{
			// 基类（SLeafWidget）把 OnPaint 关成 private，不打算被调用：本层自己起一套层号。
			const int32 LayerId = InLayerId;

			if (Holder == nullptr || !Holder->bHasRect)
			{
				return LayerId;
			}

			const FLinearColor Color = Holder->bSucceeded ? HighlightSuccessColor : HighlightFailureColor;
			const FPaintGeometry PaintGeometry = InAllottedGeometry.ToPaintGeometry();

			// 持有者给的是**窗口客户区物理像素**的矩形；这一层从窗口左上角起还带一点平移与缩放，
			// 所以两个角都要过一遍 `AbsoluteToLocal`——它同时消掉平移与缩放（实测：只除缩放会让
			// 框整体下移一整行、只减窗口位置会让框小一圈）。
			const FVector2D Min = InAllottedGeometry.AbsoluteToLocal(Holder->RectMin);
			const FVector2D Max = InAllottedGeometry.AbsoluteToLocal(Holder->RectMax);

			const TArray<FVector2D> Outline = {
				FVector2D(Min.X, Min.Y),
				FVector2D(Max.X, Min.Y),
				FVector2D(Max.X, Max.Y),
				FVector2D(Min.X, Max.Y),
				FVector2D(Min.X, Min.Y),
			};
			FSlateDrawElement::MakeLines(OutDrawElements, LayerId, PaintGeometry, Outline,
				ESlateDrawEffect::None, Color, /*bAntialias=*/true, /*Thickness=*/2.5f);

			return LayerId + 1;
		}

	private:
		FGameTestHighlightHolder* Holder = nullptr;
	};
}

UGameTestSubsystem* UGameTestSubsystem::Get()
{
	return GEngine != nullptr ? GEngine->GetEngineSubsystem<UGameTestSubsystem>() : nullptr;
}

// 构造与析构定义在这里而不是头文件里：头文件只有高亮持有者的前向声明，
// TUniquePtr 的删除器需要完整类型（否则 UHT 生成的翻译单元会报"删除不完整类型"）。
UGameTestSubsystem::UGameTestSubsystem() = default;
UGameTestSubsystem::~UGameTestSubsystem() = default;

void UGameTestSubsystem::Initialize(FSubsystemCollectionBase& InCollection)
{
	Super::Initialize(InCollection);

	Runner = MakeUnique<FGameTestRunner>();
	if (const UGameTestSettings* Settings = GetDefault<UGameTestSettings>())
	{
		RunHistory.SetCapacity(Settings->MaxRunHistory);
	}
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UGameTestSubsystem::Tick), 0.0f);

	UE_LOG(LogGameTest, Display, TEXT("GameTest 就绪（%s 进程）"), GIsEditor ? TEXT("编辑器") : TEXT("游戏"));
}

void UGameTestSubsystem::Deinitialize()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	RootedObjects.Reset();
	Handles.Reset();
	ClearHighlight();
	Runner.Reset();

	Super::Deinitialize();
}

bool UGameTestSubsystem::Tick(float InDeltaSeconds)
{
	const double Now = FPlatformTime::Seconds();
	++TickCount;
	Runner->Tick(Now);
	UpdateHighlight(Now);

	// run 期间每 5 秒记一条：编辑器被节流/卡住时，这是唯一能确认"游戏线程还在走"的证据。
	if (Runner->IsBusy() && Now - LastHeartbeatLog > 5.0)
	{
		LastHeartbeatLog = Now;
		UE_LOG(LogGameTest, Display, TEXT("run 进行中：已完成 %d/%d 步（tick %lld，帧 %llu，线程 %u）"),
			Runner->GetReport().Steps.Num(), Runner->GetQueueLength(), TickCount, GFrameCounter,
			FPlatformTLS::GetCurrentThreadId());
	}

	// job 快照在"步骤数变了"或"忙碌状态变了"时刷新：读方和推送拿到的是一份自洽的报告，
	// 游戏线程也不用每帧序列化。只看步骤数会漏掉"最后一步完成、run 收尾"那次状态翻转。
	// 还要看 runId：gt.run 里会立刻推进一次，第一步就失败的 run 在两次 Tick 之间已经开始又结束，
	// 忙碌状态没变过；步骤数又碰巧和上一次 run 一样时，不看 runId 就永远不推「结束」，面板一直显示运行中。
	const FGameTestReport& Report = Runner->GetReport();
	const bool bBusy = Runner->IsBusy();
	if (Report.Steps.Num() != LastSnapshotStepCount || bBusy != LastSnapshotBusy || CurrentRunId != LastSnapshotRunId)
	{
		LastSnapshotStepCount = Report.Steps.Num();
		LastSnapshotBusy = bBusy;
		LastSnapshotRunId = CurrentRunId;

		const TSharedPtr<FJsonObject> Snapshot = Report.ToJson();
		Snapshot->SetBoolField(TEXT("busy"), bBusy);
		Snapshot->SetStringField(TEXT("runId"), CurrentRunId);
		// 一共几步（total 是已完成的步数）：面板画进度条要用。
		Snapshot->SetNumberField(TEXT("planned"), Runner->GetQueueLength());
		Snapshot->SetStringField(TEXT("status"), bBusy ? TEXT("running") : (Report.Steps.Num() > 0 ? TEXT("finished") : TEXT("idle")));

		{
			FScopeLock Lock(&JobLock);
			JobSnapshot = Snapshot;
		}
		// 推给订阅方（UELink 的 gt.job 事件）：面板据此刷新进度，不用轮询。
		GameTestHooks::OnJobChanged().Broadcast(Snapshot.ToSharedRef());
	}

	return true;
}

void UGameTestSubsystem::ApplyBackgroundThrottleOverride()
{
	if (bThrottleOverridden)
	{
		return;
	}

	bool bPrevious = false;
	if (GameTestEditor::SetBackgroundThrottleEnabled(/*bInEnabled=*/false, bPrevious))
	{
		bThrottleOverridden = true;
		bPreviousThrottleValue = bPrevious;
		UE_LOG(LogGameTest, Display, TEXT("已关掉后台节流（编辑器不在前台时也要持续 Tick）"));
	}
}

void UGameTestSubsystem::RestoreBackgroundThrottle()
{
	if (!bThrottleOverridden)
	{
		return;
	}

	bool bIgnored = false;
	GameTestEditor::SetBackgroundThrottleEnabled(bPreviousThrottleValue, bIgnored);
	bThrottleOverridden = false;
}

void UGameTestSubsystem::UpdateHighlight(double InNowSeconds)
{
	UGameViewportClient* ViewportClient = GEngine != nullptr ? GEngine->GameViewport : nullptr;
	if (ViewportClient == nullptr)
	{
		// 没有游戏视口（编辑器没起 PIE、命令行、无头）：没什么可框的，静默跳过。
		ClearHighlight();
		return;
	}

	const int64 Serial = Runner->GetTouchSerial();
	const bool bIsNewTouch = Runner->GetTouchedWidget().IsValid()
		&& (!Highlight.IsValid() || Highlight->AppliedSerial != Serial);

	if (bIsNewTouch)
	{
		if (!Highlight.IsValid())
		{
			Highlight.Reset(new FGameTestHighlightHolder());

			// 外层 SOverlay 负责铺满视口；HitTestInvisible——框不能挡住游戏自己的输入。
			Highlight->Overlay = SNew(SOverlay)
				.Visibility(EVisibility::HitTestInvisible)
				+ SOverlay::Slot()
				.HAlign(HAlign_Fill)
				.VAlign(VAlign_Fill)
				[
					SNew(SGameTestHighlightOverlay).Holder(Highlight.Get())
				];

			ViewportClient->AddViewportWidgetContent(Highlight->Overlay.ToSharedRef(), HighlightZOrder);
		}

		// 执行器给的是"快照坐标系"的矩形（与截图标注同一份），本层的原点是窗口客户区左上角
		// → 减掉窗口位置即可。窗口位置在这儿算一次，别留到每帧的画里。
		FVector2D RectMin = FVector2D::ZeroVector;
		FVector2D RectMax = FVector2D::ZeroVector;
		const bool bHasRect = Runner->GetTouchedRect(RectMin, RectMax);
		if (bHasRect)
		{
			FVector2D WindowPosition = FVector2D::ZeroVector;
			if (const TSharedPtr<SWindow> Window = ViewportClient->GetWindow())
			{
				WindowPosition = Window->GetPositionInScreen();
			}

			Highlight->RectMin = RectMin - WindowPosition;
			Highlight->RectMax = RectMax - WindowPosition;
		}
		Highlight->bHasRect = bHasRect;

		Highlight->AppliedSerial = Serial;
		Highlight->bSucceeded = Runner->WasTouchSuccessful();
		Highlight->ExpiresAt = InNowSeconds + HighlightLingerSeconds;
	}

	// 到点了就摘掉：覆盖层只在"还有东西可看"的时候存在。
	if (Highlight.IsValid() && InNowSeconds > Highlight->ExpiresAt)
	{
		ClearHighlight();
	}

	if (Highlight.IsValid() && Highlight->Overlay.IsValid())
	{
		// 目标控件可能在动（动画、列表滚动）：每帧重画一次，框才跟得住它。
		Highlight->Overlay->Invalidate(EInvalidateWidgetReason::Paint);
	}
}

void UGameTestSubsystem::ClearHighlight()
{
	if (Highlight.IsValid() && Highlight->Overlay.IsValid())
	{
		if (UGameViewportClient* ViewportClient = GEngine != nullptr ? GEngine->GameViewport : nullptr)
		{
			ViewportClient->RemoveViewportWidgetContent(Highlight->Overlay.ToSharedRef());
		}
	}

	Highlight.Reset();
}

bool UGameTestSubsystem::StartRunOnGameThread(
	const TArray<TSharedPtr<FJsonObject>>& InSteps,
	const FString& InName,
	int32 InRevision,
	FString& OutRunId,
	bool& bOutAlreadyFinished,
	TSharedPtr<FJsonObject>& OutResult,
	int32 InStepDelayMs)
{
	bOutAlreadyFinished = false;
	OutRunId.Reset();

	// 解析失败不执行任何一步：错在配置上，让它整体失败比跑一半更安全。
	TArray<FGameTestStep> Steps;
	TArray<FString> Errors;
	for (int32 Index = 0; Index < InSteps.Num(); ++Index)
	{
		FGameTestStep Step;
		TArray<FString> StepErrors;
		if (GameTestActions::ParseStep(InSteps[Index], Index, Step, StepErrors))
		{
			Steps.Add(Step);
		}
		else
		{
			for (const FString& Error : StepErrors)
			{
				Errors.Add(FString::Printf(TEXT("第 %d 步：%s"), Index + 1, *Error));
			}
		}
	}

	if (Errors.Num() > 0)
	{
		OutResult = MakeShared<FJsonObject>();
		OutResult->SetBoolField(TEXT("ok"), false);
		OutResult->SetStringField(TEXT("error"), FString::Join(Errors, TEXT("；")));
		OutResult->SetNumberField(TEXT("stepCount"), Steps.Num());
		return false;
	}

	const FString RunName = InName.IsEmpty() ? TEXT("run") : InName;
	OutRunId = GameTestRun::MakeRunId(RunName, RunCounter.Increment());

	const FString OutputDir = FPaths::Combine(
		GameTestShot::GetOutputRootDir(),
		FString::Printf(TEXT("%s-%s"), *RunName, *FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"))));

	ApplyBackgroundThrottleOverride();

	FGameTestRunRequest Request;
	Request.Name = RunName;
	Request.RunId = OutRunId;
	Request.Revision = InRevision;
	Request.OutputDir = OutputDir;
	Request.Steps = MoveTemp(Steps);
	if (const UGameTestSettings* Settings = GetDefault<UGameTestSettings>())
	{
		Request.DefaultStepTimeoutMs = Settings->DefaultStepTimeoutMs;
		Request.StepDelayMs = Settings->StepDelayMs;
	}
	if (InStepDelayMs >= 0)
	{
		Request.StepDelayMs = InStepDelayMs;
	}

	FString StartError;
	FGameTestRuntimeState RuntimeState;
	RuntimeState.Handles = &Handles;
	RuntimeState.RootedObjects = &RootedObjects;

	const bool bStarted = Runner->StartRun(
		Request,
		RuntimeState,
		[this, OutputDir](const FGameTestReport& InReport)
		{
			RestoreBackgroundThrottle();
			// 可选模块在这里清掉本次 run 留下的状态（EnhancedInput 模块停掉输入注入）。
			GameTestInput::ReleaseHeldKeys();
			GameTestHooks::OnRunFinished().Broadcast();
			WriteReportFile(InReport, OutputDir);
			// 历史里记下产物目录（相对项目根）：面板据此找截图和 report.json。
			const TSharedPtr<FJsonObject> Summary = InReport.ToJson();
			Summary->SetStringField(TEXT("outputDir"), GameTestShot::ToProjectRelative(OutputDir));
			RunHistory.Add(Summary);
			UE_LOG(LogGameTest, Display, TEXT("run '%s' 结束：ok=%d，步骤 %d 条"),
				*InReport.Name, InReport.bSucceeded ? 1 : 0, InReport.Steps.Num());
		},
		StartError);

	if (!bStarted)
	{
		RestoreBackgroundThrottle();

		OutResult = MakeShared<FJsonObject>();
		OutResult->SetBoolField(TEXT("ok"), false);
		OutResult->SetStringField(TEXT("error"), StartError);
		return false;
	}

	CurrentRunId = OutRunId;

	// 立刻推进一次：同步步骤不需要等下一帧，调用方一次调用就能拿到结果。
	Runner->Tick(FPlatformTime::Seconds());
	bOutAlreadyFinished = !Runner->IsBusy();
	return true;
}

TSharedPtr<FJsonObject> UGameTestSubsystem::BuildOutcomeJson(bool bInAlreadyFinished) const
{
	const FGameTestReport& Report = Runner->GetReport();

	const TSharedPtr<FJsonObject> Result = Report.ToJson();
	Result->SetBoolField(TEXT("ok"), bInAlreadyFinished ? Report.bSucceeded : false);
	Result->SetBoolField(TEXT("finished"), bInAlreadyFinished);
	Result->SetStringField(TEXT("runId"), CurrentRunId);

	// 已经跑完（或单步立刻出结果）时，把最后一条记录摊到顶层：
	// 调用方最关心的就是"这一步成不成、说了什么、看什么"。
	if (bInAlreadyFinished && Report.Steps.Num() > 0)
	{
		const FGameTestStepRecord& Last = Report.Steps.Last();
		Result->SetStringField(TEXT("status"), GameTestStatus::ToString(Last.Status));
		Result->SetStringField(TEXT("message"), Last.Message);
		Result->SetStringField(TEXT("action"), Last.Step.Action.ToString());
		if (Last.Payload.IsValid())
		{
			Result->SetObjectField(TEXT("payload"), Last.Payload);
		}
	}
	else
	{
		Result->SetStringField(TEXT("hint"), TEXT("步骤还没跑完：进度会推送 gt.job 事件，也可以用 gt.job 查"));
	}

	return Result;
}

void UGameTestSubsystem::RunStepInline(const TSharedPtr<FJsonObject>& InStepJson, TSharedPtr<FJsonObject>& OutResult)
{
	checkf(IsInGameThread(), TEXT("GameTest 的入口必须在游戏线程上调用（UELink 的操作本来就在游戏线程执行）"));

	TArray<TSharedPtr<FJsonObject>> Steps;
	Steps.Add(InStepJson);

	FString RunId;
	bool bAlreadyFinished = false;
	if (!StartRunOnGameThread(Steps, TEXT("step"), /*InRevision=*/0, RunId, bAlreadyFinished, OutResult))
	{
		return;
	}

	OutResult = BuildOutcomeJson(bAlreadyFinished);
}

bool UGameTestSubsystem::BeginScenarioRun(
	const TArray<TSharedPtr<FJsonObject>>& InSteps,
	const FString& InName,
	int32 InRevision,
	FString& OutRunId,
	TSharedPtr<FJsonObject>& OutResult,
	int32 InStepDelayMs)
{
	checkf(IsInGameThread(), TEXT("GameTest 的入口必须在游戏线程上调用（UELink 的操作本来就在游戏线程执行）"));

	bool bAlreadyFinished = false;
	if (!StartRunOnGameThread(InSteps, InName, InRevision, OutRunId, bAlreadyFinished, OutResult, InStepDelayMs))
	{
		return false;
	}

	OutResult = BuildOutcomeJson(bAlreadyFinished);
	return true;
}

void UGameTestSubsystem::CancelCurrentRun()
{
	if (Runner.IsValid())
	{
		Runner->Cancel(TEXT("收到 stop 请求"));
	}
}

TSharedPtr<FJsonObject> UGameTestSubsystem::BuildHealthJson() const
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("ok"), true);
	Json->SetStringField(TEXT("plugin"), TEXT("GameTest"));
	Json->SetStringField(TEXT("process"), GIsEditor ? TEXT("editor") : TEXT("game"));
	Json->SetBoolField(TEXT("busy"), Runner.IsValid() && Runner->IsBusy());
	Json->SetStringField(TEXT("runId"), CurrentRunId);
	// 活性证据：AI 用它区分"编辑器真的卡死"与"只是这一步还没跑完"。
	Json->SetNumberField(TEXT("ticks"), static_cast<double>(TickCount));
	Json->SetNumberField(TEXT("frame"), static_cast<double>(GFrameCounter));
	Json->SetObjectField(TEXT("editor"), GameTestEditor::BuildStatusJson());

	UWorld* World = GameTestWorld::FindGameWorld();
	Json->SetBoolField(TEXT("worldPresent"), World != nullptr);
	Json->SetBoolField(TEXT("playable"), GameTestWorld::IsPlayable(World));
	if (World != nullptr)
	{
		Json->SetStringField(TEXT("map"), World->GetOutermost()->GetName());
	}

	// 可选模块的字段（如 EnhancedInput 的 activeInjections）。
	GameTestHooks::OnHealth().Broadcast(*Json);
	return Json;
}

TSharedPtr<FJsonObject> UGameTestSubsystem::BuildJobJson() const
{
	FScopeLock Lock(&JobLock);
	if (!JobSnapshot.IsValid())
	{
		const TSharedPtr<FJsonObject> Idle = MakeShared<FJsonObject>();
		Idle->SetStringField(TEXT("status"), TEXT("idle"));
		Idle->SetBoolField(TEXT("busy"), false);
		return Idle;
	}

	return JobSnapshot;
}

TSharedPtr<FJsonObject> UGameTestSubsystem::BuildActionsJson() const
{
	const TSharedPtr<FJsonObject> Json = GameTestActions::DescribeAll();
	Json->SetBoolField(TEXT("editorAvailable"), GameTestEditor::IsAvailable());
	return Json;
}

TSharedPtr<FJsonObject> UGameTestSubsystem::BuildScenariosJson() const
{
	const FString RootDir = UGameTestSettings::ResolveScenarioRootDir();
	// rootDir 给人看：在项目里就写相对项目的路径，在项目外（ProjectDoc）就写绝对路径
	FString RelativeRoot = RootDir;
	FPaths::MakePathRelativeTo(RelativeRoot, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	if (RelativeRoot.StartsWith(TEXT("..")))
	{
		RelativeRoot = RootDir;
	}

	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("rootDir"), RelativeRoot);
	Json->SetStringField(TEXT("absoluteRootDir"), RootDir);

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FGameTestScenarioInfo& Info : GameTestScenario::ScanDirectory(RootDir))
	{
		const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Info.Name);
		Item->SetStringField(TEXT("relativePath"), Info.RelativePath);
		Item->SetStringField(TEXT("_note"), Info.Note);
		Item->SetNumberField(TEXT("steps"), Info.StepCount);
		Item->SetNumberField(TEXT("revision"), Info.Revision);

		TArray<TSharedPtr<FJsonValue>> TagValues;
		TagValues.Reserve(Info.Tags.Num());
		for (const FString& Tag : Info.Tags)
		{
			TagValues.Add(MakeShared<FJsonValueString>(Tag));
		}
		Item->SetArrayField(TEXT("tags"), TagValues);

		if (!Info.ParseError.IsEmpty())
		{
			// 坏场景仍进清单：静默跳过会让"我明明写了这个文件"变成查不出来的问题。
			Item->SetStringField(TEXT("parseError"), Info.ParseError);
		}

		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	Json->SetArrayField(TEXT("scenarios"), Items);
	Json->SetNumberField(TEXT("total"), Items.Num());
	return Json;
}

TSharedPtr<FJsonObject> UGameTestSubsystem::BuildRunsJson() const
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const TSharedPtr<FJsonObject>& Summary : RunHistory.ListSummaries())
	{
		Items.Add(MakeShared<FJsonValueObject>(Summary));
	}

	Json->SetArrayField(TEXT("runs"), Items);
	Json->SetNumberField(TEXT("total"), Items.Num());
	if (Items.Num() == 0)
	{
		Json->SetStringField(TEXT("hint"), TEXT("本次会话还没有跑过任何 run（历史只保留内存里的最近若干次；产物目录下的 report.json 不受影响）"));
	}

	return Json;
}

TSharedPtr<FJsonObject> UGameTestSubsystem::BuildRunJson(const FString& InRunId) const
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	const TSharedPtr<FJsonObject> Report = RunHistory.Find(InRunId);
	if (!Report.IsValid())
	{
		Json->SetBoolField(TEXT("ok"), false);
		Json->SetStringField(TEXT("error"), FString::Printf(
			TEXT("历史里没有 runId '%s'（只有本次会话最近 %d 次；完整清单见 gt.runs）"), *InRunId, RunHistory.Num()));
		return Json;
	}

	Json->SetBoolField(TEXT("ok"), true);
	Json->SetObjectField(TEXT("report"), Report);
	return Json;
}

void UGameTestSubsystem::WriteReportFile(const FGameTestReport& InReport, const FString& InOutputDir) const
{
	if (InOutputDir.IsEmpty())
	{
		return;
	}

	const FString Path = FPaths::Combine(InOutputDir, TEXT("report.json"));
	IFileManager::Get().MakeDirectory(*InOutputDir, /*Tree=*/true);

	if (!FFileHelper::SaveStringToFile(
		GameTestJson::ToPrettyString(InReport.ToJson()), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		// 存不下证物不该改判被测结论，但必须留下痕迹。
		UE_LOG(LogGameTest, Warning, TEXT("报告落盘失败：%s（本次 run 的结论不受影响）"), *Path);
	}
}
