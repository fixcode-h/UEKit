// Copyright Epic Games, Inc. All Rights Reserved.

#include "EditorRec/GameTestEditorRecorder.h"

#include "EditorAct/GameTestEditorCommands.h"
#include "EditorAct/GameTestEditorRefs.h"
#include "EditorAct/GameTestEditorSlateFind.h"

#include "Core/GameTestHooks.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestScenario.h"
#include "GameTestSettings.h"
#include "Input/GameTestInput.h"
#include "Observe/GameTestSlate.h"
#include "Record/GameTestRecorder.h"

#include "AgentPanelBridgeUtil.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "InputCoreTypes.h"
#include "Layout/WidgetPath.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace GameTestEditorRecorderDetail
{
	/** 一次录制最多录多少步：再多多半是忘了停。 */
	constexpr int32 MaxSteps = 2000;
	/** 点击、快捷键挂多久等语义事件。 */
	constexpr double UiSettleSeconds = 0.5;
	/** 放 Actor 后多久内它的选中、属性变化算放 Actor 带出来的。 */
	constexpr double SpawnEchoSeconds = 1.0;

	TSharedRef<FJsonObject> MakeStep(const TCHAR* InAction, const TCHAR* InVia, const FString& InLabel)
	{
		const TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("action"), InAction);
		// hint 只给人看（执行时不读）：面板按 via 标「资产事件 / 界面点击 / Ctrl+Alt 点」
		const TSharedRef<FJsonObject> Hint = MakeShared<FJsonObject>();
		Hint->SetStringField(TEXT("via"), InVia);
		if (!InLabel.IsEmpty())
		{
			Hint->SetStringField(TEXT("label"), InLabel);
		}
		Step->SetObjectField(TEXT("hint"), Hint);
		return Step;
	}

	TArray<TSharedPtr<FJsonValue>> Vector3(double InX, double InY, double InZ)
	{
		// 留一位小数：拖进视口的位置带一长串小数，回放差零点几厘米没有意义，读的人也看不清
		auto Round = [](double InValue)
		{
			return FMath::RoundToDouble(InValue * 10.0) / 10.0;
		};
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(MakeShared<FJsonValueNumber>(Round(InX)));
		Out.Add(MakeShared<FJsonValueNumber>(Round(InY)));
		Out.Add(MakeShared<FJsonValueNumber>(Round(InZ)));
		return Out;
	}

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
		return Stem.IsEmpty() ? FString::Printf(TEXT("editor-%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"))) : Stem;
	}

	/** 属性改动 → 属性路径：结构体里的一项写成 结构体.项（ShopConfig.Discount），更深的或数组里的整段记成员。 */
	FString PropertyPath(const FPropertyChangedEvent& InEvent)
	{
		FProperty* Member = InEvent.MemberProperty != nullptr ? InEvent.MemberProperty : InEvent.Property;
		if (Member == nullptr)
		{
			return FString();
		}
		FString Path = Member->GetName();
		const FStructProperty* StructMember = CastField<FStructProperty>(Member);
		// Struct 在新版本里是 TObjectPtr：先取成裸指针再比，各版本都能编
		const UStruct* MemberStruct = StructMember != nullptr ? StructMember->Struct : nullptr;
		if (InEvent.Property != nullptr && InEvent.Property != Member && MemberStruct != nullptr && InEvent.Property->GetOwnerStruct() == MemberStruct)
		{
			Path += TEXT(".") + InEvent.Property->GetName();
		}
		return Path;
	}

	bool IsFunctionKey(const FKey& InKey)
	{
		const FString KeyName = InKey.GetFName().ToString();
		return KeyName.Len() >= 2 && KeyName.Len() <= 3 && KeyName[0] == TEXT('F') && FChar::IsDigit(KeyName[1]);
	}

	/** 键盘焦点在输入框里：打字和输入框自己的快捷键（Ctrl+A、Ctrl+C）不录成命令。 */
	bool IsTyping()
	{
		const TSharedPtr<SWidget> Focus = FSlateApplication::Get().GetKeyboardFocusedWidget();
		return Focus.IsValid() && Focus->GetTypeAsString().Contains(TEXT("EditableText"));
	}

	FString ChordText(const FKey& InKey, bool bInCtrl, bool bInAlt, bool bInShift, bool bInCmd)
	{
		FString Text;
		Text += bInCtrl ? TEXT("Ctrl+") : TEXT("");
		Text += bInAlt ? TEXT("Alt+") : TEXT("");
		Text += bInShift ? TEXT("Shift+") : TEXT("");
		Text += bInCmd ? TEXT("Cmd+") : TEXT("");
		return Text + InKey.GetFName().ToString();
	}
}

/** Slate 输入预处理器：只看不吃（Ctrl+Alt 拾取除外），事件照常往下走。 */
class FGameTestEditorRecordInput : public IInputProcessor
{
public:
	virtual void Tick(const float InDeltaTime, FSlateApplication& InSlateApp, TSharedRef<ICursor> InCursor) override
	{
		FGameTestEditorRecorder::Get().OnTick();
	}

	virtual bool HandleMouseButtonDownEvent(FSlateApplication& InSlateApp, const FPointerEvent& InMouseEvent) override
	{
		if (GameTestInput::IsSendingSynthetic() || InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
		{
			return false;
		}
		return FGameTestEditorRecorder::Get().OnMouseDown(FVector2D(InMouseEvent.GetScreenSpacePosition()), InMouseEvent.IsControlDown() && InMouseEvent.IsAltDown());
	}

	virtual bool HandleKeyDownEvent(FSlateApplication& InSlateApp, const FKeyEvent& InKeyEvent) override
	{
		if (!InKeyEvent.IsRepeat() && !GameTestInput::IsSendingSynthetic())
		{
			FGameTestEditorRecorder::Get().OnKeyDown(InKeyEvent.GetKey(), InKeyEvent.IsControlDown(), InKeyEvent.IsAltDown(), InKeyEvent.IsShiftDown(), InKeyEvent.IsCommandDown());
		}
		return false;
	}

	virtual const TCHAR* GetDebugName() const override
	{
		return TEXT("GameTestEditorRecorder");
	}
};

FGameTestEditorRecorder& FGameTestEditorRecorder::Get()
{
	static FGameTestEditorRecorder Instance;
	return Instance;
}

bool FGameTestEditorRecorder::Start(const FString& InName, FString& OutError)
{
	if (bRecording)
	{
		OutError = FString::Printf(TEXT("已经在录「%s」了：先 gt.editrec.stop"), *Name);
		return false;
	}
	if (FGameTestRecorder::Get().IsRecording())
	{
		OutError = TEXT("正在录游戏操作：先停掉游戏录制再录编辑器操作");
		return false;
	}
	if (GEditor == nullptr || !FSlateApplication::IsInitialized())
	{
		OutError = TEXT("这个进程没有编辑器界面（后台编辑器或游戏进程），录不了编辑器操作");
		return false;
	}

	Name = InName.IsEmpty() ? TEXT("编辑器操作") : InName;
	StartedAt = FPlatformTime::Seconds();
	Steps.Reset();
	bTruncated = false;
	PendingUi.Reset();
	PendingSpawns.Reset();
	RecentSpawns.Reset();
	PendingSaves.Reset();
	RecentCompiles.Reset();
	bSelectionDirty = false;
	LastSemanticAt = 0.0;
	SuppressUntil = 0.0;
	LastModalTitle.Reset();
	PreValue = FPreValue();
	// 开始时已经选中的不算一步：只录这之后改的
	LastSelectionKey.Reset();
	if (USelection* Selection = GEditor->GetSelectedActors())
	{
		TArray<FString> Refs;
		for (FSelectionIterator It(*Selection); It; ++It)
		{
			Refs.Add(GameTestEditorRefs::MakeRef(*It));
		}
		LastSelectionKey = FString::Join(Refs, TEXT("|"));
	}

	Bind();
	bRecording = true;
	Broadcast(TEXT("started"));
	return true;
}

void FGameTestEditorRecorder::Bind()
{
	Handles.Add(FEditorDelegates::OnMapOpened.AddLambda([](const FString& InFilename, bool /*bAsTemplate*/)
	{
		FGameTestEditorRecorder::Get().HandleMapOpened(InFilename);
	}));
	Handles.Add(USelection::SelectionChangedEvent.AddLambda([](UObject*)
	{
		FGameTestEditorRecorder& Recorder = FGameTestEditorRecorder::Get();
		if (!Recorder.bSelectionDirty)
		{
			Recorder.bSelectionDirty = true;
			Recorder.SelectionDirtyAt = FPlatformTime::Seconds();
		}
	}));
	Handles.Add(FCoreUObjectDelegates::OnPreObjectPropertyChanged.AddLambda([](UObject* InObject, const FEditPropertyChain& InChain)
	{
		FGameTestEditorRecorder::Get().HandlePreChange(InObject, InChain);
	}));
	Handles.Add(FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([](UObject* InObject, FPropertyChangedEvent& InEvent)
	{
		FGameTestEditorRecorder::Get().HandlePropertyChanged(InObject, InEvent);
	}));
	Handles.Add(GEngine->OnLevelActorAdded().AddLambda([](AActor* InActor)
	{
		FGameTestEditorRecorder::Get().HandleActorAdded(InActor);
	}));
	Handles.Add(GEditor->OnBlueprintPreCompile().AddLambda([](UBlueprint* InBlueprint)
	{
		FGameTestEditorRecorder::Get().HandlePreCompile(InBlueprint);
	}));
	Handles.Add(UPackage::PackageSavedWithContextEvent.AddLambda([](const FString& InFilename, UPackage* InPackage, FObjectPostSaveContext)
	{
		FGameTestEditorRecorder::Get().HandlePackageSaved(InFilename, InPackage);
	}));
	if (UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
	{
		AssetOpenedHandle = Editors->OnAssetEditorOpened().AddLambda([](UObject* InAsset)
		{
			FGameTestEditorRecorder::Get().HandleAssetOpened(InAsset);
		});
	}

	InputProcessor = MakeShared<FGameTestEditorRecordInput>();
	FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
}

void FGameTestEditorRecorder::Unbind()
{
	// 按 Bind 的顺序摘：句柄数组和委托一一对应
	if (Handles.Num() == 7)
	{
		FEditorDelegates::OnMapOpened.Remove(Handles[0]);
		USelection::SelectionChangedEvent.Remove(Handles[1]);
		FCoreUObjectDelegates::OnPreObjectPropertyChanged.Remove(Handles[2]);
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Handles[3]);
		if (GEngine != nullptr)
		{
			GEngine->OnLevelActorAdded().Remove(Handles[4]);
		}
		if (GEditor != nullptr)
		{
			GEditor->OnBlueprintPreCompile().Remove(Handles[5]);
		}
		UPackage::PackageSavedWithContextEvent.Remove(Handles[6]);
	}
	Handles.Reset();
	if (AssetOpenedHandle.IsValid() && GEditor != nullptr)
	{
		if (UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			Editors->OnAssetEditorOpened().Remove(AssetOpenedHandle);
		}
	}
	AssetOpenedHandle.Reset();
	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();
}

bool FGameTestEditorRecorder::Mark(const FString& InText, FString& OutError)
{
	if (!bRecording)
	{
		OutError = TEXT("没有在录编辑器操作：先 gt.editrec.start");
		return false;
	}
	const TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
	Step->SetStringField(TEXT("action"), TEXT("mark"));
	Step->SetStringField(TEXT("text"), InText.IsEmpty() ? TEXT("检查点") : InText);
	AddStep(Step, /*bInSemantic=*/false);
	return true;
}

bool FGameTestEditorRecorder::Stop(bool bInSave, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
{
	if (!bRecording)
	{
		OutError = TEXT("没有在录编辑器操作");
		return false;
	}
	// 还挂着的都按现在收尾：停止前最后一下点击、保存不丢
	FlushSpawns();
	FlushSelection(TNumericLimits<double>::Max());
	FlushSaves();
	FlushPendingUi(TNumericLimits<double>::Max());
	Unbind();
	bRecording = false;

	OutResult = MakeShared<FJsonObject>();
	OutResult->SetStringField(TEXT("name"), Name);
	OutResult->SetStringField(TEXT("target"), TEXT("editor"));
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
			TEXT("录制于 %s，录的是 UE 编辑器操作（editor.*），只能在编辑器里跑。会改关卡和资源：建议在干净的工作区跑，跑完不想要就撤销或还原。"), *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M"))));
		Scenario->SetNumberField(TEXT("revision"), 1);
		TArray<TSharedPtr<FJsonValue>> Tags;
		Tags.Add(MakeShared<FJsonValueString>(TEXT("recorded")));
		Tags.Add(MakeShared<FJsonValueString>(TEXT("editor")));
		Scenario->SetArrayField(TEXT("tags"), Tags);
		Scenario->SetNumberField(TEXT("stepDelayMs"), 0);
		Scenario->SetArrayField(TEXT("steps"), StepValues);

		const FString RootDir = UGameTestSettings::ResolveScenarioRootDir();
		const FString Stem = GameTestEditorRecorderDetail::ToFileStem(Name);
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

	TArray<TSharedPtr<FJsonValue>> Preview;
	for (int32 Index = 0; Index < FMath::Min(StepValues.Num(), 20); ++Index)
	{
		Preview.Add(StepValues[Index]);
	}
	OutResult->SetArrayField(TEXT("preview"), Preview);
	Broadcast(TEXT("stopped"));
	Steps.Reset();
	return true;
}

TSharedRef<FJsonObject> FGameTestEditorRecorder::DescribeStatus(int32 InLastSteps) const
{
	const TSharedRef<FJsonObject> Status = MakeShared<FJsonObject>();
	Status->SetBoolField(TEXT("recording"), bRecording);
	Status->SetStringField(TEXT("target"), TEXT("editor"));
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
	if (!LastModalTitle.IsEmpty())
	{
		Status->SetStringField(TEXT("modal"), LastModalTitle);
	}
	TArray<TSharedPtr<FJsonValue>> Last;
	for (int32 Index = FMath::Max(0, Steps.Num() - InLastSteps); Index < Steps.Num(); ++Index)
	{
		Last.Add(MakeShared<FJsonValueObject>(Steps[Index]));
	}
	Status->SetArrayField(TEXT("last"), Last);
	return Status;
}

void FGameTestEditorRecorder::Shutdown()
{
	if (bRecording)
	{
		Unbind();
	}
	bRecording = false;
	Steps.Reset();
	PendingUi.Reset();
	PendingSpawns.Reset();
}

void FGameTestEditorRecorder::Broadcast(const TCHAR* InPhase)
{
	const TSharedRef<FJsonObject> Event = DescribeStatus(/*InLastSteps=*/1);
	Event->SetStringField(TEXT("phase"), InPhase);
	GameTestHooks::OnRecordChanged().Broadcast(Event);
}

void FGameTestEditorRecorder::AddStep(const TSharedRef<FJsonObject>& InStep, bool bInSemantic)
{
	if (bInSemantic)
	{
		LastSemanticAt = FPlatformTime::Seconds();
	}
	if (Steps.Num() >= GameTestEditorRecorderDetail::MaxSteps)
	{
		bTruncated = true;
		return;
	}
	Steps.Add(InStep);
	Broadcast(TEXT("step"));
}

bool FGameTestEditorRecorder::IsSuppressed(UObject* InObject) const
{
	const double Now = FPlatformTime::Seconds();
	if (Now < SuppressUntil || GIsTransacting)
	{
		return true;
	}
	AActor* Actor = Cast<AActor>(InObject);
	if (Actor == nullptr)
	{
		if (const UActorComponent* Component = Cast<UActorComponent>(InObject))
		{
			Actor = Component->GetOwner();
		}
	}
	if (Actor == nullptr)
	{
		return false;
	}
	// 同一帧里刚放、还没写成步骤的 Actor：放的时候引擎会设位置、标签，这些不是人改的
	if (PendingSpawns.Contains(Actor))
	{
		return true;
	}
	const double* SpawnedAt = RecentSpawns.Find(Actor->GetActorLabel());
	return SpawnedAt != nullptr && Now - *SpawnedAt < GameTestEditorRecorderDetail::SpawnEchoSeconds;
}

//~ 语义事件 ----------------------------------------------------------------------

void FGameTestEditorRecorder::HandleMapOpened(const FString& InFilename)
{
	if (!bRecording)
	{
		return;
	}
	FString PackageName;
	if (!FPackageName::TryConvertFilenameToLongPackageName(InFilename, PackageName))
	{
		PackageName = InFilename;
	}
	// 换关卡会清选中、重建大纲：这一下之后引擎自己做的事不录
	SuppressUntil = FPlatformTime::Seconds() + 1.0;
	LastSelectionKey.Reset();
	bSelectionDirty = false;
	const TSharedRef<FJsonObject> Step = GameTestEditorRecorderDetail::MakeStep(TEXT("editor.open"), TEXT("event"), FPaths::GetBaseFilename(PackageName));
	Step->SetStringField(TEXT("map"), PackageName);
	AddStep(Step, /*bInSemantic=*/true);
}

void FGameTestEditorRecorder::HandleAssetOpened(UObject* InAsset)
{
	// 关卡由 OnMapOpened 录；临时资源（预览、PIE 里的）不录
	if (!bRecording || Cast<UWorld>(InAsset) != nullptr || !GameTestEditorRefs::IsRecordable(InAsset))
	{
		return;
	}
	const TSharedRef<FJsonObject> Step = GameTestEditorRecorderDetail::MakeStep(TEXT("editor.open"), TEXT("event"), GameTestEditorRefs::Describe(InAsset));
	Step->SetStringField(TEXT("asset"), InAsset->GetPathName());
	AddStep(Step, /*bInSemantic=*/true);
}

void FGameTestEditorRecorder::HandlePreChange(UObject* InObject, const FEditPropertyChain& InChain)
{
	if (!bRecording || !GameTestEditorRefs::IsRecordable(InObject))
	{
		return;
	}
	const auto* Node = InChain.GetActiveMemberNode();
	FProperty* Member = Node != nullptr ? Node->GetValue() : nullptr;
	const UClass* Owner = Member != nullptr ? Member->GetOwnerClass() : nullptr;
	if (Owner == nullptr || !InObject->GetClass()->IsChildOf(Owner))
	{
		return;
	}
	PreValue.Object = InObject;
	PreValue.Member = Member->GetFName();
	PreValue.Value = AgentPanel::ToJson(Member, Member->ContainerPtrToValuePtr<void>(InObject), 2000);
}

void FGameTestEditorRecorder::HandlePropertyChanged(UObject* InObject, const FPropertyChangedEvent& InEvent)
{
	// 拖滑条时的中间值（Interactive）不录，只录松手后的那一下
	if (!bRecording || (InEvent.ChangeType & EPropertyChangeType::Interactive) != 0 || IsSuppressed(InObject))
	{
		return;
	}
	const FString Ref = GameTestEditorRefs::MakeRef(InObject);
	const FString Path = GameTestEditorRecorderDetail::PropertyPath(InEvent);
	if (Ref.IsEmpty() || Path.IsEmpty())
	{
		return;
	}
	AgentPanel::FResolved Resolved;
	FString Error;
	if (!AgentPanel::ResolvePath(InObject->GetClass(), InObject, InObject, Path, Resolved, Error))
	{
		return;
	}
	const TSharedPtr<FJsonValue> Value = AgentPanel::ToJson(Resolved.Prop, Resolved.Value, 2000);

	// 改之前的值：路径是成员本身就是整段，是结构体里的一项就从整段里取
	TSharedPtr<FJsonValue> From;
	FString MemberName = Path;
	FString Child;
	Path.Split(TEXT("."), &MemberName, &Child);
	if (PreValue.Object.Get() == InObject && PreValue.Member == FName(*MemberName) && PreValue.Value.IsValid())
	{
		if (Child.IsEmpty())
		{
			From = PreValue.Value;
		}
		else if (PreValue.Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Whole = PreValue.Value->AsObject();
			From = Whole.IsValid() ? Whole->TryGetField(Child) : nullptr;
		}
	}

	// 连着改同一个属性（细节面板里一个数字改了几遍）合成一步，起点保留第一次的
	if (Steps.Num() > 0)
	{
		const TSharedRef<FJsonObject>& Last = Steps.Last();
		FString LastAction;
		FString LastObject;
		FString LastPath;
		GameTestJson::TryGetString(*Last, TEXT("action"), LastAction);
		GameTestJson::TryGetString(*Last, TEXT("object"), LastObject);
		GameTestJson::TryGetString(*Last, TEXT("path"), LastPath);
		if (LastAction == TEXT("editor.prop") && LastObject == Ref && LastPath == Path)
		{
			Last->SetField(TEXT("value"), Value);
			LastSemanticAt = FPlatformTime::Seconds();
			Broadcast(TEXT("step"));
			return;
		}
	}

	const TSharedRef<FJsonObject> Step = GameTestEditorRecorderDetail::MakeStep(TEXT("editor.prop"), TEXT("event"), GameTestEditorRefs::Describe(InObject));
	Step->SetStringField(TEXT("object"), Ref);
	Step->SetStringField(TEXT("path"), Path);
	Step->SetField(TEXT("value"), Value);
	if (From.IsValid())
	{
		const TSharedPtr<FJsonObject> Hint = GameTestJson::TryGetObject(*Step, TEXT("hint"));
		if (Hint.IsValid())
		{
			Hint->SetField(TEXT("from"), From);
		}
	}
	AddStep(Step, /*bInSemantic=*/true);
}

void FGameTestEditorRecorder::HandleActorAdded(AActor* InActor)
{
	if (!bRecording || !IsValid(InActor) || GIsTransacting || FPlatformTime::Seconds() < SuppressUntil)
	{
		return;
	}
	// 只录编辑器关卡里的：PIE 里生成的、预览场景里的不算
	if (InActor->GetWorld() == nullptr || InActor->GetWorld() != GameTestEditorRefs::GetEditorWorld() || !GameTestEditorRefs::IsRecordable(InActor))
	{
		return;
	}
	// 下一帧再写：拖进视口时标签、位置是放好之后才设的
	PendingSpawns.Add(InActor);
	LastSemanticAt = FPlatformTime::Seconds();
}

void FGameTestEditorRecorder::HandlePreCompile(UBlueprint* InBlueprint)
{
	if (!bRecording || !GameTestEditorRefs::IsRecordable(InBlueprint))
	{
		return;
	}
	// 一次编译可能广播几遍（编译依赖它的蓝图、保存时自动编译）：同一个蓝图两秒内只录一次
	const FString Path = InBlueprint->GetPathName();
	const double Now = FPlatformTime::Seconds();
	const double* Previous = RecentCompiles.Find(Path);
	if (Previous != nullptr && Now - *Previous < 2.0)
	{
		return;
	}
	RecentCompiles.Add(Path, Now);
	const TSharedRef<FJsonObject> Step = GameTestEditorRecorderDetail::MakeStep(TEXT("editor.compile"), TEXT("event"), InBlueprint->GetName());
	Step->SetStringField(TEXT("asset"), Path);
	AddStep(Step, /*bInSemantic=*/true);
}

void FGameTestEditorRecorder::HandlePackageSaved(const FString& InFilename, UPackage* InPackage)
{
	// 自动保存写在 Saved/Autosaves 下，不是人点的保存
	if (!bRecording || InPackage == nullptr || InFilename.Contains(TEXT("Autosaves")) || !GameTestEditorRefs::IsRecordable(InPackage))
	{
		return;
	}
	PendingSaves.AddUnique(InPackage->GetName());
	LastSemanticAt = FPlatformTime::Seconds();
}

//~ 按帧收尾 ----------------------------------------------------------------------

void FGameTestEditorRecorder::OnTick()
{
	if (!bRecording)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	CheckModal();
	FlushSpawns();
	FlushSelection(Now);
	FlushSaves();
	FlushPendingUi(Now);
}

void FGameTestEditorRecorder::FlushSpawns()
{
	const double Now = FPlatformTime::Seconds();
	for (const TWeakObjectPtr<AActor>& Weak : PendingSpawns)
	{
		AActor* Actor = Weak.Get();
		if (!IsValid(Actor))
		{
			continue;
		}
		UClass* Class = Actor->GetClass();
		const UBlueprint* Blueprint = UBlueprint::GetBlueprintFromClass(Class);
		const FString Label = Actor->GetActorLabel();
		const TSharedRef<FJsonObject> Step = GameTestEditorRecorderDetail::MakeStep(TEXT("editor.spawn"), TEXT("event"), Label);
		// 蓝图写资源路径（UELink 的 editor.spawn 认 /Game/BP/BP_Enemy 这种写法），原生类写类名
		Step->SetStringField(TEXT("class"), Blueprint != nullptr ? GameTestEditorRefs::ToPackageName(Blueprint->GetPathName()) : Class->GetName());
		const FVector Location = Actor->GetActorLocation();
		const FRotator Rotation = Actor->GetActorRotation();
		Step->SetArrayField(TEXT("location"), GameTestEditorRecorderDetail::Vector3(Location.X, Location.Y, Location.Z));
		if (!Rotation.IsNearlyZero())
		{
			Step->SetArrayField(TEXT("rotation"), GameTestEditorRecorderDetail::Vector3(Rotation.Pitch, Rotation.Yaw, Rotation.Roll));
		}
		Step->SetStringField(TEXT("label"), Label);
		RecentSpawns.Add(Label, Now);
		AddStep(Step, /*bInSemantic=*/true);
	}
	PendingSpawns.Reset();
}

void FGameTestEditorRecorder::FlushSelection(double InNow)
{
	// 选中事件一次点击会来好几次（先清空再选上）：静下来 0.15 秒再看最终选中了谁
	if (!bSelectionDirty || InNow - SelectionDirtyAt < 0.15 || GEditor == nullptr)
	{
		return;
	}
	bSelectionDirty = false;
	if (FPlatformTime::Seconds() < SuppressUntil)
	{
		return;
	}
	TArray<FString> Refs;
	TArray<FString> Labels;
	bool bOnlySpawned = true;
	if (USelection* Selection = GEditor->GetSelectedActors())
	{
		for (FSelectionIterator It(*Selection); It; ++It)
		{
			const FString Ref = GameTestEditorRefs::MakeRef(*It);
			if (Ref.IsEmpty())
			{
				continue;
			}
			Refs.Add(Ref);
			const FString Label = GameTestEditorRefs::Describe(*It);
			Labels.Add(Label);
			const double* SpawnedAt = RecentSpawns.Find(Label);
			bOnlySpawned = bOnlySpawned && SpawnedAt != nullptr && InNow - *SpawnedAt < GameTestEditorRecorderDetail::SpawnEchoSeconds;
		}
	}
	const FString Key = FString::Join(Refs, TEXT("|"));
	const bool bSame = Key == LastSelectionKey;
	LastSelectionKey = Key;
	// 空选中（点空白处取消选择）回放没有意义；刚放的 Actor 被引擎自动选中也不算
	if (bSame || Refs.Num() == 0 || bOnlySpawned)
	{
		return;
	}
	const TSharedRef<FJsonObject> Step = GameTestEditorRecorderDetail::MakeStep(TEXT("editor.select"), TEXT("event"), FString::Join(Labels, TEXT("、")));
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& Ref : Refs)
	{
		Values.Add(MakeShared<FJsonValueString>(Ref));
	}
	Step->SetArrayField(TEXT("actors"), Values);
	AddStep(Step, /*bInSemantic=*/true);
}

void FGameTestEditorRecorder::FlushSaves()
{
	if (PendingSaves.Num() == 0)
	{
		return;
	}
	TArray<TSharedPtr<FJsonValue>> Values;
	TArray<FString> Names;
	for (const FString& Package : PendingSaves)
	{
		Values.Add(MakeShared<FJsonValueString>(Package));
		Names.Add(FPaths::GetBaseFilename(Package));
	}
	PendingSaves.Reset();
	const TSharedRef<FJsonObject> Step = GameTestEditorRecorderDetail::MakeStep(TEXT("editor.save"), TEXT("event"), FString::Join(Names, TEXT("、")));
	Step->SetArrayField(TEXT("assets"), Values);
	AddStep(Step, /*bInSemantic=*/true);
}

void FGameTestEditorRecorder::FlushPendingUi(double InNow)
{
	for (int32 Index = 0; Index < PendingUi.Num();)
	{
		const FPendingUi& Pending = PendingUi[Index];
		if (InNow - Pending.At < GameTestEditorRecorderDetail::UiSettleSeconds)
		{
			++Index;
			continue;
		}
		// 按下之后有了语义事件（点「编译」引出编译、Ctrl+S 引出保存）：语义步骤更稳，这一下不录
		if (LastSemanticAt < Pending.At && Pending.Step.IsValid())
		{
			AddStep(Pending.Step.ToSharedRef(), /*bInSemantic=*/false);
		}
		PendingUi.RemoveAt(Index);
	}
}

void FGameTestEditorRecorder::CheckModal()
{
	const TSharedPtr<SWindow> Modal = FSlateApplication::Get().GetActiveModalWindow();
	const FString Title = Modal.IsValid() ? Modal->GetTitle().ToString() : FString();
	if (Title == LastModalTitle)
	{
		return;
	}
	LastModalTitle = Title;
	if (Title.IsEmpty())
	{
		return;
	}
	// 模态框是嵌套消息循环：回放时 GameTest 的每帧回调能不能在里面跑还没在本地验证（定稿 10.1），先如实标出来
	const TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
	Step->SetStringField(TEXT("action"), TEXT("mark"));
	Step->SetStringField(TEXT("text"), FString::Printf(TEXT("这里弹出了模态框「%s」：回放到这里可能卡住，要在全局守护里加处理规则，或者改成不弹框的做法（例如 editor.save 代替另存为）"), *Title));
	AddStep(Step, /*bInSemantic=*/false);
}

//~ 输入 --------------------------------------------------------------------------

bool FGameTestEditorRecorder::OnMouseDown(const FVector2D& InScreenPosition, bool bInPick)
{
	if (!bRecording)
	{
		return false;
	}
	FSlateApplication& App = FSlateApplication::Get();
	const FWidgetPath Path = App.LocateWindowUnderMouse(InScreenPosition, App.GetInteractiveTopLevelWindows());
	if (!Path.IsValid() || GameTestSlate::IsInGameUi(Path.GetLastWidget(), Path.GetWindow()))
	{
		return false;
	}
	// 视口、细节面板、大纲、内容浏览器里的点击会引出选中、改属性这些事件，由事件录
	if (!bInPick && GameTestEditorSlateFind::IsSemanticArea(Path))
	{
		return false;
	}
	const TSharedPtr<SWidget> Widget = GameTestEditorSlateFind::FindClickable(Path);
	if (!Widget.IsValid())
	{
		return false;
	}
	FGameTestEditorWidgetSpec Spec;
	GameTestEditorSlateFind::Describe(Widget.ToSharedRef(), Path.GetWindow(), Spec);

	if (bInPick)
	{
		const TSharedRef<FJsonObject> Step = GameTestEditorRecorderDetail::MakeStep(TEXT("editor.assert"), TEXT("pick"), Spec.Describe());
		Spec.WriteTo(*Step);
		AddStep(Step, /*bInSemantic=*/false);
		// 吃掉这一下：拾取断言不该真的点下去
		return true;
	}

	const TSharedRef<FJsonObject> Step = GameTestEditorRecorderDetail::MakeStep(TEXT("editor.click"), TEXT("ui"), Spec.Describe());
	Spec.WriteTo(*Step);
	// 按钮上的字和某个命令对得上：面板上提示「换成命令更稳」（不受界面布局、语言影响）
	TArray<FString> Commands;
	GameTestEditorCommands::FindByLabel(Spec.Text, 3, Commands);
	if (Commands.Num() > 0)
	{
		const TSharedRef<FJsonObject> Suggest = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Names;
		for (const FString& Command : Commands)
		{
			Names.Add(MakeShared<FJsonValueString>(Command));
		}
		Suggest->SetArrayField(TEXT("commands"), Names);
		const FString Asset = GameTestEditorCommands::ActiveEditorAsset();
		if (!Asset.IsEmpty() && Spec.Tab.StartsWith(FPaths::GetBaseFilename(GameTestEditorRefs::ToPackageName(Asset))))
		{
			Suggest->SetStringField(TEXT("asset"), Asset);
		}
		Step->SetObjectField(TEXT("suggest"), Suggest);
	}
	FPendingUi Pending;
	Pending.Step = Step;
	Pending.At = FPlatformTime::Seconds();
	PendingUi.Add(Pending);
	return false;
}

void FGameTestEditorRecorder::OnKeyDown(const FKey& InKey, bool bInCtrl, bool bInAlt, bool bInShift, bool bInCmd)
{
	if (!bRecording || InKey.IsModifierKey() || InKey.IsMouseButton())
	{
		return;
	}
	// 只看组合键和 F 键：单个字母多半是在打字或视口里的移动键
	if (!bInCtrl && !bInAlt && !bInCmd && !GameTestEditorRecorderDetail::IsFunctionKey(InKey))
	{
		return;
	}
	if (GameTestEditorRecorderDetail::IsTyping() || GameTestSlate::IsInGameUi(FSlateApplication::Get().GetKeyboardFocusedWidget()))
	{
		return;
	}
	FString Command;
	if (!GameTestEditorCommands::FindByChord(InKey, bInCtrl, bInAlt, bInShift, bInCmd, Command))
	{
		return;
	}
	const FString Chord = GameTestEditorRecorderDetail::ChordText(InKey, bInCtrl, bInAlt, bInShift, bInCmd);
	const TSharedRef<FJsonObject> Step = GameTestEditorRecorderDetail::MakeStep(TEXT("editor.command"), TEXT("key"), Chord);
	Step->SetStringField(TEXT("name"), Command);
	// 关卡编辑器、通用命令之外的（资源编辑器自己的）要在那个资源的编辑器里执行
	if (!Command.StartsWith(TEXT("LevelEditor.")) && !Command.StartsWith(TEXT("MainFrame.")) && !Command.StartsWith(TEXT("GenericCommands.")))
	{
		const FString Asset = GameTestEditorCommands::ActiveEditorAsset();
		if (!Asset.IsEmpty())
		{
			Step->SetStringField(TEXT("asset"), Asset);
		}
	}
	FPendingUi Pending;
	Pending.Step = Step;
	Pending.At = FPlatformTime::Seconds();
	PendingUi.Add(Pending);
}
