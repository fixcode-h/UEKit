// Copyright Epic Games, Inc. All Rights Reserved.

// 录制器的「录移动」和快捷操作：从 GameTestRecorder.cpp 拆出来，那个文件只留入口调用。
// 录移动期间只记角色的轨迹，别的操作不写步骤；结束时写一条 move（回放见 Operate/GameTestMove.cpp）。

#include "Record/GameTestRecorder.h"
#include "Core/GameTestClock.h"

#include "Core/GameTestTrajectory.h"
#include "Core/GameTestWorld.h"
#include "Observe/GameTestNav.h"
#include "Operate/GameTestMove.h"
#include "Record/GameTestRecordMove.h"
#include "Record/GameTestRecordToolbar.h"
#include "GameTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"

namespace GameTestRecorderMoveDetail
{
	/** 离上一个记下的点超过这么远（厘米）才记新点。 */
	constexpr double SampleSpacing = 50.0;

	/** 简化轨迹：偏离连线不到 30 厘米的点去掉，最多留 300 个点。 */
	constexpr double SimplifyTolerance = 30.0;
	constexpr int32 MaxPathPoints = 300;

	/** 「在这附近」默认半径（厘米）。 */
	constexpr double DefaultNearRadius = 200.0;

	/** 提示显示多久（秒）。 */
	constexpr double NoticeSeconds = 6.0;

	TSharedRef<FJsonObject> MakeStep(const TCHAR* InAction)
	{
		const TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("action"), InAction);
		return Step;
	}

	bool IsHotkeyModifier(const FName& InKey)
	{
		return InKey == EKeys::LeftAlt.GetFName() || InKey == EKeys::RightAlt.GetFName()
			|| InKey == EKeys::LeftControl.GetFName() || InKey == EKeys::RightControl.GetFName();
	}

	/** 地图路径（PIE 的 UEDPIE_0_ 前缀去掉，和场景里 pie.start 的写法一致）。 */
	FString MapOf(const UWorld& InWorld)
	{
		return UWorld::RemovePIEPrefix(InWorld.GetOutermost()->GetName());
	}

	/** 本地玩家角色和控制器的朝向写进步骤：到了以后镜头转回来，后面点游戏画面的步骤才对得准。 */
	void SetFacing(FJsonObject& InStep, const APawn& InPawn)
	{
		const AController* Controller = InPawn.GetController();
		const FRotator Rotation = Controller != nullptr ? Controller->GetControlRotation() : InPawn.GetActorRotation();
		InStep.SetNumberField(TEXT("yaw"), FMath::RoundToDouble(FRotator::NormalizeAxis(Rotation.Yaw) * 10.0) / 10.0);
		InStep.SetNumberField(TEXT("pitch"), FMath::RoundToDouble(FRotator::NormalizeAxis(Rotation.Pitch) * 10.0) / 10.0);
	}
}

//~ 移动段的采样 ------------------------------------------------------------------

FGameTestMoveSegment::EMode FGameTestMoveSegment::ModeOf(const APawn& InPawn)
{
	const UCharacterMovementComponent* Movement = Cast<UCharacterMovementComponent>(InPawn.GetMovementComponent());
	if (Movement == nullptr)
	{
		return EMode::Walk;
	}
	if (Movement->IsSwimming() || Movement->IsFlying())
	{
		return EMode::Fly;
	}
	return Movement->IsFalling() ? EMode::Fall : EMode::Walk;
}

bool FGameTestMoveSegment::Begin(APawn& InPawn, double InNow)
{
	Pawn = &InPawn;
	World = InPawn.GetWorld();
	StartedAt = InNow;
	Start = InPawn.GetActorLocation();
	Points.Reset();
	Modes.Reset();
	Jumps.Reset();
	Meters = 0.0;
	Points.Add(Start);
	Modes.Add(ModeOf(InPawn));
	return World.IsValid();
}

bool FGameTestMoveSegment::Sample(FString& OutWhy)
{
	APawn* PawnPtr = Pawn.Get();
	UWorld* Current = GameTestWorld::FindGameWorld();
	if (PawnPtr == nullptr || !World.IsValid() || Current == nullptr)
	{
		OutWhy = TEXT("角色或游戏世界没了，这段移动作废");
		return false;
	}
	if (PawnPtr->GetWorld() != World.Get())
	{
		OutWhy = TEXT("地图换了，这段移动不能录，已放弃");
		return false;
	}
	APlayerController* Controller = World->GetFirstPlayerController();
	if (Controller == nullptr || Controller->GetPawn() != PawnPtr)
	{
		OutWhy = TEXT("角色换了（上车、变身或重生），这段移动不能录，已放弃");
		return false;
	}
	if (Controller->bCinematicMode)
	{
		OutWhy = TEXT("进了过场动画，位移不是角色自己走的，这段移动已放弃");
		return false;
	}

	const FVector Location = PawnPtr->GetActorLocation();
	const EMode Mode = ModeOf(*PawnPtr);
	const EMode LastMode = Modes.Num() > 0 ? Modes.Last() : EMode::Walk;
	// 起跳：从站在地上变成下落、而且往上走。跳之前的那个点就是起跳点。
	const bool bJumped = LastMode == EMode::Walk && Mode == EMode::Fall && PawnPtr->GetVelocity().Z > 50.0;
	const bool bFar = Points.Num() == 0 || FVector::Dist(Points.Last(), Location) >= GameTestRecorderMoveDetail::SampleSpacing;
	if (bJumped && Points.Num() > 0)
	{
		Jumps.Add(Points.Num() - 1);
	}
	if (bFar || Mode != LastMode)
	{
		if (Points.Num() > 0)
		{
			Meters += FVector::Dist(Points.Last(), Location) / 100.0;
		}
		Points.Add(Location);
		Modes.Add(Mode);
	}
	return true;
}

//~ 录制器的快捷操作 --------------------------------------------------------------

bool FGameTestRecorder::Quick(const FString& InPhase, double InRadius, FString& OutError)
{
	if (!bRecording)
	{
		OutError = TEXT("没有在录制：先 gt.record.start");
		return false;
	}
	// 先把排队的事件处理掉：步骤顺序和人操作的顺序一致。
	OnTick();

	bool bOk = false;
	if (InPhase == TEXT("start"))
	{
		bOk = BeginMove(OutError);
	}
	else if (InPhase == TEXT("end"))
	{
		bOk = EndMove(/*bInAuto=*/false, OutError);
	}
	else if (InPhase == TEXT("toggle"))
	{
		bOk = MoveSegment.IsValid() ? EndMove(/*bInAuto=*/false, OutError) : BeginMove(OutError);
	}
	else if (InPhase == TEXT("cancel"))
	{
		if (!MoveSegment.IsValid())
		{
			OutError = TEXT("没有在录移动");
			return false;
		}
		CancelMove(TEXT("已放弃这段移动"));
		bOk = true;
	}
	else if (InPhase == TEXT("here"))
	{
		bOk = AddHereStep(OutError);
	}
	else if (InPhase == TEXT("near"))
	{
		bOk = AddNearStep(InRadius > 0.0 ? InRadius : GameTestRecorderMoveDetail::DefaultNearRadius, OutError);
	}
	else if (InPhase == TEXT("pick") || InPhase == TEXT("wait") || InPhase == TEXT("branch"))
	{
		// 再按一次同一个就取消；按了别的就换成别的（下一次点击只录一种）
		const EPick Want = InPhase == TEXT("pick") ? EPick::Assert : InPhase == TEXT("wait") ? EPick::Wait : EPick::Branch;
		if (MoveSegment.IsValid() && Want != EPick::Assert)
		{
			OutError = TEXT("正在录移动：先结束（Alt+1）");
			SetNotice(OutError);
			SyncToolbar();
			return false;
		}
		PickArmed = PickArmed == Want ? EPick::None : Want;
		bOk = true;
	}
	else
	{
		OutError = FString::Printf(TEXT("gt.record.move 不认识 phase=%s（start / end / toggle / cancel / here / near / pick / wait / branch）"), *InPhase);
		return false;
	}

	if (!bOk)
	{
		SetNotice(OutError);
	}
	SyncToolbar();
	// 加了步骤的已经由 AddStep 推过 step；这里只推状态（录移动开没开、提示），面板不当成新步骤追加。
	Broadcast(TEXT("quick"));
	return bOk;
}

bool FGameTestRecorder::IsMoving() const
{
	return MoveSegment.IsValid();
}

bool FGameTestRecorder::OnHotkey(const FKey& InKey)
{
	if (!bRecording || !bHotkeys)
	{
		return false;
	}

	// 暂停问「这一格怎么认」时：Alt+K 直接接受推荐的继续（不用切到浏览器）；别的快捷键先不认
	if (IsAskingKey())
	{
		if (InKey != EKeys::K)
		{
			return false;
		}
		AcceptAskedKey();
		return true;
	}

	const TCHAR* Phase = nullptr;
	if (InKey == EKeys::One)
	{
		Phase = TEXT("toggle");
	}
	else if (InKey == EKeys::Zero)
	{
		Phase = TEXT("cancel");
	}
	else if (InKey == EKeys::Two)
	{
		Phase = TEXT("here");
	}
	else if (InKey == EKeys::Three)
	{
		Phase = TEXT("near");
	}
	else if (InKey == EKeys::Four)
	{
		Phase = TEXT("pick");
	}
	else if (InKey == EKeys::Six)
	{
		Phase = TEXT("wait");
	}
	else if (InKey == EKeys::Seven)
	{
		Phase = TEXT("branch");
	}
	else if (InKey != EKeys::Five)
	{
		return false;
	}

	OnTick();
	ForgetHotkeyModifiers();
	FString Error;
	if (Phase == nullptr)
	{
		if (!Mark(FString(), /*bInScreenshot=*/false, Error))
		{
			SetNotice(Error);
		}
		return true;
	}
	Quick(Phase, 0.0, Error);
	return true;
}

bool FGameTestRecorder::IsOnToolbar(const FVector2D& InScreenPosition) const
{
	return Toolbar.IsValid() && Toolbar->Contains(InScreenPosition);
}

void FGameTestRecorder::ForgetHotkeyModifiers()
{
	// 删掉尾巴上 Alt / Ctrl 的按下（和拾取断言同一套），再把它们从「移动段里按下的键」里摘掉：结束移动时不补按下。
	DropPickModifiers();
	for (TPair<FName, FHeldKey>& Pair : HeldKeys)
	{
		if (GameTestRecorderMoveDetail::IsHotkeyModifier(Pair.Key))
		{
			Pair.Value.bTyping = true;
			Pair.Value.bMoveOnly = false;
		}
	}
}

bool FGameTestRecorder::BeginMove(FString& OutError)
{
	if (MoveSegment.IsValid())
	{
		OutError = TEXT("已经在录移动了：先结束（Alt+1）或放弃（Alt+0）");
		return false;
	}
	UWorld* World = GameTestWorld::FindGameWorld();
	APawn* Pawn = World != nullptr ? GameTestMove::FindLocalPawn(*World) : nullptr;
	if (Pawn == nullptr)
	{
		OutError = TEXT("没有本地玩家的角色（游戏没在跑，或在菜单、观战里），录不了移动");
		return false;
	}

	const double Now = GameTestClock::Now();
	FlushTyping();
	AddGapWait(Now, /*bInUiStep=*/false);

	TUniquePtr<FGameTestMoveSegment> Segment = MakeUnique<FGameTestMoveSegment>();
	// 开始时按着的键（按住 Shift 跑）：补一条松开，这段里它的松开不录；结束时还按着再补按下。
	for (TPair<FName, FHeldKey>& Pair : HeldKeys)
	{
		if (Pair.Value.bTyping || GameTestRecorderMoveDetail::IsHotkeyModifier(Pair.Key))
		{
			continue;
		}
		const TSharedRef<FJsonObject> Release = GameTestRecorderMoveDetail::MakeStep(TEXT("key"));
		Release->SetStringField(TEXT("key"), Pair.Key.ToString());
		Release->SetStringField(TEXT("phase"), TEXT("release"));
		if (!Pair.Value.Target.IsEmpty())
		{
			Release->SetStringField(TEXT("target"), Pair.Value.Target);
		}
		AddStep(Release);
		Pair.Value.bTyping = true;
		Segment->HeldAtStart.Add(Pair.Key);
	}

	if (!Segment->Begin(*Pawn, Now))
	{
		OutError = TEXT("角色不在游戏世界里，录不了移动");
		return false;
	}
	Segment->LastBroadcastAt = Now;
	MoveSegment = MoveTemp(Segment);
	SetNotice(FString());
	UE_LOG(LogGameTest, Display, TEXT("录制：开始录移动（%s）"), *Pawn->GetName());
	return true;
}

bool FGameTestRecorder::EndMove(bool bInAuto, FString& OutError)
{
	if (!MoveSegment.IsValid())
	{
		OutError = TEXT("没有在录移动：先「录移动」（Alt+1）");
		return false;
	}
	FString Why;
	if (!MoveSegment->Sample(Why))
	{
		CancelMove(Why);
		OutError = Why;
		return false;
	}

	FGameTestMoveSegment& Segment = *MoveSegment;
	APawn* Pawn = Segment.Pawn.Get();
	UWorld* World = Segment.World.Get();
	const double Now = GameTestClock::Now();
	const FVector End = Pawn->GetActorLocation();
	if (FVector::Dist(Segment.Points.Last(), End) > 1.0)
	{
		Segment.Meters += FVector::Dist(Segment.Points.Last(), End) / 100.0;
		Segment.Points.Add(End);
		Segment.Modes.Add(FGameTestMoveSegment::ModeOf(*Pawn));
	}

	// 简化：起跳点和移动模式变化的点一定保留。
	TSet<int32> Keep = Segment.Jumps;
	for (int32 Index = 1; Index < Segment.Modes.Num(); ++Index)
	{
		if (Segment.Modes[Index] != Segment.Modes[Index - 1])
		{
			Keep.Add(Index - 1);
			Keep.Add(Index);
		}
	}
	const TArray<int32> Kept = GameTestTrajectory::SimplifyToLimit(Segment.Points, GameTestRecorderMoveDetail::SimplifyTolerance,
		GameTestRecorderMoveDetail::MaxPathPoints, Keep);
	TArray<FVector> Path;
	TArray<int32> Jumps;
	TArray<int32> Fly;
	for (int32 NewIndex = 0; NewIndex < Kept.Num(); ++NewIndex)
	{
		const int32 Old = Kept[NewIndex];
		Path.Add(Segment.Points[Old]);
		if (Segment.Jumps.Contains(Old))
		{
			Jumps.Add(NewIndex);
		}
		if (Segment.Modes.IsValidIndex(Old) && Segment.Modes[Old] == FGameTestMoveSegment::EMode::Fly)
		{
			Fly.Add(NewIndex);
		}
	}

	const TSharedRef<FJsonObject> Step = GameTestRecorderMoveDetail::MakeStep(TEXT("move"));
	Step->SetField(TEXT("to"), GameTestTrajectory::PointToJson(End));
	GameTestRecorderMoveDetail::SetFacing(*Step, *Pawn);
	const bool bWalked = Segment.Meters >= 0.5 && Path.Num() >= 2;
	if (bWalked)
	{
		Step->SetArrayField(TEXT("path"), GameTestTrajectory::PathToJson(Path));
		if (Jumps.Num() > 0)
		{
			Step->SetArrayField(TEXT("jumps"), GameTestTrajectory::IndicesToJson(Jumps));
		}
		if (Fly.Num() > 0)
		{
			Step->SetArrayField(TEXT("fly"), GameTestTrajectory::IndicesToJson(Fly));
		}
	}
	Step->SetStringField(TEXT("note"), bWalked
		? FString::Printf(TEXT("录制的移动（%.1f 米）"), Segment.Meters)
		: FString(TEXT("录制时记下的位置")));

	// 录制线索：只给人和面板看（回放不核对起点）。nav = 录制时导航网格能不能从起点算到终点。
	const TSharedRef<FJsonObject> Hint = MakeShared<FJsonObject>();
	Hint->SetField(TEXT("from"), GameTestTrajectory::PointToJson(Segment.Start));
	Hint->SetNumberField(TEXT("durationMs"), FMath::RoundToInt((Now - Segment.StartedAt) * 1000.0));
	Hint->SetNumberField(TEXT("distance"), FMath::RoundToInt(Segment.Meters * 100.0));
	TArray<FVector> NavPoints;
	FString NavWhy;
	Hint->SetBoolField(TEXT("nav"), World != nullptr && GameTestNav::GetState(*World, Pawn) == GameTestNav::EState::Ready
		&& GameTestNav::FindPath(*World, Pawn, Segment.Start, End, NavPoints, NavWhy));
	Hint->SetStringField(TEXT("pawn"), Pawn->GetClass()->GetName());
	if (World != nullptr)
	{
		Hint->SetStringField(TEXT("map"), GameTestRecorderMoveDetail::MapOf(*World));
	}
	Step->SetObjectField(TEXT("hint"), Hint);
	AddStep(Step);
	LastEventAt = Now;

	// 移动段里按下、现在还按着的键（以及开始前就按着、一直没松的）：补按下，之后的松开照常录。
	for (TPair<FName, FHeldKey>& Pair : HeldKeys)
	{
		const bool bCarried = Pair.Value.bMoveOnly || Segment.HeldAtStart.Contains(Pair.Key);
		if (!bCarried || GameTestRecorderMoveDetail::IsHotkeyModifier(Pair.Key))
		{
			continue;
		}
		const TSharedRef<FJsonObject> Press = GameTestRecorderMoveDetail::MakeStep(TEXT("key"));
		Press->SetStringField(TEXT("key"), Pair.Key.ToString());
		Press->SetStringField(TEXT("phase"), TEXT("press"));
		AddStep(Press);
		Pair.Value.bMoveOnly = false;
		Pair.Value.bTyping = false;
		Pair.Value.Target.Reset();
		Pair.Value.Time = Now;
	}

	UE_LOG(LogGameTest, Display, TEXT("录制：移动结束，%.1f 米、%d 个轨迹点%s"), Segment.Meters, Path.Num(), bInAuto ? TEXT("（自动结束）") : TEXT(""));
	MoveSegment.Reset();
	SetNotice(bInAuto ? FString(TEXT("点了界面，移动已自动结束")) : FString());
	return true;
}

void FGameTestRecorder::CancelMove(const FString& InWhy)
{
	if (!MoveSegment.IsValid())
	{
		return;
	}
	// 这段里按下的键没写步骤，松开也就不该录：当成打字顺带的键跳过。开始前按着的键已经补过松开。
	for (TPair<FName, FHeldKey>& Pair : HeldKeys)
	{
		if (Pair.Value.bMoveOnly)
		{
			Pair.Value.bMoveOnly = false;
			Pair.Value.bTyping = true;
		}
	}
	MoveSegment.Reset();
	LastEventAt = GameTestClock::Now();
	SetNotice(InWhy);
	UE_LOG(LogGameTest, Display, TEXT("录制：%s"), *InWhy);
	Broadcast(TEXT("quick"));
}

bool FGameTestRecorder::AddHereStep(FString& OutError)
{
	if (MoveSegment.IsValid())
	{
		OutError = TEXT("正在录移动：先结束（Alt+1）");
		return false;
	}
	UWorld* World = GameTestWorld::FindGameWorld();
	APawn* Pawn = World != nullptr ? GameTestMove::FindLocalPawn(*World) : nullptr;
	if (Pawn == nullptr)
	{
		OutError = TEXT("没有本地玩家的角色，记不了位置");
		return false;
	}
	FlushTyping();
	AddGapWait(GameTestClock::Now(), /*bInUiStep=*/false);
	const TSharedRef<FJsonObject> Step = GameTestRecorderMoveDetail::MakeStep(TEXT("move"));
	Step->SetField(TEXT("to"), GameTestTrajectory::PointToJson(Pawn->GetActorLocation()));
	GameTestRecorderMoveDetail::SetFacing(*Step, *Pawn);
	Step->SetStringField(TEXT("note"), TEXT("录制时记下的位置"));
	const TSharedRef<FJsonObject> Hint = MakeShared<FJsonObject>();
	Hint->SetStringField(TEXT("pawn"), Pawn->GetClass()->GetName());
	Hint->SetStringField(TEXT("map"), GameTestRecorderMoveDetail::MapOf(*World));
	Step->SetObjectField(TEXT("hint"), Hint);
	AddStep(Step);
	SetNotice(TEXT("已记下当前位置"));
	return true;
}

bool FGameTestRecorder::AddNearStep(double InRadius, FString& OutError)
{
	if (MoveSegment.IsValid())
	{
		OutError = TEXT("正在录移动：先结束（Alt+1）");
		return false;
	}
	UWorld* World = GameTestWorld::FindGameWorld();
	APawn* Pawn = World != nullptr ? GameTestMove::FindLocalPawn(*World) : nullptr;
	if (Pawn == nullptr)
	{
		OutError = TEXT("没有本地玩家的角色，记不了位置");
		return false;
	}
	FlushTyping();
	AddGapWait(GameTestClock::Now(), /*bInUiStep=*/false);
	// 用已有的 wait until=actorNear：按角色的类找（单机只有一个），检查前面的操作确实把角色带到了这里。
	const TSharedRef<FJsonObject> Step = GameTestRecorderMoveDetail::MakeStep(TEXT("wait"));
	Step->SetStringField(TEXT("until"), TEXT("actorNear"));
	Step->SetStringField(TEXT("actor"), Pawn->GetClass()->GetName());
	Step->SetField(TEXT("point"), GameTestTrajectory::PointToJson(Pawn->GetActorLocation()));
	Step->SetNumberField(TEXT("radius"), InRadius);
	Step->SetStringField(TEXT("note"), TEXT("录制时：角色应该在这附近"));
	AddStep(Step);
	SetNotice(TEXT("已录「在这附近」"));
	return true;
}

bool FGameTestRecorder::FilterDuringMove(const FRawEvent& InEvent)
{
	switch (InEvent.Kind)
	{
	case ERawKind::MouseDown:
	{
		bool bIsUi = false;
		FString Target;
		TSharedPtr<FJsonObject> Hint;
		TSharedPtr<SWidget> Slate;
		if (DescribeHandler(InEvent, bIsUi, Target, Hint, Slate) && bIsUi)
		{
			// 点了界面：移动到这里为止，这一下点击照常录（用户定：不丢操作，也不用记着先结束）。
			FString Error;
			EndMove(/*bInAuto=*/true, Error);
			return false;
		}
		return true;
	}
	case ERawKind::MouseUp:
	{
		// 移动段开始前按下的那一下（点击还没松开）照常配对；段里在游戏画面上的按下没录，松开也不录。
		if (UiPress.IsSet() && UiPress->Button == InEvent.Key)
		{
			return false;
		}
		const FHeldKey* Held = HeldKeys.Find(InEvent.Key);
		return Held == nullptr || Held->bMoveOnly || Held->bTyping;
	}
	case ERawKind::KeyDown:
		if (!HeldKeys.Contains(InEvent.Key))
		{
			FHeldKey Held;
			Held.Time = InEvent.Time;
			Held.bMoveOnly = true;
			HeldKeys.Add(InEvent.Key, Held);
		}
		return true;
	case ERawKind::KeyUp:
		HeldKeys.Remove(InEvent.Key);
		return true;
	default:
		return true;
	}
}

void FGameTestRecorder::TickMove()
{
	if (!bRecording)
	{
		return;
	}
	const double Now = GameTestClock::Now();
	if (MoveSegment.IsValid())
	{
		FString Why;
		if (!MoveSegment->Sample(Why))
		{
			CancelMove(Why);
		}
		else if (Now - MoveSegment->LastBroadcastAt >= 1.0)
		{
			MoveSegment->LastBroadcastAt = Now;
			Broadcast(TEXT("moving"));
		}
	}
	if (!Notice.IsEmpty() && Now - NoticeAt > GameTestRecorderMoveDetail::NoticeSeconds)
	{
		Notice.Reset();
	}
	SyncToolbar();
}

void FGameTestRecorder::SetNotice(const FString& InNotice)
{
	Notice = InNotice;
	NoticeAt = GameTestClock::Now();
}

void FGameTestRecorder::SyncToolbar()
{
	if (!bRecording)
	{
		return;
	}
	if (!Toolbar.IsValid())
	{
		Toolbar = MakeShared<FGameTestRecordToolbar>();
	}
	FGameTestRecordToolbar::FState State;
	State.bMoving = MoveSegment.IsValid();
	State.bHotkeys = bHotkeys;
	State.bPickArmed = PickArmed != EPick::None;
	State.PickHint = DescribePickArmed();
	State.Notice = Notice;
	const double Seconds = GameTestClock::Now() - StartedAt;
	State.Status = State.bMoving
		? FString::Printf(TEXT("正在录移动 · 已走 %.1f 米 · 其他操作暂不记录"), MoveSegment->Meters)
		: FString::Printf(TEXT("录制中 %02d:%02d · %d 步"), int32(Seconds) / 60, int32(Seconds) % 60, Steps.Num());
	Toolbar->Sync(State);
}

void FGameTestRecorder::HideToolbar()
{
	if (Toolbar.IsValid())
	{
		Toolbar->Hide();
	}
	Toolbar.Reset();
}

void FGameTestRecorder::DescribeQuick(FJsonObject& OutStatus) const
{
	OutStatus.SetBoolField(TEXT("hotkeys"), bHotkeys);
	if (PickArmed != EPick::None)
	{
		OutStatus.SetStringField(TEXT("pickKind"), PickName(PickArmed));
		OutStatus.SetBoolField(TEXT("pickArmed"), true);
	}
	if (!Notice.IsEmpty())
	{
		OutStatus.SetStringField(TEXT("notice"), Notice);
	}
	if (MoveSegment.IsValid())
	{
		const TSharedRef<FJsonObject> Moving = MakeShared<FJsonObject>();
		Moving->SetNumberField(TEXT("meters"), FMath::RoundToDouble(MoveSegment->Meters * 10.0) / 10.0);
		Moving->SetNumberField(TEXT("seconds"), FMath::RoundToDouble((GameTestClock::Now() - MoveSegment->StartedAt) * 10.0) / 10.0);
		Moving->SetNumberField(TEXT("points"), MoveSegment->Points.Num());
		OutStatus.SetObjectField(TEXT("moving"), Moving);
	}
}
