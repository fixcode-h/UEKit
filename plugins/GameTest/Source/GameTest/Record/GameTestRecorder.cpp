// Copyright Epic Games, Inc. All Rights Reserved.

#include "Record/GameTestRecorder.h"

#include "Core/GameTestHooks.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestScenario.h"
#include "Core/GameTestWorld.h"
#include "Editor/GameTestEditorApi.h"
#include "Input/GameTestInput.h"
#include "Observe/GameTestSelector.h"
#include "Observe/GameTestSnapshot.h"
#include "Observe/GameTestWidgetNode.h"
#include "Operate/GameTestOps.h"
#include "GameTest.h"
#include "GameTestSettings.h"

#include "Components/ComboBoxString.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/MultiLineEditableText.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/Slider.h"
#include "Components/SpinBox.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "InputCoreTypes.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace
{
	/** 两次操作隔这么久以上才补 wait（太短的间隔是人手的抖动，回放时一帧一步已经够了）。 */
	constexpr double MinGapSeconds = 0.15;

	/** 按下到松开短于这个就录成 tap。 */
	constexpr double TapSeconds = 0.25;

	/** 一次录制最多这么多步：再多的场景已经没法读，也说明录的时候忘了停。 */
	constexpr int32 MaxSteps = 2000;

	/** 光标落在哪：游戏窗口外（不录）、游戏画面上（录坐标 + 按键）、某个 UMG 控件上（录选择器）。 */
	enum class EHitKind : uint8
	{
		NotGame,
		Game,
		Widget,
	};

	/** 改值类控件：离开时录最终值。 */
	enum class EEditKind : uint8
	{
		None,
		Text,
		Option,
		Value,
	};

	EEditKind GetEditKind(const UWidget* InWidget)
	{
		if (InWidget == nullptr)
		{
			return EEditKind::None;
		}
		if (InWidget->IsA<UEditableText>() || InWidget->IsA<UEditableTextBox>()
			|| InWidget->IsA<UMultiLineEditableText>() || InWidget->IsA<UMultiLineEditableTextBox>())
		{
			return EEditKind::Text;
		}
		if (InWidget->IsA<UComboBoxString>())
		{
			return EEditKind::Option;
		}
		if (InWidget->IsA<USlider>() || InWidget->IsA<USpinBox>())
		{
			return EEditKind::Value;
		}
		return EEditKind::None;
	}

	/** 改值类控件的当前值（统一成文本好比较；数值用 %g，避免 0.5 和 0.500001 被当成改过）。 */
	FString ReadEditValue(const UWidget* InWidget)
	{
		if (const UEditableText* Text = Cast<UEditableText>(InWidget))
		{
			return Text->GetText().ToString();
		}
		if (const UEditableTextBox* TextBox = Cast<UEditableTextBox>(InWidget))
		{
			return TextBox->GetText().ToString();
		}
		if (const UMultiLineEditableText* MultiLine = Cast<UMultiLineEditableText>(InWidget))
		{
			return MultiLine->GetText().ToString();
		}
		if (const UMultiLineEditableTextBox* MultiLineBox = Cast<UMultiLineEditableTextBox>(InWidget))
		{
			return MultiLineBox->GetText().ToString();
		}
		if (const UComboBoxString* Combo = Cast<UComboBoxString>(InWidget))
		{
			return Combo->GetSelectedOption();
		}
		if (const USlider* Slider = Cast<USlider>(InWidget))
		{
			return FString::Printf(TEXT("%g"), Slider->GetValue());
		}
		if (const USpinBox* SpinBox = Cast<USpinBox>(InWidget))
		{
			return FString::Printf(TEXT("%g"), SpinBox->GetValue());
		}
		return FString();
	}

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

	FString GetScenarioRootDir()
	{
		return UGameTestSettings::ResolveScenarioRootDir();
	}

	/** 控件树里每个建出了 Slate 控件的节点，按 Slate 控件指针建索引（光标命中的是 Slate 控件）。 */
	struct FIndexedNode
	{
		TSharedPtr<FGameTestWidgetNode> Node;
		TSharedPtr<FGameTestWidgetNode> Screen;
	};

	void IndexNodes(
		const TSharedPtr<FGameTestWidgetNode>& InNode,
		const TSharedPtr<FGameTestWidgetNode>& InScreen,
		TMap<const SWidget*, FIndexedNode>& OutIndex)
	{
		if (!InNode.IsValid())
		{
			return;
		}
		if (const UWidget* Widget = InNode->Widget.Get())
		{
			if (const TSharedPtr<SWidget> Cached = Widget->GetCachedWidget())
			{
				OutIndex.Add(Cached.Get(), { InNode, InScreen });
			}
		}
		for (const TSharedPtr<FGameTestWidgetNode>& Child : InNode->Children)
		{
			IndexNodes(Child, InScreen, OutIndex);
		}
	}
}

/** Slate 输入预处理器：只看不吃（每个回调都返回 false），事件照常往下走。 */
class FGameTestRecorderInput : public IInputProcessor
{
public:
	virtual void Tick(const float InDeltaTime, FSlateApplication& InSlateApp, TSharedRef<ICursor> InCursor) override
	{
	}

	virtual bool HandleKeyDownEvent(FSlateApplication& InSlateApp, const FKeyEvent& InKeyEvent) override
	{
		if (!InKeyEvent.IsRepeat() && !GameTestInput::IsSendingSynthetic())
		{
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
		if (!GameTestInput::IsSendingSynthetic())
		{
			FGameTestRecorder::Get().OnMouseButton(InMouseEvent.GetEffectingButton(), /*bInDown=*/true, FVector2D(InMouseEvent.GetScreenSpacePosition()));
		}
		return false;
	}

	virtual bool HandleMouseButtonUpEvent(FSlateApplication& InSlateApp, const FPointerEvent& InMouseEvent) override
	{
		if (!GameTestInput::IsSendingSynthetic())
		{
			FGameTestRecorder::Get().OnMouseButton(InMouseEvent.GetEffectingButton(), /*bInDown=*/false, FVector2D(InMouseEvent.GetScreenSpacePosition()));
		}
		return false;
	}

	virtual const TCHAR* GetDebugName() const override
	{
		return TEXT("GameTestRecorder");
	}
};

struct FGameTestRecorder::FPendingEdit
{
	FString Target;
	TWeakObjectPtr<UWidget> Widget;
	EEditKind Kind = EEditKind::None;
	FString Initial;
};

FGameTestRecorder::FGameTestRecorder() = default;

FGameTestRecorder::~FGameTestRecorder() = default;

FGameTestRecorder& FGameTestRecorder::Get()
{
	static FGameTestRecorder Instance;
	return Instance;
}

bool FGameTestRecorder::Start(const FString& InName, bool bInStartPie, FString& OutError)
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
	KeyDownAt.Reset();
	MouseDownTarget.Reset();
	bMouseDownOnGame = false;
	bTruncated = false;
	PendingEdit.Reset();
	StartedAt = FPlatformTime::Seconds();
	LastEventAt = 0.0;

	// 编辑器里 PIE 正在跑：回放时先把同一张地图的 PIE 拉起来，场景才能单独跑。
	if (bInStartPie && GameTestEditor::IsAvailable() && GameTestEditor::IsPieRunning())
	{
		const TSharedRef<FJsonObject> Pie = MakeStep(TEXT("pie.start"));
		const FString Map = GameTestEditor::GetEditorLevelPath();
		if (!Map.IsEmpty())
		{
			Pie->SetStringField(TEXT("map"), Map);
		}
		Pie->SetStringField(TEXT("note"), TEXT("录制时 PIE 已经在跑：回放先启动同一张地图"));
		Steps.Add(Pie);
	}

	InputProcessor = MakeShared<FGameTestRecorderInput>();
	FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
	bRecording = true;

	UE_LOG(LogGameTest, Display, TEXT("开始录制「%s」"), *Name);
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

	FlushPendingEdit();
	AddGapWait();

	const TSharedRef<FJsonObject> Step = MakeStep(TEXT("mark"));
	Step->SetStringField(TEXT("text"), InText.IsEmpty() ? TEXT("检查点") : InText);
	AddStep(Step);

	if (bInScreenshot)
	{
		const TSharedRef<FJsonObject> Shot = MakeStep(TEXT("screenshot"));
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

	FlushPendingEdit();

	// 录制结束时还按着的键补一条松开：回放时不留卡住的键。
	TArray<FName> StillDown;
	KeyDownAt.GetKeys(StillDown);
	for (const FName& KeyName : StillDown)
	{
		const TSharedRef<FJsonObject> Release = MakeStep(TEXT("key"));
		Release->SetStringField(TEXT("key"), KeyName.ToString());
		Release->SetStringField(TEXT("phase"), TEXT("release"));
		AddStep(Release);
	}
	KeyDownAt.Reset();

	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();
	bRecording = false;

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
		// 录下来的节奏靠 wait 步骤还原，步间不再额外停顿。
		Scenario->SetNumberField(TEXT("stepDelayMs"), 0);
		Scenario->SetArrayField(TEXT("steps"), StepValues);

		const FString RootDir = GetScenarioRootDir();
		const FString Stem = ToFileStem(Name);
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
	if (!bRecording)
	{
		return Status;
	}

	Status->SetStringField(TEXT("name"), Name);
	Status->SetNumberField(TEXT("steps"), Steps.Num());
	Status->SetNumberField(TEXT("seconds"), FMath::RoundToInt((FPlatformTime::Seconds() - StartedAt) * 10.0) / 10.0);
	if (bTruncated)
	{
		Status->SetBoolField(TEXT("truncated"), true);
	}

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
	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();
	bRecording = false;
	Steps.Reset();
	PendingEdit.Reset();
}

void FGameTestRecorder::OnKey(const FKey& InKey, bool bInDown)
{
	if (!bRecording || InKey.IsMouseButton())
	{
		return;
	}

	// 只录发给游戏的按键：键盘焦点在编辑器别的面板上时不算。
	UGameViewportClient* Viewport = GameTestWorld::FindGameViewport();
	const TSharedPtr<SWindow> GameWindow = Viewport != nullptr ? Viewport->GetWindow() : nullptr;
	const TSharedPtr<SWindow> ActiveWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
	if (!GameWindow.IsValid() || ActiveWindow != GameWindow)
	{
		return;
	}

	// 正在往输入框里打字：按键不录，离开时录最终文本；回车算「提交」，顺手录掉。
	if (PendingEdit.IsValid() && PendingEdit->Kind == EEditKind::Text)
	{
		if (!bInDown && (InKey == EKeys::Enter || InKey == EKeys::Tab))
		{
			FlushPendingEdit();
		}
		return;
	}

	const FName KeyName = InKey.GetFName();
	const double Now = FPlatformTime::Seconds();
	if (bInDown)
	{
		if (KeyDownAt.Contains(KeyName))
		{
			return;
		}
		AddGapWait();
		const TSharedRef<FJsonObject> Step = MakeStep(TEXT("key"));
		Step->SetStringField(TEXT("key"), KeyName.ToString());
		Step->SetStringField(TEXT("phase"), TEXT("press"));
		AddStep(Step);
		KeyDownAt.Add(KeyName, Now);
		return;
	}

	const double* DownAt = KeyDownAt.Find(KeyName);
	if (DownAt == nullptr)
	{
		// 录制开始前就按下的键：按下没录到，松开也不录。
		return;
	}

	// 按下后马上松开、中间没录别的：合成一条 tap。
	const bool bQuick = Now - *DownAt < TapSeconds;
	KeyDownAt.Remove(KeyName);
	if (bQuick && Steps.Num() > 0)
	{
		const TSharedRef<FJsonObject>& Last = Steps.Last();
		FString LastAction;
		FString LastKey;
		Last->TryGetStringField(TEXT("action"), LastAction);
		Last->TryGetStringField(TEXT("key"), LastKey);
		if (LastAction == TEXT("key") && LastKey == KeyName.ToString())
		{
			Last->SetStringField(TEXT("phase"), TEXT("tap"));
			LastEventAt = Now;
			Broadcast(TEXT("step"));
			return;
		}
	}

	AddGapWait();
	const TSharedRef<FJsonObject> Step = MakeStep(TEXT("key"));
	Step->SetStringField(TEXT("key"), KeyName.ToString());
	Step->SetStringField(TEXT("phase"), TEXT("release"));
	AddStep(Step);
}

void FGameTestRecorder::OnMouseButton(const FKey& InButton, bool bInDown, const FVector2D& InScreenPosition)
{
	if (!bRecording)
	{
		return;
	}

	TSharedPtr<FJsonObject> Hit;
	const bool bOnWidget = LocateWidgetUnderCursor(InScreenPosition, Hit);
	if (!Hit.IsValid())
	{
		// 不在游戏窗口里：编辑器其他地方的点击不录。
		return;
	}

	const FString Target = bOnWidget ? Hit->GetStringField(TEXT("target")) : FString();
	const FString Kind = bOnWidget ? Hit->GetStringField(TEXT("kind")) : FString();

	if (bInDown)
	{
		// 点到别处就算离开了正在改的控件：先把它的最终值录下来。
		if (PendingEdit.IsValid() && PendingEdit->Target != Target)
		{
			FlushPendingEdit();
		}

		if (Kind == TEXT("click"))
		{
			MouseDownTarget = Target;
			return;
		}

		if (Kind == TEXT("edit"))
		{
			if (!PendingEdit.IsValid())
			{
				// 命中信息是 JSON（放不了控件指针），这里按选择器再解析一次拿到控件。
				UWidget* Widget = nullptr;
				TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
				GameTestSnapshot::CaptureScreens(GameTestWorld::FindGameWorld(), Screens);
				TSharedPtr<FGameTestWidgetNode> Node;
				FString Error;
				if (GameTestSelector::ResolveText(Screens, Target, Node, Error) && Node.IsValid())
				{
					Widget = Node->Widget.Get();
				}
				if (Widget != nullptr)
				{
					PendingEdit = MakeUnique<FPendingEdit>();
					PendingEdit->Target = Target;
					PendingEdit->Widget = Widget;
					PendingEdit->Kind = GetEditKind(Widget);
					PendingEdit->Initial = ReadEditValue(Widget);
				}
			}
			return;
		}

		// 点在游戏画面上（或不能代码点击的控件上）：摆好光标再按真实的鼠标键。
		AddGapWait();
		const TSharedRef<FJsonObject> Cursor = MakeStep(TEXT("input.cursor"));
		Cursor->SetNumberField(TEXT("x"), Hit->GetNumberField(TEXT("x")));
		Cursor->SetNumberField(TEXT("y"), Hit->GetNumberField(TEXT("y")));
		AddStep(Cursor);
		const TSharedRef<FJsonObject> Press = MakeStep(TEXT("key"));
		Press->SetStringField(TEXT("key"), InButton.GetFName().ToString());
		Press->SetStringField(TEXT("phase"), TEXT("press"));
		AddStep(Press);
		KeyDownAt.Add(InButton.GetFName(), FPlatformTime::Seconds());
		bMouseDownOnGame = true;
		return;
	}

	// 松开
	if (!MouseDownTarget.IsEmpty())
	{
		// 按下和松开在同一个控件上才是一次点击（拖出去再松开，UMG 自己也不算点击）。
		if (MouseDownTarget == Target)
		{
			AddGapWait();
			const TSharedRef<FJsonObject> Click = MakeStep(TEXT("click"));
			Click->SetStringField(TEXT("target"), Target);
			AddStep(Click);
		}
		MouseDownTarget.Reset();
		return;
	}

	if (bMouseDownOnGame)
	{
		bMouseDownOnGame = false;
		const FName ButtonName = InButton.GetFName();
		const double* DownAt = KeyDownAt.Find(ButtonName);
		const bool bQuick = DownAt != nullptr && FPlatformTime::Seconds() - *DownAt < TapSeconds;
		KeyDownAt.Remove(ButtonName);
		if (bQuick && Steps.Num() > 0)
		{
			FString LastKey;
			Steps.Last()->TryGetStringField(TEXT("key"), LastKey);
			if (LastKey == ButtonName.ToString())
			{
				Steps.Last()->SetStringField(TEXT("phase"), TEXT("tap"));
				LastEventAt = FPlatformTime::Seconds();
				Broadcast(TEXT("step"));
				return;
			}
		}
		AddGapWait();
		const TSharedRef<FJsonObject> Release = MakeStep(TEXT("key"));
		Release->SetStringField(TEXT("key"), ButtonName.ToString());
		Release->SetStringField(TEXT("phase"), TEXT("release"));
		AddStep(Release);
	}
}

void FGameTestRecorder::AddGapWait()
{
	const double Now = FPlatformTime::Seconds();
	// 第一个动作之前的空档不录：那是人在准备，不是场景的一部分。
	if (LastEventAt > 0.0)
	{
		const double Gap = Now - LastEventAt;
		if (Gap >= MinGapSeconds)
		{
			const TSharedRef<FJsonObject> Wait = MakeStep(TEXT("wait"));
			Wait->SetStringField(TEXT("until"), TEXT("seconds"));
			Wait->SetNumberField(TEXT("seconds"), FMath::RoundToInt(Gap * 100.0) / 100.0);
			AddStep(Wait);
		}
	}
	LastEventAt = Now;
}

void FGameTestRecorder::AddStep(const TSharedRef<FJsonObject>& InStep)
{
	if (Steps.Num() >= MaxSteps)
	{
		bTruncated = true;
		return;
	}
	Steps.Add(InStep);
	Broadcast(TEXT("step"));
}

void FGameTestRecorder::FlushPendingEdit()
{
	if (!PendingEdit.IsValid())
	{
		return;
	}

	const TUniquePtr<FPendingEdit> Edit = MoveTemp(PendingEdit);
	const UWidget* Widget = Edit->Widget.Get();
	if (Widget == nullptr)
	{
		return;
	}

	const FString Value = ReadEditValue(Widget);
	if (Value == Edit->Initial)
	{
		return;
	}

	AddGapWait();
	TSharedRef<FJsonObject> Step = MakeStep(Edit->Kind == EEditKind::Text ? TEXT("type") : TEXT("set"));
	Step->SetStringField(TEXT("target"), Edit->Target);
	switch (Edit->Kind)
	{
	case EEditKind::Text:
		Step->SetStringField(TEXT("text"), Value);
		Step->SetBoolField(TEXT("commit"), true);
		break;
	case EEditKind::Option:
		Step->SetStringField(TEXT("option"), Value);
		break;
	case EEditKind::Value:
		Step->SetNumberField(TEXT("value"), FCString::Atod(*Value));
		break;
	default:
		return;
	}
	AddStep(Step);
}

bool FGameTestRecorder::LocateWidgetUnderCursor(const FVector2D& InScreenPosition, TSharedPtr<FJsonObject>& OutWidgetInfo) const
{
	OutWidgetInfo.Reset();

	UWorld* World = GameTestWorld::FindGameWorld();
	UGameViewportClient* Viewport = GameTestWorld::FindGameViewport();
	const TSharedPtr<SWindow> GameWindow = Viewport != nullptr ? Viewport->GetWindow() : nullptr;
	if (World == nullptr || !GameWindow.IsValid())
	{
		return false;
	}

	FSlateApplication& App = FSlateApplication::Get();
	const FWidgetPath Path = App.LocateWindowUnderMouse(InScreenPosition, App.GetInteractiveTopLevelWindows());
	if (!Path.IsValid() || Path.GetWindow() != GameWindow.ToSharedRef())
	{
		return false;
	}

	// 先给出「在游戏窗口里」和归一化坐标：没点到 UMG 时录坐标用。
	OutWidgetInfo = MakeShared<FJsonObject>();
	if (const TSharedPtr<SViewport> ViewportWidget = Viewport->GetGameViewportWidget())
	{
		const FGeometry& Geometry = ViewportWidget->GetCachedGeometry();
		const FVector2D Local = FVector2D(Geometry.AbsoluteToLocal(InScreenPosition));
		const FVector2D Size = FVector2D(Geometry.GetLocalSize());
		OutWidgetInfo->SetNumberField(TEXT("x"), Size.X > 0.0 ? FMath::RoundToInt(FMath::Clamp(Local.X / Size.X, 0.0, 1.0) * 1000.0) / 1000.0 : 0.5);
		OutWidgetInfo->SetNumberField(TEXT("y"), Size.Y > 0.0 ? FMath::RoundToInt(FMath::Clamp(Local.Y / Size.Y, 0.0, 1.0) * 1000.0) / 1000.0 : 0.5);
	}

	TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
	GameTestSnapshot::CaptureScreens(World, Screens);
	TMap<const SWidget*, FIndexedNode> Index;
	for (const TSharedPtr<FGameTestWidgetNode>& Screen : Screens)
	{
		IndexNodes(Screen, Screen, Index);
	}

	// 从最深的命中往外找第一个能操作的 UMG 控件（点在按钮里的文字上，录的是按钮）。
	for (int32 WidgetIndex = Path.Widgets.Num() - 1; WidgetIndex >= 0; --WidgetIndex)
	{
		const FIndexedNode* Found = Index.Find(&Path.Widgets[WidgetIndex].Widget.Get());
		if (Found == nullptr || !Found->Node.IsValid())
		{
			continue;
		}

		const UWidget* Widget = Found->Node->Widget.Get();
		FString Hint;
		const bool bClickable = GameTestOps::IsClickable(Widget, Hint);
		const bool bEditable = GetEditKind(Widget) != EEditKind::None;
		if (!bClickable && !bEditable)
		{
			continue;
		}

		// 选择器 = 屏的类名 + 屏内相对路径；回放前用同一套解析核对一遍，确认指回的是同一个控件。
		FString Relative = Found->Node->Path;
		Relative.RemoveFromStart(Found->Screen->Path + TEXT("/"));
		FString Target = FString::Printf(TEXT("%s:%s"), *Found->Screen->ClassName, *Relative);

		TSharedPtr<FGameTestWidgetNode> Check;
		FString Error;
		if (!GameTestSelector::ResolveText(Screens, Target, Check, Error) || !Check.IsValid() || Check->Widget != Found->Node->Widget)
		{
			// 同一个屏开了不止一个：不写屏名时整棵树里可能是唯一的。
			const FString Fallback = Relative;
			if (GameTestSelector::ResolveText(Screens, Fallback, Check, Error) && Check.IsValid() && Check->Widget == Found->Node->Widget)
			{
				Target = Fallback;
			}
			else
			{
				OutWidgetInfo->SetStringField(TEXT("ambiguous"), Error);
			}
		}

		OutWidgetInfo->SetStringField(TEXT("target"), Target);
		OutWidgetInfo->SetStringField(TEXT("kind"), bClickable ? TEXT("click") : TEXT("edit"));
		return true;
	}

	return false;
}

void FGameTestRecorder::Broadcast(const TCHAR* InPhase) const
{
	const TSharedRef<FJsonObject> Event = DescribeStatus(/*InLastSteps=*/1);
	Event->SetStringField(TEXT("phase"), InPhase);
	GameTestHooks::OnRecordChanged().Broadcast(Event);
}
