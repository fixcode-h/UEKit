// Copyright Epic Games, Inc. All Rights Reserved.

#include "Record/GameTestRecordState.h"

#include "Core/GameTestAction.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestRunConfig.h"
#include "Core/GameTestTypes.h"
#include "Observe/GameTestCondition.h"
#include "Observe/GameTestItemKey.h"
#include "Observe/GameTestSelectorWriter.h"
#include "Observe/GameTestSlate.h"
#include "Observe/GameTestSnapshot.h"
#include "Observe/GameTestTextMatch.h"
#include "Observe/GameTestWidgetNode.h"
#include "Operate/GameTestSkipIf.h"
#include "Record/GameTestRecordShot.h"

#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"

namespace GameTestRecordStateDetail
{
	/** 往上找到这个控件所在的 WBP（控件自己就是 WBP 时是它自己），和 GameTestCondition::ReadField 认的是同一个。 */
	UUserWidget* OwnerOf(UWidget* InWidget)
	{
		if (InWidget == nullptr)
		{
			return nullptr;
		}
		if (UUserWidget* Self = Cast<UUserWidget>(InWidget))
		{
			return Self;
		}
		return InWidget->GetTypedOuter<UUserWidget>();
	}

	/** 最外层的 WBP：点的控件所在界面整棵读（勾、「已领取」的字常常不在按钮自己的 WBP 里）。 */
	UUserWidget* RootOf(UUserWidget* InOwner)
	{
		UUserWidget* Root = InOwner;
		for (int32 Guard = 0; Root != nullptr && Guard < 16; ++Guard)
		{
			UUserWidget* Up = Root->GetTypedOuter<UUserWidget>();
			if (Up == nullptr || Up == Root)
			{
				break;
			}
			Root = Up;
		}
		return Root;
	}

	/** `InChild` 在不在 `InAncestor` 里面（跨 WBP 也算：按钮是个 WBP 时它的字在它自己的控件树里）。 */
	bool IsInside(const UWidget* InChild, const UWidget* InAncestor)
	{
		const UWidget* Current = InChild;
		for (int32 Guard = 0; Current != nullptr && Guard < 64; ++Guard)
		{
			if (Current == InAncestor)
			{
				return true;
			}
			const UWidget* Parent = Current->GetParent();
			Current = Parent != nullptr ? Parent : static_cast<const UWidget*>(Current->GetTypedOuter<UUserWidget>());
		}
		return false;
	}

	/** 种类的先后：同分时更直接的证据排前面。 */
	int32 KindRank(const FString& InKind)
	{
		static const TArray<FString> Order = { TEXT("checked"), TEXT("text"), TEXT("field"), TEXT("visible"), TEXT("image"), TEXT("enabled") };
		const int32 Index = Order.IndexOfByKey(InKind);
		return Index == INDEX_NONE ? Order.Num() : Index;
	}

	const TCHAR* BoolText(bool bIn)
	{
		return bIn ? TEXT("true") : TEXT("false");
	}

	/** 值写给人看（候选列表里的「之前 → 之后」）。 */
	FString ValueText(const TSharedPtr<FJsonValue>& InValue)
	{
		if (!InValue.IsValid())
		{
			return FString();
		}
		const FString Formatted = GameTestItemKey::FormatValue(*InValue);
		if (!Formatted.IsEmpty())
		{
			return Formatted;
		}
		FString Text;
		return InValue->TryGetString(Text) ? Text : FString();
	}

	/** 选择器的屏名（`屏:路径` 的「屏」），句柄、不写屏名的返回空。 */
	FString ScreenOf(const FString& InTarget)
	{
		int32 Colon = INDEX_NONE;
		if (InTarget.StartsWith(TEXT("@")) || !InTarget.FindChar(TEXT(':'), Colon) || Colon <= 0)
		{
			return FString();
		}
		return InTarget.Left(Colon);
	}

	/** 类名、资产路径都归一成不带 `_C` 的短名比较。 */
	FString ShortClass(const FString& InName)
	{
		FString Name = InName;
		int32 Slash = INDEX_NONE;
		if (Name.FindLastChar(TEXT('/'), Slash))
		{
			Name.RightChopInline(Slash + 1);
		}
		int32 Dot = INDEX_NONE;
		if (Name.FindLastChar(TEXT('.'), Dot))
		{
			Name.RightChopInline(Dot + 1);
		}
		Name.RemoveFromEnd(TEXT("_C"));
		return Name;
	}

	bool ContainsScreen(const TArray<FString>& InScreens, const FString& InScreen)
	{
		const FString Want = ShortClass(InScreen);
		for (const FString& Screen : InScreens)
		{
			if (ShortClass(Screen).Equals(Want, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	/** 候选内部用：还没写选择器的控件。 */
	struct FRawCandidate
	{
		FGameTestStateCandidate Candidate;
		TWeakObjectPtr<UWidget> Widget;
	};

	TSharedPtr<FJsonObject> MakeCond()
	{
		return MakeShared<FJsonObject>();
	}
}

void FGameTestRecordState::Begin(EGameTestInferSkip InMode, int32 InWindowMs)
{
	End();
	bActive = true;
	Mode = InMode;
	WindowSeconds = FMath::Clamp(InWindowMs > 0 ? InWindowMs : DefaultWindowMs, 300, 10000) / 1000.0;
}

void FGameTestRecordState::End()
{
	bActive = false;
	Pressing.Reset();
	Watching.Reset();
	AutoSteps.Reset();
}

void FGameTestRecordState::OnUiPress(const TSharedPtr<SWidget>& InSlate)
{
	if (!IsEnabled())
	{
		return;
	}
	// 上一下还没看完：按现在的样子收尾（这一下还没松开，界面还没被它改）
	if (Watching.IsSet())
	{
		Conclude(Watching.GetValue(), /*bInTakeNow=*/true, nullptr, [](const TSharedPtr<FJsonObject>&) { return INDEX_NONE; });
		Watching.Reset();
	}
	Pressing.Reset();

	UWidget* Pressed = GameTestSlate::FindOwningWidget(InSlate);
	UUserWidget* Owner = GameTestRecordStateDetail::OwnerOf(Pressed);
	UUserWidget* Root = GameTestRecordStateDetail::RootOf(Owner);
	if (Pressed == nullptr || Root == nullptr)
	{
		return;
	}
	FPending Pending;
	Pending.Pressed = Pressed;
	Pending.Owner = Owner;
	Pending.Root = Root;
	Pending.ScreensBefore = FGameTestRecordShots::TopScreens();
	Pending.Before = Capture(Pending);
	Pressing = MoveTemp(Pending);
}

void FGameTestRecordState::OnClick(const TSharedRef<FJsonObject>& InStep)
{
	if (!Pressing.IsSet())
	{
		return;
	}
	FPending Pending = MoveTemp(Pressing.GetValue());
	Pressing.Reset();
	Pending.Step = InStep;
	Pending.ReleasedAt = FPlatformTime::Seconds();
	Watching = MoveTemp(Pending);
}

void FGameTestRecordState::OnCancel()
{
	Pressing.Reset();
}

FString FGameTestRecordState::Tick(double InNow, TFunctionRef<int32(const TSharedPtr<FJsonObject>&)> InStepIndex)
{
	if (!Watching.IsSet())
	{
		return FString();
	}
	FPending& Pending = Watching.GetValue();
	constexpr int32 NumShots = 3;
	static const double Fractions[NumShots] = { 0.15, 0.5, 1.0 };
	const int32 Taken = Pending.After.Num();
	// 一帧最多读一次：读一遍要采整个界面的控件树
	if (Taken < NumShots && InNow - Pending.ReleasedAt >= Fractions[Taken] * WindowSeconds)
	{
		Pending.After.Add(Capture(Pending));
	}
	if (Pending.After.Num() < NumShots)
	{
		return FString();
	}
	FString Notice;
	Conclude(Pending, /*bInTakeNow=*/false, &Notice, InStepIndex);
	Watching.Reset();
	return Notice;
}

void FGameTestRecordState::Finish(TArray<TSharedRef<FJsonObject>>& InOutSteps)
{
	if (Watching.IsSet())
	{
		Conclude(Watching.GetValue(), /*bInTakeNow=*/true, nullptr, [](const TSharedPtr<FJsonObject>&) { return INDEX_NONE; });
		Watching.Reset();
	}
	Pressing.Reset();

	TArray<FString> BlockerScreens;
	FGameTestRunConfig RunConfig;
	FString ConfigError;
	if (GameTestRunConfig::Load(RunConfig, ConfigError))
	{
		for (const FGameTestBlockerRule& Rule : RunConfig.Blockers)
		{
			if (Rule.bEnabled)
			{
				BlockerScreens.Add(Rule.Widget);
			}
		}
	}
	for (const TWeakPtr<FJsonObject>& Weak : AutoSteps)
	{
		const TSharedPtr<FJsonObject> Step = Weak.Pin();
		if (!Step.IsValid())
		{
			continue;
		}
		const int32 Index = InOutSteps.IndexOfByPredicate([&Step](const TSharedRef<FJsonObject>& InStep) { return &InStep.Get() == Step.Get(); });
		const TSharedPtr<FJsonObject> Hint = GameTestJson::TryGetObject(*Step, TEXT("hint"));
		const TSharedPtr<FJsonObject> SkipIf = GameTestJson::TryGetObject(*Step, TEXT("skipIf"));
		const TArray<TSharedPtr<FJsonValue>>* OpenedValues = nullptr;
		if (Index == INDEX_NONE || !Hint.IsValid() || !SkipIf.IsValid() || !Hint->TryGetArrayField(TEXT("opened"), OpenedValues) || OpenedValues == nullptr)
		{
			continue;
		}
		TArray<FString> Opened;
		for (const TSharedPtr<FJsonValue>& Value : *OpenedValues)
		{
			FString Screen;
			if (Value.IsValid() && Value->TryGetString(Screen))
			{
				Opened.Add(Screen);
			}
		}
		const int32 Also = ComputeAlso(InOutSteps, Index, Opened, BlockerScreens);
		if (Also > 0)
		{
			SkipIf->SetNumberField(TEXT("also"), Also);
		}
	}
	AutoSteps.Reset();
}

bool FGameTestRecordState::IsPurposeName(const FString& InName)
{
	static const TArray<FString> Words = {
		TEXT("check"), TEXT("tick"), TEXT("mark"), TEXT("got"), TEXT("done"), TEXT("claim"), TEXT("receiv"),
		TEXT("finish"), TEXT("complet"), TEXT("select"), TEXT("reddot"), TEXT("red_dot"), TEXT("owned"),
		TEXT("已"), TEXT("勾"), TEXT("领"),
	};
	for (const FString& Word : Words)
	{
		if (InName.Contains(Word, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

bool FGameTestRecordState::IsMeaningfulTextChange(const FString& InBefore, const FString& InAfter)
{
	const FString After = InAfter.TrimStartAndEnd();
	if (After.IsEmpty() || After.Len() > 30)
	{
		return false;
	}
	const FString NormalAfter = GameTestTextMatch::Normalize(After);
	// 只剩数字和标点（计数、倒计时）、或者只是数字变了：回放时本来就会不一样
	return !NormalAfter.IsEmpty() && NormalAfter != GameTestTextMatch::Normalize(InBefore);
}

int32 FGameTestRecordState::ComputeAlso(const TArray<TSharedRef<FJsonObject>>& InSteps, int32 InIndex, const TArray<FString>& InOpened,
	const TArray<FString>& InBlockerScreens)
{
	TArray<FString> Opened;
	for (const FString& Screen : InOpened)
	{
		if (!GameTestRecordStateDetail::ContainsScreen(InBlockerScreens, Screen))
		{
			Opened.Add(Screen);
		}
	}
	if (Opened.Num() == 0 || !InSteps.IsValidIndex(InIndex))
	{
		return 0;
	}
	int32 Also = 0;
	for (int32 Index = InIndex + 1; Index < InSteps.Num() && Index - InIndex <= GameTestSkipIf::MaxAlso; ++Index)
	{
		FString Action;
		InSteps[Index]->TryGetStringField(TEXT("action"), Action);
		if (Action == TEXT("wait"))
		{
			continue;
		}
		FString Target;
		InSteps[Index]->TryGetStringField(TEXT("target"), Target);
		const FString Screen = GameTestRecordStateDetail::ScreenOf(Target);
		if (Screen.IsEmpty() || !GameTestRecordStateDetail::ContainsScreen(Opened, Screen))
		{
			break;
		}
		Also = Index - InIndex;
	}
	return Also;
}

int32 FGameTestRecordState::SortAndPick(TArray<FGameTestStateCandidate>& InOutCandidates)
{
	InOutCandidates.StableSort([](const FGameTestStateCandidate& A, const FGameTestStateCandidate& B) {
		if (A.Score != B.Score)
		{
			return A.Score > B.Score;
		}
		return GameTestRecordStateDetail::KindRank(A.Kind) < GameTestRecordStateDetail::KindRank(B.Kind);
	});
	if (InOutCandidates.Num() > MaxCandidates)
	{
		InOutCandidates.SetNum(MaxCandidates);
	}
	return InOutCandidates.Num() > 0 && InOutCandidates[0].Score >= AutoScore ? 0 : INDEX_NONE;
}

FGameTestRecordState::FShot FGameTestRecordState::Capture(const FPending& InPending) const
{
	FShot Shot;
	UUserWidget* Root = InPending.Root.Get();
	if (!IsValid(Root))
	{
		return Shot;
	}
	// 嵌套的 WBP 在快照里只记引用不展开，这里逐个展开（行、按钮常常各自是一个 WBP）
	TArray<UUserWidget*> Screens = { Root };
	int32 Count = 0;
	for (int32 ScreenIndex = 0; ScreenIndex < Screens.Num() && Count < MaxNodes; ++ScreenIndex)
	{
		const TSharedPtr<FGameTestWidgetNode> Tree = GameTestSnapshot::CaptureSubtree(Screens[ScreenIndex], FString(), /*InIsScreenRoot=*/true, /*bInLight=*/true);
		if (!Tree.IsValid())
		{
			continue;
		}
		TArray<const FGameTestWidgetNode*> Stack = { Tree.Get() };
		while (Stack.Num() > 0 && Count < MaxNodes)
		{
			const FGameTestWidgetNode* Node = Stack.Pop();
			for (const TSharedPtr<FGameTestWidgetNode>& Child : Node->Children)
			{
				if (Child.IsValid())
				{
					Stack.Add(Child.Get());
				}
			}
			UWidget* Widget = Node->Widget.Get();
			if (Widget == nullptr)
			{
				continue;
			}
			if (Node->bIsUserWidget && Widget != Screens[ScreenIndex])
			{
				if (UUserWidget* Nested = Cast<UUserWidget>(Widget))
				{
					Screens.AddUnique(Nested);
				}
			}
			FWidgetState State;
			State.Widget = Widget;
			State.Name = Node->Name;
			State.bVisible = Node->Visibility != TEXT("Collapsed") && Node->Visibility != TEXT("Hidden");
			State.bEnabled = Node->bEnabled;
			bool bChecked = false;
			State.Checked = GameTestCondition::ReadChecked(*Node, bChecked) ? (bChecked ? 1 : 0) : -1;
			State.Text = Node->Text;
			State.Image = GameTestCondition::ReadImage(Widget);
			Shot.Widgets.Add(TObjectKey<UWidget>(Widget), MoveTemp(State));
			++Count;
		}
	}

	// 点的控件所在 WBP 的字段：勾是存在字段里、界面自己画的项目，只看得到字段变了
	if (UUserWidget* Owner = InPending.Owner.Get())
	{
		FGameTestKeyCache Cache;
		const TPair<const TCHAR*, TSharedPtr<FJsonObject>> Sources[] = {
			{ TEXT("@self."), GameTestItemKey::DumpFields(Owner, Cache) },
			{ TEXT("@lua."), GameTestItemKey::DumpLuaFields(Owner, Cache) },
		};
		for (const TPair<const TCHAR*, TSharedPtr<FJsonObject>>& Source : Sources)
		{
			if (!Source.Value.IsValid())
			{
				continue;
			}
			TArray<TPair<FString, TSharedPtr<FJsonValue>>> Leaves;
			GameTestItemKey::Flatten(*Source.Value, 3, GameTestItemKey::MaxFieldsPerSource, Leaves);
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Leaf : Leaves)
			{
				Shot.Fields.Add(FString(Source.Key) + Leaf.Key, Leaf.Value);
			}
		}
	}
	Shot.bValid = true;
	return Shot;
}

void FGameTestRecordState::Diff(const FPending& InPending, const FShot& InFinal, TArray<FGameTestStateCandidate>& OutCandidates) const
{
	using namespace GameTestRecordStateDetail;
	const UWidget* Pressed = InPending.Pressed.Get();
	TArray<FRawCandidate> Raw;
	auto Add = [&Raw](UWidget* InWidget, const TCHAR* InKind, const TSharedPtr<FJsonObject>& InCond, const FString& InBefore, const FString& InAfter, int32 InScore) {
		if (InScore < MinScore)
		{
			return;
		}
		FRawCandidate Item;
		Item.Widget = InWidget;
		Item.Candidate.Kind = InKind;
		Item.Candidate.Cond = InCond;
		Item.Candidate.Before = InBefore;
		Item.Candidate.After = InAfter;
		Item.Candidate.Score = InScore;
		Raw.Add(MoveTemp(Item));
	};

	for (const TPair<TObjectKey<UWidget>, FWidgetState>& Pair : InPending.Before.Widgets)
	{
		const FWidgetState* After = InFinal.Widgets.Find(Pair.Key);
		UWidget* Widget = Pair.Value.Widget.Get();
		if (After == nullptr || Widget == nullptr)
		{
			continue;
		}
		const FWidgetState& Before = Pair.Value;
		const bool bSelf = Widget == Pressed;
		const bool bLabel = !bSelf && Pressed != nullptr && IsInside(Widget, Pressed);
		const bool bPurpose = IsPurposeName(Before.Name) || IsPurposeName(After->Image);

		if (Before.Checked >= 0 && After->Checked >= 0 && Before.Checked != After->Checked)
		{
			const TSharedPtr<FJsonObject> Cond = MakeCond();
			Cond->SetBoolField(TEXT("checked"), After->Checked == 1);
			Add(Widget, TEXT("checked"), Cond, BoolText(Before.Checked == 1), BoolText(After->Checked == 1), bSelf || bLabel || bPurpose ? 10 : 8);
		}
		if (Before.Text != After->Text && IsMeaningfulTextChange(Before.Text, After->Text))
		{
			const TSharedPtr<FJsonObject> Cond = MakeCond();
			Cond->SetStringField(TEXT("textLike"), After->Text.TrimStartAndEnd());
			// 按钮上的字（「领取」→「已领取」）：条件挂在按钮自己身上，回放时和点击认的是同一个控件
			Add(bLabel ? const_cast<UWidget*>(Pressed) : Widget, TEXT("text"), Cond, Before.Text, After->Text,
				bSelf || bLabel ? 8 : (bPurpose ? 6 : 5));
		}
		if (Before.bVisible != After->bVisible)
		{
			const TSharedPtr<FJsonObject> Cond = MakeCond();
			Cond->SetBoolField(TEXT("visible"), After->bVisible);
			Add(Widget, TEXT("visible"), Cond, BoolText(Before.bVisible), BoolText(After->bVisible), bPurpose ? 7 : (bSelf ? 6 : 4));
		}
		if (Before.bEnabled != After->bEnabled)
		{
			const TSharedPtr<FJsonObject> Cond = MakeCond();
			Cond->SetBoolField(TEXT("enabled"), After->bEnabled);
			Add(Widget, TEXT("enabled"), Cond, BoolText(Before.bEnabled), BoolText(After->bEnabled), !After->bEnabled && (bSelf || bLabel) ? 6 : 4);
		}
		if (!Before.Image.IsEmpty() && !After->Image.IsEmpty() && Before.Image != After->Image)
		{
			const TSharedPtr<FJsonObject> Cond = MakeCond();
			Cond->SetStringField(TEXT("image"), After->Image);
			Add(Widget, TEXT("image"), Cond, Before.Image, After->Image, bPurpose || IsPurposeName(After->Image) ? 7 : 5);
		}
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : InPending.Before.Fields)
	{
		const TSharedPtr<FJsonValue>* After = InFinal.Fields.Find(Pair.Key);
		if (After == nullptr || !After->IsValid() || !Pair.Value.IsValid())
		{
			continue;
		}
		const FString BeforeText = ValueText(Pair.Value);
		const FString AfterText = ValueText(*After);
		if (BeforeText == AfterText || AfterText.IsEmpty())
		{
			continue;
		}
		int32 Score = 4;
		if ((*After)->Type == EJson::Boolean)
		{
			Score = IsPurposeName(Pair.Key) ? 8 : 6;
		}
		else if ((*After)->Type == EJson::String && !IsMeaningfulTextChange(BeforeText, AfterText))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Cond = MakeCond();
		Cond->SetStringField(TEXT("field"), Pair.Key);
		Cond->SetField(TEXT("equals"), *After);
		// 字段读的是点的控件所在的 WBP：条件挂在这一步自己的目标上
		Add(const_cast<UWidget*>(Pressed), TEXT("field"), Cond, BeforeText, AfterText, Score);
	}

	// 先按分排好，只给排在前面的写选择器（写选择器要核对解析，不为一堆低分的变化做）
	Raw.StableSort([](const FRawCandidate& A, const FRawCandidate& B) {
		return A.Candidate.Score != B.Candidate.Score ? A.Candidate.Score > B.Candidate.Score
			: KindRank(A.Candidate.Kind) < KindRank(B.Candidate.Kind);
	});
	for (FRawCandidate& Item : Raw)
	{
		if (OutCandidates.Num() >= MaxCandidates)
		{
			break;
		}
		UWidget* Widget = Item.Widget.Get();
		if (Widget == nullptr)
		{
			continue;
		}
		if (Widget != Pressed)
		{
			FGameTestWrittenTarget Written;
			FString Error;
			if (!GameTestSelectorWriter::Write(*Widget, Written, Error) || !Written.bVerified || Written.Target.IsEmpty())
			{
				continue;
			}
			Item.Candidate.Target = Written.Target;
		}
		OutCandidates.Add(MoveTemp(Item.Candidate));
	}
}

bool FGameTestRecordState::VerifyAuto(const TSharedPtr<FJsonObject>& InStep, const FGameTestStateCandidate& InCandidate) const
{
	if (!InStep.IsValid() || !InCandidate.Cond.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> SkipIf = MakeShared<FJsonObject>();
	SkipIf->Values = InCandidate.Cond->Values;
	if (!InCandidate.Target.IsEmpty())
	{
		SkipIf->SetStringField(TEXT("target"), InCandidate.Target);
	}
	SkipIf->SetStringField(TEXT("by"), TEXT("auto"));
	const TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
	Copy->Values = InStep->Values;
	Copy->SetObjectField(TEXT("skipIf"), SkipIf);

	// 用回放同一套解析和判断核对一遍：此刻（刚点完）条件应当成立，不成立说明选择器或读法回放时用不上，不自动写
	FGameTestStep Step;
	TArray<FString> Errors;
	if (!GameTestActions::ParseStep(Copy, 0, Step, Errors))
	{
		return false;
	}
	FString Actual;
	if (!GameTestSkipIf::Holds(*SkipIf, Step, nullptr, Actual))
	{
		return false;
	}
	InStep->SetObjectField(TEXT("skipIf"), SkipIf);
	return true;
}

void FGameTestRecordState::Conclude(FPending& InPending, bool bInTakeNow, FString* OutNotice,
	TFunctionRef<int32(const TSharedPtr<FJsonObject>&)> InStepIndex)
{
	const TSharedPtr<FJsonObject> Step = InPending.Step;
	if (!Step.IsValid() || !InPending.Before.bValid)
	{
		return;
	}
	const FShot Final = bInTakeNow || InPending.After.Num() == 0 ? Capture(InPending) : InPending.After.Last();
	if (!Final.bValid)
	{
		return;
	}
	TSharedPtr<FJsonObject> Hint = GameTestJson::TryGetObject(*Step, TEXT("hint"));
	if (!Hint.IsValid())
	{
		Hint = MakeShared<FJsonObject>();
	}

	// 点完打开了什么界面（开了截图时截图那边已经写过）：停止时算连带跳过用
	if (!Hint->HasField(TEXT("opened")))
	{
		TArray<TSharedPtr<FJsonValue>> Opened;
		for (const FString& Screen : FGameTestRecordShots::TopScreens())
		{
			if (!InPending.ScreensBefore.Contains(Screen))
			{
				Opened.Add(MakeShared<FJsonValueString>(Screen));
			}
		}
		if (Opened.Num() > 0)
		{
			Hint->SetArrayField(TEXT("opened"), Opened);
		}
	}

	TArray<FGameTestStateCandidate> Candidates;
	Diff(InPending, Final, Candidates);
	if (Candidates.Num() > 0)
	{
		const int32 Pick = SortAndPick(Candidates);
		int32 Picked = INDEX_NONE;
		if (Mode == EGameTestInferSkip::Auto && Pick != INDEX_NONE && !Step->HasField(TEXT("skipIf")) && VerifyAuto(Step, Candidates[Pick]))
		{
			Picked = Pick;
			AutoSteps.Add(Step);
		}

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FGameTestStateCandidate& Candidate : Candidates)
		{
			const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			if (!Candidate.Target.IsEmpty())
			{
				Item->SetStringField(TEXT("target"), Candidate.Target);
			}
			Item->SetStringField(TEXT("kind"), Candidate.Kind);
			Item->SetObjectField(TEXT("cond"), Candidate.Cond);
			Item->SetStringField(TEXT("before"), Candidate.Before);
			Item->SetStringField(TEXT("after"), Candidate.After);
			Item->SetNumberField(TEXT("score"), Candidate.Score);
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetArrayField(TEXT("cands"), Items);
		if (Picked != INDEX_NONE)
		{
			State->SetNumberField(TEXT("picked"), Picked);
			if (OutNotice != nullptr)
			{
				const int32 Index = InStepIndex(Step);
				const FString Say = GameTestCondition::Describe(*Candidates[Picked].Cond);
				*OutNotice = Index != INDEX_NONE
					? FString::Printf(TEXT("第 %d 步：已达成就跳过（%s）"), Index + 1, *Say)
					: FString::Printf(TEXT("已达成就跳过（%s）"), *Say);
			}
		}
		Hint->SetObjectField(TEXT("state"), State);
	}
	if (Hint->Values.Num() > 0)
	{
		Step->SetObjectField(TEXT("hint"), Hint);
	}
}
