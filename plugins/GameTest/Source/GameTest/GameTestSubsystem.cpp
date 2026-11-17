// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameTestSubsystem.h"

#include "Core/GameTestAction.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestRunConfig.h"
#include "Core/GameTestScenario.h"
#include "Core/GameTestWorld.h"
#include "Editor/GameTestEditorApi.h"
#include "Core/GameTestHistory.h"
#include "Core/GameTestHooks.h"
#include "Input/GameTestInput.h"
#include "Operate/GameTestMove.h"
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

/**
 * 线框层画的数据。和覆盖层共享所有权：视口可能比子系统的持有者活得久
 * （PIE 停了、换了视口，没来得及摘掉），覆盖层再画时读的仍是活着的数据，不会读到已释放的持有者。
 */
struct FGameTestHighlightPaint
{
	/**
	 * 要画的框（**本层局部坐标**，单位是物理像素）。由执行器给的快照矩形减去窗口位置得到——
	 * 别在画的时候读控件几何：那条路上有视口 DPI 与窗口坐标两层换算，实测会把框画歪。
	 */
	bool bHasRect = false;
	FVector2D RectMin = FVector2D::ZeroVector;
	FVector2D RectMax = FVector2D::ZeroVector;

	/** 这一步成没成（决定框的颜色）。 */
	bool bSucceeded = true;
};

/** 高亮覆盖层的持有者：Slate 类型只出现在本翻译单元里，头文件不必把 Slate 拖给每个包含它的模块。 */
struct FGameTestHighlightHolder
{
	/** 加进视口的那一层：外层 SOverlay 负责铺满，内层只画。 */
	TSharedPtr<SWidget> Overlay;

	/** 覆盖层加在哪个视口上：摘的时候只从这个视口摘，视口换了（新的 PIE）不会摘错、也不会留下孤儿层。 */
	TWeakObjectPtr<UGameViewportClient> Viewport;

	/** 要画的数据，和覆盖层共享。 */
	TSharedRef<FGameTestHighlightPaint> Paint = MakeShared<FGameTestHighlightPaint>();

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
			SLATE_ARGUMENT(TSharedPtr<FGameTestHighlightPaint>, Paint)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			Paint = InArgs._Paint;
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

			const FGameTestHighlightPaint* Holder = Paint.Get();
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
		TSharedPtr<FGameTestHighlightPaint> Paint;
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
	LogTap.Attach();
	Evidence = MakeUnique<FGameTestEvidence>(LogTap);
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
	if (Evidence.IsValid())
	{
		// 还在等「之后」那半截的日志立刻写完，文件不留「还在收集」
		Evidence->FlushAll();
		Evidence.Reset();
	}
	LogTap.Detach();

	Super::Deinitialize();
}

void UGameTestSubsystem::LogFinishedSteps()
{
	const TArray<FGameTestStepRecord>& Steps = Runner->GetReport().Steps;
	// 开新 run 时在 StartRun 前清零；这里再兜一次：报告比记到的还短，说明换了一个 run。
	if (Steps.Num() < LastLoggedStepCount - 1)
	{
		LastLoggedStepCount = 0;
	}
	for (int32 Index = LastLoggedStepCount; Index < Steps.Num(); ++Index)
	{
		const FGameTestStepRecord& Record = Steps[Index];
		UE_LOG(LogGameTest, Display, TEXT("%s %s %s：%s"), *Record.Step.Label(), *Record.Step.Action.ToString(),
			GameTestStatus::ToString(Record.Status), *Record.Message.Left(300));
	}
	LastLoggedStepCount = FMath::Max(LastLoggedStepCount, Steps.Num());
}

bool UGameTestSubsystem::Tick(float InDeltaSeconds)
{
	// 子系统正在关（Deinitialize 先摘 ticker，但模块热重载等路径上顺序不保证）：没有执行器就什么都不做。
	if (!Runner.IsValid() || !Evidence.IsValid())
	{
		return true;
	}

	const double Now = FPlatformTime::Seconds();
	++TickCount;
	// 阻挡界面先处理再推进步骤：弹窗这一帧关掉，等着的点击下一次检查就能点到
	if (Runner->IsBusy() && Blockers.IsActive())
	{
		TArray<TSharedPtr<FJsonObject>> BlockerEvents;
		Blockers.Tick(Now, GameTestWorld::FindGameWorld(), Runner->GetStepInFlight(), BlockerEvents);
		for (const TSharedPtr<FJsonObject>& Event : BlockerEvents)
		{
			Runner->AddStepEvent(TEXT("blockers"), Event);
		}
	}
	Runner->Tick(Now);
	LogFinishedSteps();
	Evidence->Tick(Now);
	DriveBatch();
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
	// 批量运行时两个场景之间执行器是空的，但整件事还没完：busy 要连着批量算，面板才不会在两个场景之间显示「已结束」
	const FGameTestReport& Report = Runner->GetReport();
	const bool bBusy = Runner->IsBusy() || Batch.IsActive();
	if (Report.Steps.Num() != LastSnapshotStepCount || bBusy != LastSnapshotBusy || CurrentRunId != LastSnapshotRunId
		|| Batch.GetSerial() != LastSnapshotBatchSerial)
	{
		LastSnapshotStepCount = Report.Steps.Num();
		LastSnapshotBusy = bBusy;
		LastSnapshotRunId = CurrentRunId;
		LastSnapshotBatchSerial = Batch.GetSerial();

		const TSharedPtr<FJsonObject> Snapshot = Report.ToJson();
		Snapshot->SetBoolField(TEXT("busy"), bBusy);
		Snapshot->SetStringField(TEXT("runId"), CurrentRunId);
		// 一共几步（total 是已完成的步数）：面板画进度条要用。
		Snapshot->SetNumberField(TEXT("planned"), Runner->GetQueueLength());
		Snapshot->SetStringField(TEXT("status"), bBusy ? TEXT("running") : (Report.Steps.Num() > 0 ? TEXT("finished") : TEXT("idle")));
		if (Batch.HasData())
		{
			Snapshot->SetObjectField(TEXT("batch"), Batch.ToJson());
		}

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

	// 视口换了（PIE 停了又起）：旧层跟着旧视口走，这里重建，不往新视口上叠一个指着旧数据的层。
	if (Highlight.IsValid() && Highlight->Viewport.Get() != ViewportClient)
	{
		ClearHighlight();
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
					SNew(SGameTestHighlightOverlay).Paint(Highlight->Paint)
				];

			ViewportClient->AddViewportWidgetContent(Highlight->Overlay.ToSharedRef(), HighlightZOrder);
			Highlight->Viewport = ViewportClient;
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

			Highlight->Paint->RectMin = RectMin - WindowPosition;
			Highlight->Paint->RectMax = RectMax - WindowPosition;
		}
		Highlight->Paint->bHasRect = bHasRect;

		Highlight->AppliedSerial = Serial;
		Highlight->Paint->bSucceeded = Runner->WasTouchSuccessful();
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
		// 只从当初加上去的那个视口摘；它已经没了（PIE 停了）就不用摘，层跟着视口一起释放。
		// 画的数据和层共享所有权，层哪怕多活几帧也不会读到已释放的内存。
		if (UGameViewportClient* ViewportClient = Highlight->Viewport.Get())
		{
			ViewportClient->RemoveViewportWidgetContent(Highlight->Overlay.ToSharedRef());
		}
		// 先把画的数据清掉：万一层还挂在别处，也不再画框。
		Highlight->Paint->bHasRect = false;
	}

	Highlight.Reset();
}

bool UGameTestSubsystem::StartRunOnGameThread(
	const FGameTestScenarioRun& InRun,
	FString& OutRunId,
	bool& bOutAlreadyFinished,
	TSharedPtr<FJsonObject>& OutResult)
{
	bOutAlreadyFinished = false;
	OutRunId.Reset();
	const TArray<TSharedPtr<FJsonObject>>& InSteps = InRun.Steps;
	const FString& InName = InRun.Name;
	const int32 InRevision = InRun.Revision;
	const int32 InStepDelayMs = InRun.StepDelayMs;

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
	Request.Scenario = InRun.Scenario;
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

	// 项目运行配置（场景根的 _settings.json）：阻挡界面规则和 UI 等待上限。写错了整次不跑——
	// 规则没生效却照跑，弹窗挡住时只会看到一串莫名其妙的超时。
	FGameTestRunConfig RunConfig;
	FString ConfigError;
	if (!GameTestRunConfig::Load(RunConfig, ConfigError))
	{
		RestoreBackgroundThrottle();
		OutResult = MakeShared<FJsonObject>();
		OutResult->SetBoolField(TEXT("ok"), false);
		OutResult->SetStringField(TEXT("error"), ConfigError);
		return false;
	}
	// UI 等待上限：场景 > 运行配置 > 项目设置
	Request.UiWaitTimeoutMs = InRun.UiWaitTimeoutMs >= 0 ? InRun.UiWaitTimeoutMs
		: RunConfig.UiWaitTimeoutMs >= 0 ? RunConfig.UiWaitTimeoutMs
		: (GetDefault<UGameTestSettings>() != nullptr ? GetDefault<UGameTestSettings>()->UiWaitTimeoutMs : 0);
	// 点击方式：场景 > 运行配置 > 默认 code（不动鼠标）
	Request.ClickVia = !InRun.ClickVia.IsEmpty() ? InRun.ClickVia : !RunConfig.ClickVia.IsEmpty() ? RunConfig.ClickVia : FString(TEXT("code"));
	// move 步骤的默认设置：场景的 move 段盖在运行配置的 move 段上
	Request.MoveDefaults = GameTestMove::MergeDefaults(RunConfig.Move, InRun.Move);

	// 前置步骤（运行配置的 setup）：只在从场景文件跑时加在最前面，场景写了 "setup": false 的不加；
	// 「从这一步开始运行」和 Agent 直接给步骤的临时运行没有场景路径，也不加——它们接着当前状态跑。
	if (!InRun.Scenario.IsEmpty() && !InRun.bSkipSetup && RunConfig.Setup.Num() > 0)
	{
		TArray<FGameTestStep> SetupSteps;
		TArray<FString> SetupErrors;
		for (int32 Index = 0; Index < RunConfig.Setup.Num(); ++Index)
		{
			FGameTestStep Step;
			TArray<FString> StepErrors;
			if (GameTestActions::ParseStep(RunConfig.Setup[Index], Index, Step, StepErrors))
			{
				Step.bSetup = true;
				SetupSteps.Add(Step);
			}
			for (const FString& Error : StepErrors)
			{
				SetupErrors.Add(FString::Printf(TEXT("前置第 %d 步：%s"), Index + 1, *Error));
			}
		}
		if (SetupErrors.Num() > 0)
		{
			RestoreBackgroundThrottle();
			OutResult = MakeShared<FJsonObject>();
			OutResult->SetBoolField(TEXT("ok"), false);
			OutResult->SetStringField(TEXT("error"), FString::Printf(TEXT("运行配置 %s 的前置步骤写错了：%s"),
				*GameTestRunConfig::GetPath(), *FString::Join(SetupErrors, TEXT("；"))));
			return false;
		}
		Request.Steps.Insert(SetupSteps, 0);
	}

	// 固定引擎的全局随机（蓝图 Random 节点、FMath::Rand / FRand）：抽卡、掉落走它的场景每次一样。
	// 游戏自己的 FRandomStream、Lua 的 math.random 管不到，用前置步骤里的 GM 固定。
	if (RunConfig.RandomSeed.IsSet())
	{
		FMath::RandInit(RunConfig.RandomSeed.GetValue());
		FMath::SRandInit(RunConfig.RandomSeed.GetValue());
		UE_LOG(LogGameTest, Display, TEXT("run '%s' 固定随机种子 %d"), *RunName, RunConfig.RandomSeed.GetValue());
	}

	FString StartError;
	FGameTestRuntimeState RuntimeState;
	RuntimeState.Handles = &Handles;
	RuntimeState.RootedObjects = &RootedObjects;
	Evidence->BeginRun();
	const UGameTestSettings* EvidenceSettings = GetDefault<UGameTestSettings>();
	if (EvidenceSettings == nullptr || EvidenceSettings->bCaptureFailureEvidence)
	{
		RuntimeState.OnStepFailed = [this](FGameTestStepRecord& InOutRecord, const FString& InRunDir)
		{
			// 失败这一步先写进日志再留证据：证据里「失败前」的日志要看得到前面每一步和这一步为什么失败。
			// 这时记录还没进报告，记下它已经写过，后面按报告补日志时跳过它。
			if (Runner.IsValid())
			{
				LogFinishedSteps();
				UE_LOG(LogGameTest, Display, TEXT("%s %s %s：%s"), *InOutRecord.Step.Label(), *InOutRecord.Step.Action.ToString(),
					GameTestStatus::ToString(InOutRecord.Status), *InOutRecord.Message.Left(300));
				LastLoggedStepCount = Runner->GetReport().Steps.Num() + 1;
			}
			// 留证据只是附加信息：子系统正在关时没有 Evidence，跳过就好，不影响这一步的结论。
			if (Evidence.IsValid())
			{
				Evidence->Capture(InOutRecord, InRunDir, &Handles);
			}
		};
	}

	LastLoggedStepCount = 0;
	Blockers.Begin(RunConfig.Blockers);
	// 不用真实鼠标的 run 用虚拟光标：input.cursor、拖动、滚轮、鼠标键都不移系统光标
	GameTestInput::SetVirtualCursor(!Request.ClickVia.Equals(TEXT("pointer"), ESearchCase::IgnoreCase));
	const bool bStarted = Runner->StartRun(
		Request,
		RuntimeState,
		[this, OutputDir](const FGameTestReport& InReport)
		{
			RestoreBackgroundThrottle();
			Blockers.End();
			// 可选模块在这里清掉本次 run 留下的状态（EnhancedInput 模块停掉输入注入）。
			GameTestInput::ReleaseHeldKeys();
			GameTestHooks::OnRunFinished().Broadcast();
			WriteReportFile(InReport, OutputDir);
			// 历史里记下产物目录（相对项目根）：面板据此找截图和 report.json。
			const FString OutputRelative = GameTestShot::ToProjectRelative(OutputDir);
			const TSharedPtr<FJsonObject> Summary = InReport.ToJson();
			Summary->SetStringField(TEXT("outputDir"), OutputRelative);
			RunHistory.Add(Summary);
			// 按场景文件的历史（跨会话，列表上的历史条）和批量运行的记账
			GameTestHistory::Record(InReport, OutputRelative);
			Batch.OnRunFinished(InReport);
			LogFinishedSteps();
			UE_LOG(LogGameTest, Display, TEXT("run '%s' 结束：ok=%d，步骤 %d 条"),
				*InReport.Name, InReport.bSucceeded ? 1 : 0, InReport.Steps.Num());
		},
		StartError);

	if (!bStarted)
	{
		RestoreBackgroundThrottle();
		Blockers.End();

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

	if (RefuseDuringBatch(OutResult))
	{
		return;
	}

	FGameTestScenarioRun Run;
	Run.Steps.Add(InStepJson);
	Run.Name = TEXT("step");

	FString RunId;
	bool bAlreadyFinished = false;
	if (!StartRunOnGameThread(Run, RunId, bAlreadyFinished, OutResult))
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

	FGameTestScenarioRun Run;
	Run.Steps = InSteps;
	Run.Name = InName;
	Run.Revision = InRevision;
	Run.StepDelayMs = InStepDelayMs;
	return BeginScenarioFileRun(Run, OutRunId, OutResult);
}

bool UGameTestSubsystem::BeginScenarioFileRun(const FGameTestScenarioRun& InRun, FString& OutRunId, TSharedPtr<FJsonObject>& OutResult)
{
	checkf(IsInGameThread(), TEXT("GameTest 的入口必须在游戏线程上调用（UELink 的操作本来就在游戏线程执行）"));

	if (RefuseDuringBatch(OutResult))
	{
		return false;
	}

	bool bAlreadyFinished = false;
	if (!StartRunOnGameThread(InRun, OutRunId, bAlreadyFinished, OutResult))
	{
		return false;
	}

	OutResult = BuildOutcomeJson(bAlreadyFinished);
	return true;
}

bool UGameTestSubsystem::RefuseDuringBatch(TSharedPtr<FJsonObject>& OutResult)
{
	if (Batch.IsActive())
	{
		OutResult = MakeShared<FJsonObject>();
		OutResult->SetBoolField(TEXT("ok"), false);
		OutResult->SetStringField(TEXT("error"), TEXT("批量运行还在进行：等它跑完，或先 gt.stop 取消整批"));
		return true;
	}
	// 单独跑一个场景或一步：上一份批量的结果不再挂在作业快照上，免得面板把它当成这次的
	Batch.Clear();
	return false;
}

bool UGameTestSubsystem::BeginBatch(const TArray<FString>& InScenarios, const FString& InLabel, TSharedPtr<FJsonObject>& OutResult)
{
	checkf(IsInGameThread(), TEXT("GameTest 的入口必须在游戏线程上调用（UELink 的操作本来就在游戏线程执行）"));

	OutResult = MakeShared<FJsonObject>();
	if (Batch.IsActive() || Runner->IsBusy())
	{
		OutResult->SetBoolField(TEXT("ok"), false);
		OutResult->SetStringField(TEXT("error"), TEXT("已经有 run 或批量在跑：等它结束，或先 gt.stop"));
		return false;
	}
	if (InScenarios.Num() == 0)
	{
		OutResult->SetBoolField(TEXT("ok"), false);
		OutResult->SetStringField(TEXT("error"), TEXT("没有要跑的场景"));
		return false;
	}

	const FString BatchId = GameTestRun::MakeRunId(TEXT("batch"), RunCounter.Increment());
	Batch.Start(BatchId, InLabel, InScenarios);
	UE_LOG(LogGameTest, Display, TEXT("批量运行 %s（%s）：%d 个场景"), *BatchId, *InLabel, InScenarios.Num());

	// 立刻起第一个：和单个场景一样，第一步能同步做完的在这次调用里就有结果
	DriveBatch();

	OutResult->SetBoolField(TEXT("ok"), true);
	OutResult->SetStringField(TEXT("batchId"), BatchId);
	OutResult->SetObjectField(TEXT("batch"), Batch.ToJson());
	return true;
}

void UGameTestSubsystem::DriveBatch()
{
	if (!Batch.IsActive() || Runner->IsBusy())
	{
		return;
	}

	const int32 Index = Batch.TakeNext();
	if (Index == INDEX_NONE)
	{
		if (Batch.FinishIfDone())
		{
			UE_LOG(LogGameTest, Display, TEXT("批量运行结束：通过 %d，失败 %d"),
				Batch.Count(TEXT("passed")), Batch.Count(TEXT("failed")) + Batch.Count(TEXT("error")));
		}
		return;
	}

	// 一帧只起一个：前一个刚收尾，给 UMG 和 PIE 一帧喘气（上一个场景可能刚 pie.stop）
	FGameTestScenarioRun Run;
	FString Error;
	if (!GameTestScenario::LoadForRun(UGameTestSettings::ResolveScenarioRootDir(), Batch.GetItem(Index).Scenario, Run, Error))
	{
		Batch.MarkError(Index, Error);
		return;
	}

	FString RunId;
	bool bAlreadyFinished = false;
	TSharedPtr<FJsonObject> Result;
	if (!StartRunOnGameThread(Run, RunId, bAlreadyFinished, Result))
	{
		FString Reason;
		Batch.MarkError(Index, Result.IsValid() && Result->TryGetStringField(TEXT("error"), Reason) ? Reason : TEXT("run 没能开始"));
		return;
	}
	Batch.MarkStarted(Index, RunId, Run.Name);
}

void UGameTestSubsystem::CancelCurrentRun()
{
	// 先停批量再停当前 run：不然当前 run 一收尾，下一帧批量又起下一个
	Batch.Cancel();
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
