// Copyright Epic Games, Inc. All Rights Reserved.

#include "Record/GameTestRecorder.h"

#include "Core/GameTestHooks.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestRunConfig.h"
#include "Core/GameTestScenario.h"
#include "Core/GameTestWorld.h"
#include "Editor/GameTestEditorApi.h"
#include "Input/GameTestInput.h"
#include "Observe/GameTestItemKey.h"
#include "Observe/GameTestSelector.h"
#include "Observe/GameTestSelectorWriter.h"
#include "Observe/GameTestSlate.h"
#include "Observe/GameTestSnapshot.h"
#include "Observe/GameTestWidgetNode.h"
#include "Record/GameTestRecordMove.h"
#include "Record/GameTestRecordToolbar.h"
#include "Shot/GameTestShot.h"
#include "GameTest.h"
#include "GameTestSettings.h"

#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "InputCoreTypes.h"
#include "CoreGlobals.h"
#include "Layout/WidgetPath.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace GameTestRecorderDetail
{
	/** 两次操作隔这么久以上才补 wait（太短的间隔是人手的抖动，回放时一帧一步已经够了）。 */
	constexpr double MinGapSeconds = 0.15;

	/** 按下到松开短于这个就录成 tap。 */
	constexpr double TapSeconds = 0.25;

	/** 按下到松开挪了这么多像素以上算拖动（再小是手抖）。 */
	constexpr double DragThresholdPixels = 8.0;

	/** 连着滚轮间隔短于这个就合成一条。 */
	constexpr double WheelMergeSeconds = 0.5;

	/** 一次录制最多这么多步：再多的场景已经没法读，也说明录的时候忘了停。 */
	constexpr int32 MaxSteps = 2000;

	/** 队列上限：Tick 停了（窗口最小化之类）也不让它无限长。 */
	constexpr int32 MaxQueued = 512;

	TSharedRef<FJsonObject> MakeStep(const TCHAR* InAction)
	{
		const TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("action"), InAction);
		return Step;
	}

	/** 场景名 → 文件名：去掉文件名里不能用的字符，空白换成 `-`；中文名照留，文件名和场景名对得上。 */
	FString ToFileStem(const FString& InName)
	{
		FString Stem;
		for (const TCHAR Char : InName)
		{
			const bool bInvalid = FCString::Strchr(TEXT("\\/:*?\"<>|"), Char) != nullptr || Char < 32;
			Stem.AppendChar(bInvalid || FChar::IsWhitespace(Char) ? TEXT('-') : Char);
		}
		while (Stem.ReplaceInline(TEXT("--"), TEXT("-")) > 0)
		{
		}
		Stem.TrimCharInline(TEXT('-'), nullptr);
		Stem.TrimCharInline(TEXT('.'), nullptr);
		return Stem.IsEmpty() ? FString::Printf(TEXT("recording-%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"))) : Stem;
	}

	/** 控件内的点离中心不远就不写 at：回放默认点中心，步骤读起来干净。 */
	void SetAtIfOffCenter(FJsonObject& InStep, const FVector2D& InAt)
	{
		if (FMath::Abs(InAt.X - 0.5) <= 0.2 && FMath::Abs(InAt.Y - 0.5) <= 0.2)
		{
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Point;
		Point.Add(MakeShared<FJsonValueNumber>(InAt.X));
		Point.Add(MakeShared<FJsonValueNumber>(InAt.Y));
		InStep.SetArrayField(TEXT("at"), Point);
	}

	/** 光标下命中路径最深的控件（只读，不发事件）；不在游戏窗口或它的子窗口里时返回空。 */
	TSharedPtr<SWidget> LocateLeaf(const FVector2D& InScreenPosition)
	{
		FSlateApplication& App = FSlateApplication::Get();
		const FWidgetPath Path = App.LocateWindowUnderMouse(InScreenPosition, App.GetInteractiveTopLevelWindows());
		if (!Path.IsValid() || !GameTestSlate::IsInGameUi(Path.GetLastWidget(), Path.GetWindow()))
		{
			return nullptr;
		}
		return Path.GetLastWidget();
	}

	/** 键盘焦点是不是在游戏界面上：编辑器其他面板上的按键不录（PIE 在关卡视口里时游戏窗口就是编辑器主窗口，不能只看窗口）。 */
	bool IsKeyboardInGame()
	{
		return GameTestSlate::IsInGameUi(FSlateApplication::Get().GetUserFocusedWidget(0));
	}
}

/** Slate 输入预处理器：只看不吃（拾取模式的点击除外），事件照常往下走。 */
class FGameTestRecorderInput : public IInputProcessor
{
public:
	virtual void Tick(const float InDeltaTime, FSlateApplication& InSlateApp, TSharedRef<ICursor> InCursor) override
	{
		FGameTestRecorder::Get().OnTick();
	}

	virtual bool HandleKeyDownEvent(FSlateApplication& InSlateApp, const FKeyEvent& InKeyEvent) override
	{
		if (!InKeyEvent.IsRepeat() && !GameTestInput::IsSendingSynthetic())
		{
			// 录制快捷键 Alt+数字：吃掉，不传给游戏，也不录成按键。
			if (InKeyEvent.IsAltDown() && !InKeyEvent.IsControlDown() && FGameTestRecorder::Get().OnHotkey(InKeyEvent.GetKey()))
			{
				return true;
			}
			FGameTestRecorder::Get().OnKey(InKeyEvent.GetKey(), /*bInDown=*/true);
		}
		return false;
	}

	virtual bool HandleKeyUpEvent(FSlateApplication& InSlateApp, const FKeyEvent& InKeyEvent) override
	{
		if (!GameTestInput::IsSendingSynthetic())
		{
			FGameTestRecorder::Get().OnKey(InKeyEvent.GetKey(), /*bInDown=*/false);
		}
		return false;
	}

	virtual bool HandleMouseButtonDownEvent(FSlateApplication& InSlateApp, const FPointerEvent& InMouseEvent) override
	{
		if (GameTestInput::IsSendingSynthetic())
		{
			return false;
		}
		return FGameTestRecorder::Get().OnMouseButton(InMouseEvent.GetEffectingButton(), /*bInDown=*/true,
			FVector2D(InMouseEvent.GetScreenSpacePosition()), InMouseEvent.IsControlDown() && InMouseEvent.IsAltDown());
	}

	// 快速连点的第二下以双击事件送进来（没有单独的按下）：当成按下，否则连点「+」这类操作会少录一下。
	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& InSlateApp, const FPointerEvent& InMouseEvent) override
	{
		return HandleMouseButtonDownEvent(InSlateApp, InMouseEvent);
	}

	virtual bool HandleMouseButtonUpEvent(FSlateApplication& InSlateApp, const FPointerEvent& InMouseEvent) override
	{
		if (GameTestInput::IsSendingSynthetic())
		{
			return false;
		}
		return FGameTestRecorder::Get().OnMouseButton(InMouseEvent.GetEffectingButton(), /*bInDown=*/false,
			FVector2D(InMouseEvent.GetScreenSpacePosition()), InMouseEvent.IsControlDown() && InMouseEvent.IsAltDown());
	}

	virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& InSlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent) override
	{
		if (!GameTestInput::IsSendingSynthetic() && InGestureEvent == nullptr)
		{
			FGameTestRecorder::Get().OnWheel(InWheelEvent.GetWheelDelta(), FVector2D(InWheelEvent.GetScreenSpacePosition()));
		}
		return false;
	}

	virtual const TCHAR* GetDebugName() const override
	{
		return TEXT("GameTestRecorder");
	}
};

FGameTestRecorder::FGameTestRecorder() = default;

FGameTestRecorder::~FGameTestRecorder() = default;

FGameTestRecorder& FGameTestRecorder::Get()
{
	static FGameTestRecorder Instance;
	return Instance;
}

bool FGameTestRecorder::Start(const FString& InName, bool bInStartPie, bool bInKeepPace, FString& OutError)
{
	if (bRecording)
	{
		OutError = FString::Printf(TEXT("已经在录「%s」了：先 gt.record.stop"), *Name);
		return false;
	}
	if (!FSlateApplication::IsInitialized())
	{
		OutError = TEXT("这个进程没有 Slate（命令行或 -nullrhi），录不了操作");
		return false;
	}

	Name = InName.TrimStartAndEnd().IsEmpty() ? FString::Printf(TEXT("录制 %s"), *FDateTime::Now().ToString(TEXT("%m-%d %H:%M"))) : InName.TrimStartAndEnd();
	Steps.Reset();
	Queue.Reset();
	UiPress.Reset();
	Typing.Reset();
	HeldKeys.Reset();
	bMouseDownOnGame = false;
	bPickActive = false;
	bTruncated = false;
	bKeepPace = bInKeepPace;
	StartedAt = FPlatformTime::Seconds();
	LastEventAt = 0.0;
	MoveSegment.Reset();
	bPickArmed = false;
	bToolbarPressed = false;
	Notice.Reset();
	Shots.Begin(NextShotOptions);
	NextShotOptions = FGameTestShotOptions();
	KeySteps.Reset();
	KeyAsk.Reset();
	KeyChoices.Reset();
	KeyCacheDir = GameTestShot::GetOutputRootDir() / TEXT("keys") / FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
	KeyCacheCounter = 0;

	// 运行设置 move.hotkeys=false 时不认 Alt+数字（项目自己用这些键）；配置读不了不挡录制，按默认开。
	bHotkeys = true;
	FGameTestRunConfig RunConfig;
	FString ConfigError;
	if (GameTestRunConfig::Load(RunConfig, ConfigError) && RunConfig.Move.IsValid())
	{
		RunConfig.Move->TryGetBoolField(TEXT("hotkeys"), bHotkeys);
	}

	// 编辑器里 PIE 正在跑：回放时先把同一张地图的 PIE 拉起来，场景才能单独跑。
	if (bInStartPie && GameTestEditor::IsAvailable() && GameTestEditor::IsPieRunning())
	{
		const TSharedRef<FJsonObject> Pie = GameTestRecorderDetail::MakeStep(TEXT("pie.start"));
		const FString Map = GameTestEditor::GetEditorLevelPath();
		if (!Map.IsEmpty())
		{
			Pie->SetStringField(TEXT("map"), Map);
		}
		Pie->SetStringField(TEXT("note"), TEXT("录制时 PIE 已经在跑：回放先启动同一张地图"));
		Steps.Add(Pie);
	}

	HandledHandle = GameTestHandlerWatch::OnHandled().AddRaw(this, &FGameTestRecorder::OnHandled);
	InputProcessor = MakeShared<FGameTestRecorderInput>();
	FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
	bRecording = true;
	SyncToolbar();

	UE_LOG(LogGameTest, Display, TEXT("开始录制「%s」（处理者来源：%s）"), *Name,
		GameTestHandlerWatch::IsSupported() ? TEXT("Slate 调试事件") : TEXT("命中路径推断"));
	Broadcast(TEXT("started"));
	return true;
}

bool FGameTestRecorder::Mark(const FString& InText, bool bInScreenshot, FString& OutError)
{
	if (!bRecording)
	{
		OutError = TEXT("没有在录制：先 gt.record.start");
		return false;
	}

	OnTick();
	FlushTyping();
	AddGapWait(FPlatformTime::Seconds(), /*bInUiStep=*/false);

	const TSharedRef<FJsonObject> Step = GameTestRecorderDetail::MakeStep(TEXT("mark"));
	Step->SetStringField(TEXT("text"), InText.IsEmpty() ? TEXT("检查点") : InText);
	AddStep(Step);

	if (bInScreenshot)
	{
		const TSharedRef<FJsonObject> Shot = GameTestRecorderDetail::MakeStep(TEXT("screenshot"));
		Shot->SetStringField(TEXT("name"), FString::Printf(TEXT("mark-%d"), Steps.Num()));
		AddStep(Shot);
	}
	return true;
}

bool FGameTestRecorder::Stop(bool bInSave, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
{
	if (!bRecording)
	{
		OutError = TEXT("没有在录制");
		return false;
	}

	// 还在等人挑标识：这一步保持「待定」，游戏恢复
	EndAsk();
	OnTick();
	// 还在录移动就按「结束」处理：停止前走的那段路不丢。
	if (MoveSegment.IsValid())
	{
		FString MoveError;
		EndMove(/*bInAuto=*/true, MoveError);
	}
	HideToolbar();
	FlushTyping();

	// 录制结束时还按着的键补一条松开：回放时不留卡住的键（打字顺带的键回放时本来就没按）。
	for (const TPair<FName, FHeldKey>& Pair : HeldKeys)
	{
		if (Pair.Value.bTyping)
		{
			continue;
		}
		const TSharedRef<FJsonObject> Release = GameTestRecorderDetail::MakeStep(TEXT("key"));
		Release->SetStringField(TEXT("key"), Pair.Key.ToString());
		Release->SetStringField(TEXT("phase"), TEXT("release"));
		if (!Pair.Value.Target.IsEmpty())
		{
			Release->SetStringField(TEXT("target"), Pair.Value.Target);
		}
		AddStep(Release);
	}
	HeldKeys.Reset();

	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();
	GameTestHandlerWatch::OnHandled().Remove(HandledHandle);
	HandledHandle.Reset();
	bRecording = false;
	Shots.End();

	OutResult = MakeShared<FJsonObject>();
	OutResult->SetStringField(TEXT("name"), Name);
	OutResult->SetNumberField(TEXT("steps"), Steps.Num());
	OutResult->SetNumberField(TEXT("seconds"), FMath::RoundToInt((FPlatformTime::Seconds() - StartedAt) * 10.0) / 10.0);
	if (bTruncated)
	{
		OutResult->SetBoolField(TEXT("truncated"), true);
	}

	TArray<TSharedPtr<FJsonValue>> StepValues;
	for (const TSharedRef<FJsonObject>& Step : Steps)
	{
		StepValues.Add(MakeShared<FJsonValueObject>(Step));
	}

	if (bInSave && Steps.Num() > 0)
	{
		const TSharedRef<FJsonObject> Scenario = MakeShared<FJsonObject>();
		Scenario->SetStringField(TEXT("kind"), GameTestScenario::GetKind());
		Scenario->SetNumberField(TEXT("schemaVersion"), GameTestScenario::SupportedSchemaVersion);
		Scenario->SetStringField(TEXT("name"), Name);
		Scenario->SetStringField(TEXT("_note"), FString::Printf(
			TEXT("录制于 %s。mark 是录制时插的检查点，可以在 Agent 面板里改成 assert。"), *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M"))));
		Scenario->SetNumberField(TEXT("revision"), 1);
		TArray<TSharedPtr<FJsonValue>> Tags;
		Tags.Add(MakeShared<FJsonValueString>(TEXT("recorded")));
		Scenario->SetArrayField(TEXT("tags"), Tags);
		// 录下来的节奏靠 wait 步骤和 UI 动作的自动等待还原，步间不再额外停顿。
		Scenario->SetNumberField(TEXT("stepDelayMs"), 0);
		Scenario->SetArrayField(TEXT("steps"), StepValues);

		const FString RootDir = UGameTestSettings::ResolveScenarioRootDir();
		const FString Stem = GameTestRecorderDetail::ToFileStem(Name);
		FString Relative = FString::Printf(TEXT("recorded/%s.json"), *Stem);
		for (int32 Suffix = 2; IFileManager::Get().FileExists(*(RootDir / Relative)); ++Suffix)
		{
			Relative = FString::Printf(TEXT("recorded/%s-%d.json"), *Stem, Suffix);
		}

		const FString AbsolutePath = RootDir / Relative;
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), /*Tree=*/true);
		if (!FFileHelper::SaveStringToFile(GameTestJson::ToPrettyString(Scenario), *AbsolutePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("录制已停止，但场景文件写不进去：%s（目录只读或被占用？）"), *AbsolutePath);
			Broadcast(TEXT("stopped"));
			return false;
		}

		OutResult->SetStringField(TEXT("path"), Relative);
		OutResult->SetStringField(TEXT("absolutePath"), AbsolutePath);
	}

	// 只回前几步预览：完整内容在文件里，返回体不撑大。
	TArray<TSharedPtr<FJsonValue>> Preview;
	for (int32 Index = 0; Index < FMath::Min(StepValues.Num(), 20); ++Index)
	{
		Preview.Add(StepValues[Index]);
	}
	OutResult->SetArrayField(TEXT("preview"), Preview);

	UE_LOG(LogGameTest, Display, TEXT("录制「%s」结束：%d 步"), *Name, Steps.Num());
	Broadcast(TEXT("stopped"));
	return true;
}

TSharedRef<FJsonObject> FGameTestRecorder::DescribeStatus(int32 InLastSteps) const
{
	const TSharedRef<FJsonObject> Status = MakeShared<FJsonObject>();
	Status->SetBoolField(TEXT("recording"), bRecording);
	// 处理者从哪来：slateDebugging（准）/ hitPath（这个进程没编 Slate 调试，按命中路径推断，打字录不到）。
	Status->SetStringField(TEXT("handlerSource"), GameTestHandlerWatch::IsSupported() ? TEXT("slateDebugging") : TEXT("hitPath"));
	if (!bRecording)
	{
		return Status;
	}

	Status->SetStringField(TEXT("name"), Name);
	Status->SetNumberField(TEXT("steps"), Steps.Num());
	Status->SetNumberField(TEXT("seconds"), FMath::RoundToInt((FPlatformTime::Seconds() - StartedAt) * 10.0) / 10.0);
	Status->SetBoolField(TEXT("keepPace"), bKeepPace);
	if (bTruncated)
	{
		Status->SetBoolField(TEXT("truncated"), true);
	}
	DescribeQuick(*Status);
	DescribeAsk(*Status);

	TArray<TSharedPtr<FJsonValue>> Last;
	for (int32 Index = FMath::Max(0, Steps.Num() - InLastSteps); Index < Steps.Num(); ++Index)
	{
		Last.Add(MakeShared<FJsonValueObject>(Steps[Index]));
	}
	Status->SetArrayField(TEXT("last"), Last);
	return Status;
}

void FGameTestRecorder::Shutdown()
{
	EndAsk();
	KeySteps.Reset();
	KeyChoices.Reset();
	HideToolbar();
	MoveSegment.Reset();
	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();
	GameTestHandlerWatch::OnHandled().Remove(HandledHandle);
	HandledHandle.Reset();
	bRecording = false;
	Shots.End();
	Steps.Reset();
	Queue.Reset();
	UiPress.Reset();
	Typing.Reset();
}

//~ 原始事件入队 -------------------------------------------------------------------

bool FGameTestRecorder::OnMouseButton(const FKey& InButton, bool bInDown, const FVector2D& InScreenPosition, bool bInPickModifiers)
{
	if (!bRecording || Queue.Num() >= GameTestRecorderDetail::MaxQueued)
	{
		return false;
	}

	// 拾取模式：按住 Ctrl+Alt 点下去的这一下（和它的松开）吃掉，不传给游戏，改录断言。
	if (!bInDown && bPickActive)
	{
		bPickActive = false;
		return true;
	}

	// 点的是录制工具栏：交给工具栏自己的按钮，按下和配对的松开都不录（拖动时松开可能已经不在工具栏上）。
	if (bInDown && IsOnToolbar(InScreenPosition))
	{
		bToolbarPressed = true;
		return false;
	}
	if (!bInDown && bToolbarPressed)
	{
		bToolbarPressed = false;
		return false;
	}
	// Alt+4 / 工具栏「断言」之后的下一次点击：和 Ctrl+Alt 点一样录成断言。
	if (bInDown && bPickArmed)
	{
		bPickArmed = false;
		bInPickModifiers = true;
	}

	const TSharedPtr<SWidget> Leaf = GameTestRecorderDetail::LocateLeaf(InScreenPosition);
	if (!Leaf.IsValid() && bInDown)
	{
		// 不在游戏窗口里：编辑器其他地方的点击不录。
		return false;
	}

	FRawEvent Event;
	Event.Key = InButton.GetFName();
	Event.Position = InScreenPosition;
	Event.Time = FPlatformTime::Seconds();
	Event.Frame = GFrameCounter;
	Event.HitLeaf = Leaf;

	if (bInDown && bInPickModifiers)
	{
		Event.Kind = ERawKind::PickDown;
		Queue.Add(Event);
		bPickActive = true;
		return true;
	}

	Event.Kind = bInDown ? ERawKind::MouseDown : ERawKind::MouseUp;
	Queue.Add(Event);
	return false;
}

void FGameTestRecorder::OnKey(const FKey& InKey, bool bInDown)
{
	if (!bRecording || InKey.IsMouseButton() || Queue.Num() >= GameTestRecorderDetail::MaxQueued)
	{
		return;
	}
	// 只录发给游戏的按键：键盘焦点在编辑器别的面板上时不算。松开不判断窗口：按下时录了，松开就要配上。
	if (bInDown && !GameTestRecorderDetail::IsKeyboardInGame())
	{
		return;
	}

	FRawEvent Event;
	Event.Kind = bInDown ? ERawKind::KeyDown : ERawKind::KeyUp;
	Event.Key = InKey.GetFName();
	Event.Time = FPlatformTime::Seconds();
	Event.Frame = GFrameCounter;
	Queue.Add(Event);
}

void FGameTestRecorder::OnWheel(float InDelta, const FVector2D& InScreenPosition)
{
	if (!bRecording || Queue.Num() >= GameTestRecorderDetail::MaxQueued)
	{
		return;
	}
	// 在录制工具栏上滚轮：不录。
	if (IsOnToolbar(InScreenPosition))
	{
		return;
	}
	const TSharedPtr<SWidget> Leaf = GameTestRecorderDetail::LocateLeaf(InScreenPosition);
	if (!Leaf.IsValid())
	{
		return;
	}

	FRawEvent Event;
	Event.Kind = ERawKind::Wheel;
	Event.Position = InScreenPosition;
	Event.Delta = InDelta;
	Event.Time = FPlatformTime::Seconds();
	Event.Frame = GFrameCounter;
	Event.HitLeaf = Leaf;
	Queue.Add(Event);
}

void FGameTestRecorder::OnHandled(GameTestHandlerWatch::EKind InKind, const GameTestHandlerWatch::FHandled& InRecord)
{
	if (!bRecording || GameTestInput::IsSendingSynthetic())
	{
		return;
	}

	// 字符事件没有经过输入处理器（处理器看不到 KeyChar）：在这里直接入队，处理者一并带上。
	if (InKind == GameTestHandlerWatch::EKind::KeyChar)
	{
		// 游戏视口接住的字符（控制台、项目的 InputChar）不是往输入框里打字：不入队，那个键照常录成游戏按键。
		const bool bViewport = InRecord.Handler.Pin() == GameTestSlate::GetGameViewportWidget();
		if (InRecord.Character >= 32 && !bViewport && Queue.Num() < GameTestRecorderDetail::MaxQueued && GameTestRecorderDetail::IsKeyboardInGame())
		{
			FRawEvent Event;
			Event.Kind = ERawKind::Char;
			Event.Character = InRecord.Character;
			Event.Time = FPlatformTime::Seconds();
			Event.Frame = GFrameCounter;
			Event.Handler = InRecord.Handler;
			Event.bHasHandler = true;
			Queue.Add(Event);
		}
		return;
	}

	ERawKind Kind;
	switch (InKind)
	{
	case GameTestHandlerWatch::EKind::MouseDown:
		Kind = ERawKind::MouseDown;
		break;
	case GameTestHandlerWatch::EKind::MouseUp:
		Kind = ERawKind::MouseUp;
		break;
	case GameTestHandlerWatch::EKind::MouseWheel:
		Kind = ERawKind::Wheel;
		break;
	case GameTestHandlerWatch::EKind::KeyDown:
		Kind = ERawKind::KeyDown;
		break;
	case GameTestHandlerWatch::EKind::KeyUp:
		Kind = ERawKind::KeyUp;
		break;
	default:
		return;
	}

	// 处理者是在路由时才知道的：补给最近一个同类、还没有处理者的事件（就是刚入队的那个）。
	// 只认同一帧入队、按键还要同一个键：长按的重复键、编辑器别处没入队的点击，处理者不会错配到更早的事件上。
	const bool bKeyboard = Kind == ERawKind::KeyDown || Kind == ERawKind::KeyUp;
	for (int32 Index = Queue.Num() - 1; Index >= 0; --Index)
	{
		FRawEvent& Event = Queue[Index];
		if (Event.Kind != Kind || Event.bHasHandler)
		{
			continue;
		}
		if (Event.Frame == GFrameCounter && (!bKeyboard || Event.Key == InRecord.Key))
		{
			Event.Handler = InRecord.Handler;
			Event.bHasHandler = true;
		}
		return;
	}
}

void FGameTestRecorder::OnTick()
{
	TickMove();
	if (bRecording)
	{
		Shots.Tick(FPlatformTime::Seconds());
	}
	// 暂停问「这一格怎么认」期间不录任何输入（人在面板里挑字段，游戏窗口里顺手点的不算）
	if (KeyAsk.IsSet())
	{
		TickAsk();
		Queue.Reset();
		return;
	}
	if (!bRecording || Queue.Num() == 0)
	{
		return;
	}

	// 先把队列拿出来再处理：处理时写选择器会做命中测试和解析，期间不该有新事件插进同一个数组。
	const TArray<FRawEvent> Pending = MoveTemp(Queue);
	Queue.Reset();
	for (int32 Index = 0; Index < Pending.Num(); ++Index)
	{
		// 刚停下来问「这一格怎么认」：同一批里后面的事件是暂停之后的，不录
		if (KeyAsk.IsSet())
		{
			break;
		}
		ProcessEvent(Pending[Index], Pending.IsValidIndex(Index + 1) ? &Pending[Index + 1] : nullptr);
	}
}

//~ 事件 → 步骤 -------------------------------------------------------------------

void FGameTestRecorder::ProcessEvent(const FRawEvent& InEvent, const FRawEvent* InNext)
{
	// 录移动段里：只记轨迹，别的操作不写步骤（点了界面会先自动结束移动段，这一下照常录）。
	if (MoveSegment.IsValid() && FilterDuringMove(InEvent))
	{
		return;
	}
	switch (InEvent.Kind)
	{
	case ERawKind::MouseDown:
		ProcessMouseDown(InEvent);
		break;
	case ERawKind::MouseUp:
		ProcessMouseUp(InEvent);
		break;
	case ERawKind::Wheel:
		ProcessWheel(InEvent);
		break;
	case ERawKind::KeyDown:
		// 打字：按下字母键后紧跟着一个被 UI 接住的字符事件，这个键只是打字顺带的，不单独录（字录进 type）。
		ProcessKeyDown(InEvent, InNext != nullptr && InNext->Kind == ERawKind::Char);
		break;
	case ERawKind::KeyUp:
		ProcessKeyUp(InEvent);
		break;
	case ERawKind::Char:
		ProcessChar(InEvent);
		break;
	case ERawKind::PickDown:
		ProcessPick(InEvent);
		break;
	}
}

bool FGameTestRecorder::DescribeHandler(
	const FRawEvent& InEvent,
	bool& OutIsUi,
	FString& OutTarget,
	TSharedPtr<FJsonObject>& OutHint,
	TSharedPtr<SWidget>& OutSlate,
	TSharedPtr<FGameTestKeySite>* OutKeySite) const
{
	OutIsUi = false;
	OutTarget.Reset();
	OutHint.Reset();
	OutSlate.Reset();

	// 处理者：有 Slate 调试时用它（准）；没有时退回命中路径最深的控件（推断）。
	TSharedPtr<SWidget> Handler = InEvent.Handler.Pin();
	if (!Handler.IsValid() && !GameTestHandlerWatch::IsSupported())
	{
		Handler = InEvent.HitLeaf.Pin();
	}
	const TSharedPtr<SWidget> Viewport = GameTestSlate::GetGameViewportWidget();
	if (!Handler.IsValid() || Handler == Viewport)
	{
		// 按下没人接（或游戏视口接了），但点的位置上有游戏界面的控件：多半是只在松开时响应的控件
		// （列表条目、自定义 WBP 按钮）。按命中的那个控件录成 UI 点击，不录成游戏画面上的原始鼠标——
		// 原始鼠标回放要摆光标，还认不出点的是谁。
		const TSharedPtr<SWidget> Leaf = InEvent.HitLeaf.Pin();
		if (Leaf.IsValid() && Leaf != Viewport && GameTestSlate::IsInGameUi(Leaf) && GameTestSlate::FindOwningWidget(Leaf) != nullptr)
		{
			Handler = Leaf;
		}
		else
		{
			// 真点在游戏画面上
			return true;
		}
	}

	FString Error;
	if (UWidget* Widget = GameTestSlate::FindOwningWidget(Handler))
	{
		// 写不出选择器时往外找几层：外层的按钮、条目往往写得出来，点它和点里面的图标效果一样；
		// 实在写不出才录成游戏画面上的操作（那会要摆光标）。
		constexpr int32 MaxClimb = 4;
		UWidget* Current = Widget;
		for (int32 Climb = 0; Current != nullptr && Climb <= MaxClimb; ++Climb)
		{
			FGameTestWrittenTarget Written;
			if (GameTestSelectorWriter::Write(*Current, Written, Error))
			{
				if (Current != Widget)
				{
					UE_LOG(LogGameTest, Display, TEXT("录制：%s 写不出选择器，改录外层的 %s"), *GameTestSlate::Describe(Handler), *Written.Target);
				}
				OutIsUi = true;
				OutTarget = Written.Target;
				OutHint = Written.Hint;
				OutSlate = Current->GetCachedWidget();
				if (OutKeySite != nullptr)
				{
					*OutKeySite = Written.KeySite;
				}
				return true;
			}
			const TSharedPtr<SWidget> CurrentSlate = Current->GetCachedWidget();
			Current = CurrentSlate.IsValid() ? GameTestSlate::FindOwningWidget(CurrentSlate->GetParentWidget()) : nullptr;
		}
		UE_LOG(LogGameTest, Warning, TEXT("录制：%s 写不出选择器（%s），按游戏画面上的操作录"), *GameTestSlate::Describe(Handler), *Error);
		return true;
	}

	// 纯 Slate 控件（下拉项、菜单行）：只要在游戏视口或游戏窗口弹出的子窗口里，就按 Slate 段录；编辑器自己的界面不录。
	if (!GameTestSlate::IsInGameUi(Handler))
	{
		return false;
	}
	FGameTestWrittenTarget Written;
	if (!GameTestSelectorWriter::WriteSlate(Handler.ToSharedRef(), Written, Error))
	{
		return true;
	}
	OutIsUi = true;
	OutTarget = Written.Target;
	OutHint = Written.Hint;
	OutSlate = Handler;
	return true;
}

void FGameTestRecorder::ProcessMouseDown(const FRawEvent& InEvent)
{
	FlushTyping();

	bool bIsUi = false;
	FString Target;
	TSharedPtr<FJsonObject> Hint;
	TSharedPtr<SWidget> Slate;
	TSharedPtr<FGameTestKeySite> KeySite;
	if (!DescribeHandler(InEvent, bIsUi, Target, Hint, Slate, &KeySite))
	{
		return;
	}

	if (bIsUi)
	{
		// 先不出步骤：松开时才知道是点击还是拖动。
		FUiPress Press;
		Press.Button = InEvent.Key;
		Press.StartPosition = InEvent.Position;
		Press.Target = Target;
		Press.Hint = Hint;
		Press.KeySite = KeySite;
		if (Slate.IsValid())
		{
			GameTestSelectorWriter::ToLocalPoint(*Slate, InEvent.Position, Press.At);
		}
		// 按下这一刻截图（按钮默认松开才触发，图里是点之前的界面）：写进 hint.shot / screen，点完打开的界面之后补 opened
		Shots.OnUiPress(Slate, InEvent.Position, Hint);
		UiPress = Press;
		return;
	}

	// 点在游戏画面上：摆好光标再按真实的鼠标键。
	FVector2D Point;
	if (!ToViewportPoint(InEvent.Position, Point))
	{
		return;
	}
	AddGapWait(InEvent.Time, /*bInUiStep=*/false);
	const TSharedRef<FJsonObject> Cursor = GameTestRecorderDetail::MakeStep(TEXT("input.cursor"));
	Cursor->SetNumberField(TEXT("x"), Point.X);
	Cursor->SetNumberField(TEXT("y"), Point.Y);
	AddStep(Cursor);
	const TSharedRef<FJsonObject> Press = GameTestRecorderDetail::MakeStep(TEXT("key"));
	Press->SetStringField(TEXT("key"), InEvent.Key.ToString());
	Press->SetStringField(TEXT("phase"), TEXT("press"));
	// 点在游戏画面上也截一块（光标周围），AI 整理时看得出点的是什么
	if (Shots.IsEnabled())
	{
		const TSharedPtr<FJsonObject> ShotHint = MakeShared<FJsonObject>();
		Shots.OnGamePress(InEvent.Position, ShotHint);
		if (ShotHint->Values.Num() > 0)
		{
			Press->SetObjectField(TEXT("hint"), ShotHint);
		}
	}
	AddStep(Press);
	FHeldKey Held;
	Held.Time = InEvent.Time;
	HeldKeys.Add(InEvent.Key, Held);
	bMouseDownOnGame = true;
}

void FGameTestRecorder::ProcessMouseUp(const FRawEvent& InEvent)
{
	if (UiPress.IsSet() && UiPress->Button == InEvent.Key)
	{
		const FUiPress Press = UiPress.GetValue();
		UiPress.Reset();
		AddGapWait(InEvent.Time, /*bInUiStep=*/true);

		const bool bMoved = FVector2D::Distance(Press.StartPosition, InEvent.Position) > GameTestRecorderDetail::DragThresholdPixels;
		if (!bMoved)
		{
			const TSharedRef<FJsonObject> Click = GameTestRecorderDetail::MakeStep(TEXT("click"));
			Click->SetStringField(TEXT("target"), Press.Target);
			GameTestRecorderDetail::SetAtIfOffCenter(*Click, Press.At);
			if (Press.Button != EKeys::LeftMouseButton.GetFName())
			{
				Click->SetStringField(TEXT("button"), Press.Button.ToString());
			}
			if (Press.Hint.IsValid())
			{
				Click->SetObjectField(TEXT("hint"), Press.Hint);
			}
			AddStep(Click);
			AfterKeyStep(Click, Press.KeySite, /*bInMayAsk=*/true);
			return;
		}

		// 拖动：终点落在某个 UMG 控件上就录 toTarget，否则录按视口归一化的位移。
		const TSharedRef<FJsonObject> Drag = GameTestRecorderDetail::MakeStep(TEXT("drag"));
		Drag->SetStringField(TEXT("target"), Press.Target);
		GameTestRecorderDetail::SetAtIfOffCenter(*Drag, Press.At);
		if (Press.Button != EKeys::LeftMouseButton.GetFName())
		{
			Drag->SetStringField(TEXT("button"), Press.Button.ToString());
		}

		bool bWroteEnd = false;
		const TSharedPtr<SWidget> EndLeaf = GameTestRecorderDetail::LocateLeaf(InEvent.Position);
		if (UWidget* EndWidget = GameTestSlate::FindOwningWidget(EndLeaf))
		{
			FGameTestWrittenTarget EndTarget;
			FString Error;
			if (GameTestSelectorWriter::Write(*EndWidget, EndTarget, Error) && EndTarget.bVerified && EndTarget.Target != Press.Target)
			{
				Drag->SetStringField(TEXT("toTarget"), EndTarget.Target);
				FVector2D ToAt(0.5, 0.5);
				if (const TSharedPtr<SWidget> EndSlate = EndWidget->GetCachedWidget())
				{
					GameTestSelectorWriter::ToLocalPoint(*EndSlate, InEvent.Position, ToAt);
				}
				TArray<TSharedPtr<FJsonValue>> Point;
				Point.Add(MakeShared<FJsonValueNumber>(ToAt.X));
				Point.Add(MakeShared<FJsonValueNumber>(ToAt.Y));
				Drag->SetArrayField(TEXT("toAt"), Point);
				bWroteEnd = true;
			}
		}
		if (!bWroteEnd)
		{
			const TSharedPtr<SWidget> Viewport = GameTestSlate::GetGameViewportWidget();
			const FVector2D ViewSize = Viewport.IsValid() ? FVector2D(Viewport->GetCachedGeometry().GetAbsoluteSize()) : FVector2D::ZeroVector;
			const FVector2D Offset = InEvent.Position - Press.StartPosition;
			TArray<TSharedPtr<FJsonValue>> By;
			By.Add(MakeShared<FJsonValueNumber>(ViewSize.X >= 1.0 ? FMath::RoundToDouble(Offset.X / ViewSize.X * 1000.0) / 1000.0 : 0.0));
			By.Add(MakeShared<FJsonValueNumber>(ViewSize.Y >= 1.0 ? FMath::RoundToDouble(Offset.Y / ViewSize.Y * 1000.0) / 1000.0 : 0.0));
			Drag->SetArrayField(TEXT("by"), By);
		}
		if (Press.Hint.IsValid())
		{
			Drag->SetObjectField(TEXT("hint"), Press.Hint);
		}
		AddStep(Drag);
		// 拖动不停下来问（拖到一半停游戏没意义），标识照样复查、记下，录完可以在步骤详情里改
		AfterKeyStep(Drag, Press.KeySite, /*bInMayAsk=*/false);
		return;
	}

	if (!bMouseDownOnGame)
	{
		return;
	}

	bMouseDownOnGame = false;
	const FHeldKey* Held = HeldKeys.Find(InEvent.Key);
	const bool bQuick = Held != nullptr && InEvent.Time - Held->Time < GameTestRecorderDetail::TapSeconds;
	HeldKeys.Remove(InEvent.Key);
	if (bQuick && Steps.Num() > 0)
	{
		FString LastKey;
		Steps.Last()->TryGetStringField(TEXT("key"), LastKey);
		if (LastKey == InEvent.Key.ToString())
		{
			Steps.Last()->SetStringField(TEXT("phase"), TEXT("tap"));
			LastEventAt = InEvent.Time;
			Broadcast(TEXT("step"));
			return;
		}
	}
	AddGapWait(InEvent.Time, /*bInUiStep=*/false);
	const TSharedRef<FJsonObject> Release = GameTestRecorderDetail::MakeStep(TEXT("key"));
	Release->SetStringField(TEXT("key"), InEvent.Key.ToString());
	Release->SetStringField(TEXT("phase"), TEXT("release"));
	AddStep(Release);
}

void FGameTestRecorder::ProcessWheel(const FRawEvent& InEvent)
{
	FlushTyping();

	bool bIsUi = false;
	FString Target;
	TSharedPtr<FJsonObject> Hint;
	TSharedPtr<SWidget> Slate;
	if (!DescribeHandler(InEvent, bIsUi, Target, Hint, Slate))
	{
		return;
	}

	// 连着滚同一个地方：合成一条，delta 相加。
	if (Steps.Num() > 0 && InEvent.Time - LastEventAt < GameTestRecorderDetail::WheelMergeSeconds)
	{
		const TSharedRef<FJsonObject>& Last = Steps.Last();
		FString LastAction;
		FString LastTarget;
		Last->TryGetStringField(TEXT("action"), LastAction);
		Last->TryGetStringField(TEXT("target"), LastTarget);
		if (LastAction == TEXT("wheel") && LastTarget == Target)
		{
			Last->SetNumberField(TEXT("delta"), Last->GetNumberField(TEXT("delta")) + InEvent.Delta);
			LastEventAt = InEvent.Time;
			Broadcast(TEXT("step"));
			return;
		}
	}

	AddGapWait(InEvent.Time, bIsUi);
	if (!bIsUi)
	{
		// 游戏画面上滚：先把光标摆到原处（滚轮发在光标位置）。
		FVector2D Point;
		if (ToViewportPoint(InEvent.Position, Point))
		{
			const TSharedRef<FJsonObject> Cursor = GameTestRecorderDetail::MakeStep(TEXT("input.cursor"));
			Cursor->SetNumberField(TEXT("x"), Point.X);
			Cursor->SetNumberField(TEXT("y"), Point.Y);
			AddStep(Cursor);
		}
	}
	const TSharedRef<FJsonObject> Wheel = GameTestRecorderDetail::MakeStep(TEXT("wheel"));
	if (bIsUi)
	{
		Wheel->SetStringField(TEXT("target"), Target);
	}
	Wheel->SetNumberField(TEXT("delta"), InEvent.Delta);
	if (Hint.IsValid())
	{
		Wheel->SetObjectField(TEXT("hint"), Hint);
	}
	AddStep(Wheel);
}

void FGameTestRecorder::ProcessKeyDown(const FRawEvent& InEvent, bool bInTyping)
{
	if (HeldKeys.Contains(InEvent.Key))
	{
		return;
	}

	FHeldKey Held;
	Held.Time = InEvent.Time;
	if (bInTyping)
	{
		Held.bTyping = true;
		HeldKeys.Add(InEvent.Key, Held);
		return;
	}

	bool bIsUi = false;
	FString Target;
	TSharedPtr<FJsonObject> Hint;
	TSharedPtr<SWidget> Slate;
	if (InEvent.bHasHandler && !DescribeHandler(InEvent, bIsUi, Target, Hint, Slate))
	{
		return;
	}

	// 被输入框接住的键（回车、退格、方向键）：先把打到一半的字落成一条 type，步骤顺序才和人操作的一致。
	FlushTyping();

	AddGapWait(InEvent.Time, bIsUi);
	const TSharedRef<FJsonObject> Step = GameTestRecorderDetail::MakeStep(TEXT("key"));
	Step->SetStringField(TEXT("key"), InEvent.Key.ToString());
	Step->SetStringField(TEXT("phase"), TEXT("press"));
	if (bIsUi)
	{
		Step->SetStringField(TEXT("target"), Target);
		Held.Target = Target;
	}
	AddStep(Step);
	HeldKeys.Add(InEvent.Key, Held);
}

void FGameTestRecorder::ProcessKeyUp(const FRawEvent& InEvent)
{
	const FHeldKey* Held = HeldKeys.Find(InEvent.Key);
	if (Held == nullptr)
	{
		// 录制开始前就按下的键：按下没录到，松开也不录。
		return;
	}
	const FHeldKey Down = *Held;
	HeldKeys.Remove(InEvent.Key);
	if (Down.bTyping)
	{
		return;
	}

	// 按下后马上松开、中间没录别的：合成一条 tap。
	const bool bQuick = InEvent.Time - Down.Time < GameTestRecorderDetail::TapSeconds;
	if (bQuick && Steps.Num() > 0)
	{
		const TSharedRef<FJsonObject>& Last = Steps.Last();
		FString LastAction;
		FString LastKey;
		Last->TryGetStringField(TEXT("action"), LastAction);
		Last->TryGetStringField(TEXT("key"), LastKey);
		if (LastAction == TEXT("key") && LastKey == InEvent.Key.ToString())
		{
			Last->SetStringField(TEXT("phase"), TEXT("tap"));
			LastEventAt = InEvent.Time;
			Broadcast(TEXT("step"));
			return;
		}
	}

	// 长按、中间没录别的步骤：合成一条带 holdMs 的 tap（「按住 W 1.2 秒」），回放按住这么久再松开。
	// 按住期间录了别的步骤（按住 Alt 点按钮、按住 W 时点技能）才分成 press … release，那时按住的状态本身就是操作的一部分。
	if (!bQuick && Steps.Num() > 0)
	{
		const TSharedRef<FJsonObject>& Last = Steps.Last();
		FString LastAction;
		FString LastKey;
		FString LastPhase;
		Last->TryGetStringField(TEXT("action"), LastAction);
		Last->TryGetStringField(TEXT("key"), LastKey);
		Last->TryGetStringField(TEXT("phase"), LastPhase);
		if (LastAction == TEXT("key") && LastKey == InEvent.Key.ToString() && LastPhase == TEXT("press"))
		{
			Last->SetStringField(TEXT("phase"), TEXT("tap"));
			Last->SetNumberField(TEXT("holdMs"), FMath::RoundToInt((InEvent.Time - Down.Time) * 1000.0));
			LastEventAt = InEvent.Time;
			Broadcast(TEXT("step"));
			return;
		}
	}

	AddGapWait(InEvent.Time, !Down.Target.IsEmpty());
	const TSharedRef<FJsonObject> Step = GameTestRecorderDetail::MakeStep(TEXT("key"));
	Step->SetStringField(TEXT("key"), InEvent.Key.ToString());
	Step->SetStringField(TEXT("phase"), TEXT("release"));
	if (!Down.Target.IsEmpty())
	{
		Step->SetStringField(TEXT("target"), Down.Target);
	}
	AddStep(Step);
}

void FGameTestRecorder::ProcessChar(const FRawEvent& InEvent)
{
	const TSharedPtr<SWidget> Handler = InEvent.Handler.Pin();
	if (!Handler.IsValid())
	{
		return;
	}

	// 同一个控件接着打：只往缓冲里加字，不重新写选择器。
	if (Typing.IsSet() && Typing->Handler.Pin() == Handler)
	{
		Typing->Text.AppendChar(InEvent.Character);
		return;
	}

	FlushTyping();
	bool bIsUi = false;
	FString Target;
	TSharedPtr<FJsonObject> Hint;
	TSharedPtr<SWidget> Slate;
	if (!DescribeHandler(InEvent, bIsUi, Target, Hint, Slate) || !bIsUi)
	{
		return;
	}

	FTyping NewTyping;
	NewTyping.Handler = Handler;
	NewTyping.Target = Target;
	NewTyping.Hint = Hint;
	NewTyping.Text.AppendChar(InEvent.Character);
	Typing = NewTyping;
	LastEventAt = InEvent.Time;
}

void FGameTestRecorder::DropPickModifiers()
{
	static const FName Modifiers[] = {
		EKeys::LeftControl.GetFName(), EKeys::RightControl.GetFName(), EKeys::LeftAlt.GetFName(), EKeys::RightAlt.GetFName()};

	for (const FName& Key : Modifiers)
	{
		FHeldKey* Held = HeldKeys.Find(Key);
		if (Held == nullptr || Held->bTyping)
		{
			continue;
		}
		// 松开时按「打字顺带的键」跳过。
		Held->bTyping = true;
	}

	// 删掉步骤尾巴上这几个修饰键的按下（以及夹在中间的 wait）：拾取断言前面只该是人真正的操作。
	while (Steps.Num() > 0)
	{
		const TSharedRef<FJsonObject>& Last = Steps.Last();
		FString Action;
		FString Key;
		FString Phase;
		Last->TryGetStringField(TEXT("action"), Action);
		Last->TryGetStringField(TEXT("key"), Key);
		Last->TryGetStringField(TEXT("phase"), Phase);
		bool bModifier = false;
		for (const FName& Modifier : Modifiers)
		{
			bModifier |= Modifier.ToString() == Key;
		}
		const bool bModifierPress = Action == TEXT("key") && Phase == TEXT("press") && bModifier;
		if (!bModifierPress && Action != TEXT("wait"))
		{
			break;
		}
		Steps.Pop();
	}
}

void FGameTestRecorder::ProcessPick(const FRawEvent& InEvent)
{
	FlushTyping();
	DropPickModifiers();

	UWidget* Widget = GameTestSlate::FindOwningWidget(InEvent.HitLeaf.Pin());
	if (Widget == nullptr)
	{
		UE_LOG(LogGameTest, Display, TEXT("录制：Ctrl+Alt 点到的地方没有 UMG 控件，没录断言"));
		return;
	}

	FGameTestWrittenTarget Written;
	FString Error;
	if (!GameTestSelectorWriter::Write(*Widget, Written, Error))
	{
		UE_LOG(LogGameTest, Warning, TEXT("录制：拾取的控件写不出选择器（%s）"), *Error);
		return;
	}

	AddGapWait(InEvent.Time, /*bInUiStep=*/true);
	const TSharedRef<FJsonObject> Assert = GameTestRecorderDetail::MakeStep(TEXT("assert"));
	Assert->SetStringField(TEXT("target"), Written.Target);
	Assert->SetBoolField(TEXT("visible"), true);

	// 文字类控件顺带断言文字（快照里只有文字类控件有 Text）：录下此刻看到的。
	TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
	GameTestSnapshot::CaptureScreens(GameTestWorld::FindGameWorld(), Screens, /*bInLight=*/true);
	TSharedPtr<FGameTestWidgetNode> Node;
	FString ResolveError;
	if (Written.bVerified && GameTestSelector::ResolveText(Screens, Written.Target, Node, ResolveError) && Node.IsValid() && !Node->Text.IsEmpty())
	{
		Assert->SetStringField(TEXT("text"), Node->Text);
	}
	if (Written.Hint.IsValid())
	{
		Assert->SetObjectField(TEXT("hint"), Written.Hint);
	}
	Assert->SetStringField(TEXT("note"), TEXT("录制时 Ctrl+Alt 拾取的断言"));
	AddStep(Assert);
}

void FGameTestRecorder::FlushTyping()
{
	if (!Typing.IsSet())
	{
		return;
	}

	const FTyping Done = Typing.GetValue();
	Typing.Reset();
	if (Done.Text.IsEmpty())
	{
		return;
	}

	const TSharedRef<FJsonObject> Step = GameTestRecorderDetail::MakeStep(TEXT("type"));
	Step->SetStringField(TEXT("target"), Done.Target);
	Step->SetStringField(TEXT("text"), Done.Text);
	// 录的是「在原有文字后面接着打」：点进去时光标在哪、删了几个字都已经按顺序录成了 click / key。
	Step->SetBoolField(TEXT("append"), true);
	if (Done.Hint.IsValid())
	{
		Step->SetObjectField(TEXT("hint"), Done.Hint);
	}
	AddStep(Step);
}

bool FGameTestRecorder::ToViewportPoint(const FVector2D& InScreenPosition, FVector2D& OutPoint) const
{
	const TSharedPtr<SWidget> Viewport = GameTestSlate::GetGameViewportWidget();
	if (!Viewport.IsValid())
	{
		return false;
	}
	const FGeometry& Geometry = Viewport->GetCachedGeometry();
	const FVector2D Local = FVector2D(Geometry.AbsoluteToLocal(InScreenPosition));
	const FVector2D Size = FVector2D(Geometry.GetLocalSize());
	OutPoint.X = Size.X > 0.0 ? FMath::RoundToInt(FMath::Clamp(Local.X / Size.X, 0.0, 1.0) * 1000.0) / 1000.0 : 0.5;
	OutPoint.Y = Size.Y > 0.0 ? FMath::RoundToInt(FMath::Clamp(Local.Y / Size.Y, 0.0, 1.0) * 1000.0) / 1000.0 : 0.5;
	return true;
}

void FGameTestRecorder::AddGapWait(double InNow, bool bInUiStep)
{
	// UI 操作回放时自己会等控件可操作，录下来的秒数只会让慢机器失败、快机器白等；keepPace 时照录。
	const bool bSkip = bInUiStep && !bKeepPace;

	// 第一个动作之前的空档不录：那是人在准备，不是场景的一部分。
	if (!bSkip && LastEventAt > 0.0)
	{
		const double Gap = InNow - LastEventAt;
		if (Gap >= GameTestRecorderDetail::MinGapSeconds)
		{
			const TSharedRef<FJsonObject> Wait = GameTestRecorderDetail::MakeStep(TEXT("wait"));
			Wait->SetStringField(TEXT("until"), TEXT("seconds"));
			Wait->SetNumberField(TEXT("seconds"), FMath::RoundToInt(Gap * 100.0) / 100.0);
			AddStep(Wait);
		}
	}
	LastEventAt = InNow;
}

void FGameTestRecorder::AddStep(const TSharedRef<FJsonObject>& InStep)
{
	if (Steps.Num() >= GameTestRecorderDetail::MaxSteps)
	{
		bTruncated = true;
		return;
	}
	Steps.Add(InStep);
	Broadcast(TEXT("step"));
}

void FGameTestRecorder::Broadcast(const TCHAR* InPhase) const
{
	const TSharedRef<FJsonObject> Event = DescribeStatus(/*InLastSteps=*/1);
	Event->SetStringField(TEXT("phase"), InPhase);
	GameTestHooks::OnRecordChanged().Broadcast(Event);
}
