// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/GameTestMove.h"
#include "Core/GameTestClock.h"
#include "GameTestCompat.h"

#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestTrajectory.h"
#include "Core/GameTestWorld.h"
#include "Input/GameTestInput.h"
#include "Observe/GameTestActor.h"
#include "Observe/GameTestNav.h"
#include "Observe/GameTestPathProbe.h"
#include "GameTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"

namespace GameTestMoveDetail
{
	/** 每帧探路最多做这么多次物理查询：几百次射线和胶囊查询在一帧里是零点几毫秒。 */
	constexpr int32 ProbeBudgetPerFrame = 300;

	/** 推了这么久离下一个路点没近 20 厘米算卡住（先试一次跳，还卡就换下一种走法）。 */
	constexpr double StuckSeconds = 2.0;
	constexpr double StuckProgress = 20.0;

	/** 离录制轨迹这么近（水平，厘米）就直接接上走，再远先探路走到轨迹上。 */
	constexpr double JoinDirectDistance = 300.0;

	/** 导航网格还在生成时最多等这么久，再不好就算这种走法不行。 */
	constexpr double NavBuildWaitSeconds = 30.0;

	/** 结果里带回的实际路线最多这么多个点（面板画俯视图用）。 */
	constexpr int32 MaxTrailPoints = 100;

	/** 到达判定的竖直容差：楼上楼下同一个平面位置不算到了。 */
	constexpr double ArriveVertical = 250.0;

	enum class EWay : uint8
	{
		Nav,
		Path,
		Probe,
		Teleport,
	};

	const TCHAR* WayName(EWay InWay)
	{
		switch (InWay)
		{
		case EWay::Nav:
			return TEXT("nav");
		case EWay::Path:
			return TEXT("path");
		case EWay::Probe:
			return TEXT("probe");
		default:
			return TEXT("teleport");
		}
	}

	const TCHAR* WayLabel(EWay InWay)
	{
		switch (InWay)
		{
		case EWay::Nav:
			return TEXT("导航网格寻路");
		case EWay::Path:
			return TEXT("按录制轨迹");
		case EWay::Probe:
			return TEXT("探路");
		default:
			return TEXT("瞬移");
		}
	}

	/** 合并后的走法设置：步骤字段 > 场景 move 段 > 运行设置 move 段 > 默认。 */
	struct FMoveSettings
	{
		FString Via = TEXT("auto");
		FString Fallback = TEXT("none");
		FString TeleportCommand;
		double Radius = 0.0;
		FString Drive = TEXT("input");
		FString ForwardKey = TEXT("W");
	};

	void ReadSettings(const FJsonObject* InJson, FMoveSettings& InOutSettings)
	{
		if (InJson == nullptr)
		{
			return;
		}
		GameTestJson::TryGetString(*InJson, TEXT("via"), InOutSettings.Via);
		GameTestJson::TryGetString(*InJson, TEXT("fallback"), InOutSettings.Fallback);
		GameTestJson::TryGetString(*InJson, TEXT("teleportCommand"), InOutSettings.TeleportCommand);
		GameTestJson::TryGetNumber(*InJson, TEXT("radius"), InOutSettings.Radius);
		GameTestJson::TryGetString(*InJson, TEXT("drive"), InOutSettings.Drive);
		GameTestJson::TryGetString(*InJson, TEXT("forwardKey"), InOutSettings.ForwardKey);
	}

	bool OneOf(const FString& InValue, std::initializer_list<const TCHAR*> InAllowed)
	{
		for (const TCHAR* Allowed : InAllowed)
		{
			if (InValue == Allowed)
			{
				return true;
			}
		}
		return false;
	}

	/** 每一步走法的状态机。挂在等待谓词上，每帧 Tick 一次；步骤结束时 Cleanup。 */
	class FMover
	{
	public:
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<APawn> Pawn;
		TWeakObjectPtr<AActor> GoalActor;
		FVector Goal = FVector::ZeroVector;
		TOptional<double> Yaw;
		TOptional<double> Pitch;
		double Radius = 100.0;
		TArray<FVector> Recorded;
		TSet<int32> RecordedJumps;
		TSet<int32> RecordedFly;
		FMoveSettings Settings;
		TArray<EWay> Ways;

		TSharedPtr<FJsonObject> Payload;
		TSharedPtr<FString> Progress = MakeShared<FString>();
		TSharedPtr<FString> Failure = MakeShared<FString>();

		/** 返回 true 表示这一步结束了（成功，或 Failure 里写了失败原因）。 */
		bool Tick(double InNow)
		{
			if (bDone)
			{
				return true;
			}
			APawn* PawnPtr = Pawn.Get();
			if (PawnPtr == nullptr || !World.IsValid())
			{
				return Fail(TEXT("角色没了（被销毁、换了角色或世界重载了）"));
			}
			if (StartedAt <= 0.0)
			{
				StartedAt = InNow;
			}
			if (AActor* Target = GoalActor.Get())
			{
				// 走向会动的 actor（NPC 在巡逻）：每帧跟着它的位置。
				Goal = Target->GetActorLocation();
			}
			SampleTrail(PawnPtr->GetActorLocation());

			if (Phase != EPhase::Teleporting && IsArrived(*PawnPtr))
			{
				return Finish(*PawnPtr, InNow);
			}

			switch (Phase)
			{
			case EPhase::Next:
				return StartNextWay(*PawnPtr, InNow);
			case EPhase::WaitNav:
				return TickWaitNav(*PawnPtr, InNow);
			case EPhase::Probing:
				return TickProbe(*PawnPtr, InNow);
			case EPhase::Walking:
				return TickWalk(*PawnPtr, InNow);
			case EPhase::Teleporting:
				return TickTeleport(*PawnPtr, InNow);
			default:
				return true;
			}
		}

		/** 步骤结束（成功、失败、超时、取消）都会调：松开按住的键、清掉残留的移动输入。 */
		void Cleanup()
		{
			ReleaseDrive();
		}

	private:
		enum class EPhase : uint8
		{
			Next,
			WaitNav,
			Probing,
			Walking,
			Teleporting,
		};

		EPhase Phase = EPhase::Next;
		int32 WayIndex = -1;
		double StartedAt = 0.0;
		double WayStartedAt = 0.0;
		bool bDone = false;

		TArray<FVector> Waypoints;
		TSet<int32> WaypointJumps;
		TSet<int32> WaypointFly;
		int32 Cursor = 0;

		/** 按轨迹走、离轨迹远时：先探路走到接入点，探到之后接上的那段轨迹。 */
		TArray<FVector> Tail;
		TSet<int32> TailJumps;
		TSet<int32> TailFly;
		TUniquePtr<FGameTestPathProbe> Probe;

		double BestDistance = TNumericLimits<double>::Max();
		double LastProgressAt = 0.0;
		bool bJumpTried = false;
		int32 StopJumpFrames = 0;
		bool bKeyHeld = false;
		double TeleportAt = 0.0;

		TArray<TSharedPtr<FJsonValue>> Tried;
		TArray<FVector> Trail;
		FVector StuckAt = FVector::ZeroVector;
		bool bStuck = false;

		EWay CurrentWay() const
		{
			return Ways.IsValidIndex(WayIndex) ? Ways[WayIndex] : EWay::Teleport;
		}

		bool IsFlying(const APawn& InPawn) const
		{
			const UCharacterMovementComponent* Movement = Cast<UCharacterMovementComponent>(InPawn.GetMovementComponent());
			return Movement != nullptr && (Movement->IsFlying() || Movement->IsSwimming());
		}

		bool IsArrived(const APawn& InPawn) const
		{
			const FVector Location = InPawn.GetActorLocation();
			if (IsFlying(InPawn))
			{
				return FVector::Dist(Location, Goal) <= Radius;
			}
			return FVector2D::Distance(FVector2D(Location), FVector2D(Goal)) <= Radius
				&& FMath::Abs(Location.Z - Goal.Z) <= ArriveVertical;
		}

		void SampleTrail(const FVector& InLocation)
		{
			if (Trail.Num() == 0 || FVector::Dist(Trail.Last(), InLocation) >= 100.0)
			{
				Trail.Add(InLocation);
			}
		}

		bool StartNextWay(APawn& InPawn, double InNow)
		{
			++WayIndex;
			if (!Ways.IsValidIndex(WayIndex))
			{
				TArray<FString> Lines;
				for (const TSharedPtr<FJsonValue>& Item : Tried)
				{
					const TSharedPtr<FJsonObject>* Object = nullptr;
					FString Via;
					FString Why;
					if (Item.IsValid() && Item->TryGetObject(Object) && Object != nullptr && Object->IsValid())
					{
						(*Object)->TryGetStringField(TEXT("via"), Via);
						(*Object)->TryGetStringField(TEXT("why"), Why);
					}
					Lines.Add(FString::Printf(TEXT("%s：%s"), *Via, *Why));
				}
				return Fail(FString::Printf(TEXT("走不到 (%.0f, %.0f, %.0f)。%s"), Goal.X, Goal.Y, Goal.Z,
					Lines.Num() > 0 ? *FString::Join(Lines, TEXT("；")) : TEXT("没有可用的走法")));
			}

			WayStartedAt = InNow;
			ResetWalk(InNow);
			const EWay Way = CurrentWay();
			FString Why;
			bool bOk = false;
			switch (Way)
			{
			case EWay::Nav:
				bOk = PrepareNav(InPawn, InNow, Why);
				break;
			case EWay::Path:
				bOk = PreparePath(InPawn, Why);
				break;
			case EWay::Probe:
				bOk = StartProbe(InPawn, Goal, {}, {}, {});
				break;
			case EWay::Teleport:
				bOk = Teleport(InPawn, InNow, Why);
				break;
			}
			if (!bOk)
			{
				return FailWay(Why);
			}
			*Progress = FString::Printf(TEXT("正在%s"), WayLabel(Way));
			return false;
		}

		void ResetWalk(double InNow)
		{
			Waypoints.Reset();
			WaypointJumps.Reset();
			WaypointFly.Reset();
			Tail.Reset();
			TailJumps.Reset();
			TailFly.Reset();
			Probe.Reset();
			Cursor = 0;
			BestDistance = TNumericLimits<double>::Max();
			LastProgressAt = InNow;
			bJumpTried = false;
		}

		bool PrepareNav(APawn& InPawn, double InNow, FString& OutWhy)
		{
			UWorld* WorldPtr = World.Get();
			const GameTestNav::EState State = GameTestNav::GetState(*WorldPtr, &InPawn);
			if (State == GameTestNav::EState::Missing)
			{
				OutWhy = TEXT("没有导航网格");
				return false;
			}
			if (State == GameTestNav::EState::Building)
			{
				Phase = EPhase::WaitNav;
				return true;
			}
			TArray<FVector> Points;
			if (!GameTestNav::FindPath(*WorldPtr, &InPawn, InPawn.GetActorLocation(), Goal, Points, OutWhy))
			{
				return false;
			}
			SetWaypoints(Points, {}, {});
			return true;
		}

		bool TickWaitNav(APawn& InPawn, double InNow)
		{
			if (GameTestNav::GetState(*World.Get(), &InPawn) == GameTestNav::EState::Building)
			{
				*Progress = TEXT("等导航网格生成完");
				if (InNow - WayStartedAt > NavBuildWaitSeconds)
				{
					return FailWay(FString::Printf(TEXT("导航网格 %.0f 秒还没生成完"), NavBuildWaitSeconds));
				}
				return false;
			}
			FString Why;
			if (!PrepareNav(InPawn, InNow, Why) || Phase == EPhase::WaitNav)
			{
				return FailWay(Why.IsEmpty() ? FString(TEXT("导航网格不可用")) : Why);
			}
			return false;
		}

		bool PreparePath(APawn& InPawn, FString& OutWhy)
		{
			if (Recorded.Num() < 2)
			{
				OutWhy = TEXT("没有录制轨迹");
				return false;
			}
			double Distance = 0.0;
			FVector Projected;
			const int32 Join = GameTestTrajectory::FindJoinIndex(Recorded, InPawn.GetActorLocation(), Distance, Projected);
			if (Join == INDEX_NONE)
			{
				OutWhy = TEXT("录制轨迹不完整");
				return false;
			}

			// 接上之后的那段轨迹：下标整体前移，起跳点和游泳 / 飞行段跟着走。
			TArray<FVector> Rest;
			TSet<int32> RestJumps;
			TSet<int32> RestFly;
			for (int32 Index = Join; Index < Recorded.Num(); ++Index)
			{
				Rest.Add(Recorded[Index]);
				if (RecordedJumps.Contains(Index))
				{
					RestJumps.Add(Index - Join);
				}
				if (RecordedFly.Contains(Index))
				{
					RestFly.Add(Index - Join);
				}
			}
			// 起跳点正好是接入点前一个点（角色站在起跳点和落点之间）时，接上后的第一段也要跳。
			if (RecordedJumps.Contains(Join - 1))
			{
				RestJumps.Add(-1);
			}

			if (Distance <= JoinDirectDistance)
			{
				SetWaypoints(Rest, RestJumps, RestFly);
				return true;
			}
			// 离轨迹远（回放不管起点在哪）：先探路走到接入点，再沿轨迹走完。
			return StartProbe(InPawn, Projected, Rest, RestJumps, RestFly);
		}

		bool StartProbe(APawn& InPawn, const FVector& InTarget, const TArray<FVector>& InTail, const TSet<int32>& InTailJumps, const TSet<int32>& InTailFly)
		{
			Tail = InTail;
			TailJumps = InTailJumps;
			TailFly = InTailFly;
			Probe = MakeUnique<FGameTestPathProbe>();
			Probe->Start(*World.Get(), InPawn, InPawn.GetActorLocation(), InTarget, InTail.Num() > 0 ? JoinDirectDistance * 0.5 : Radius);
			Phase = EPhase::Probing;
			return true;
		}

		bool TickProbe(APawn& InPawn, double InNow)
		{
			if (!Probe.IsValid())
			{
				return FailWay(TEXT("探路状态丢了"));
			}
			*Progress = FString::Printf(TEXT("探路中（已查 %d 格）"), Probe->GetEvaluated());
			const FGameTestPathProbe::EResult Result = Probe->Step(ProbeBudgetPerFrame);
			if (Result == FGameTestPathProbe::EResult::Running)
			{
				return false;
			}
			if (Result == FGameTestPathProbe::EResult::Failed)
			{
				return FailWay(Probe->GetWhy());
			}

			if (Payload.IsValid())
			{
				Payload->SetNumberField(TEXT("probeCells"), Probe->GetEvaluated());
			}
			TArray<FVector> Points = Probe->GetPath();
			TSet<int32> Jumps;
			TSet<int32> Fly;
			const int32 Offset = Points.Num();
			for (int32 Index = 0; Index < Tail.Num(); ++Index)
			{
				Points.Add(Tail[Index]);
			}
			for (const int32 Index : TailJumps)
			{
				Jumps.Add(Index + Offset);
			}
			for (const int32 Index : TailFly)
			{
				Fly.Add(Index + Offset);
			}
			SetWaypoints(Points, Jumps, Fly);
			LastProgressAt = InNow;
			return false;
		}

		/** 路点定下来：最后一个点总是终点（导航网格的点在地面上、轨迹的终点是录制时站的位置，都按真目标算到达）。 */
		void SetWaypoints(const TArray<FVector>& InPoints, const TSet<int32>& InJumps, const TSet<int32>& InFly)
		{
			Waypoints = InPoints;
			WaypointJumps = InJumps;
			WaypointFly = InFly;
			if (Waypoints.Num() == 0 || FVector2D::Distance(FVector2D(Waypoints.Last()), FVector2D(Goal)) > 1.0)
			{
				Waypoints.Add(Goal);
			}
			Cursor = 0;
			BestDistance = TNumericLimits<double>::Max();
			Phase = EPhase::Walking;
		}

		bool TickWalk(APawn& InPawn, double InNow)
		{
			UWorld* WorldPtr = World.Get();
			const float DeltaSeconds = WorldPtr != nullptr ? WorldPtr->GetDeltaSeconds() : 0.016f;
			ACharacter* Character = Cast<ACharacter>(&InPawn);
			if (StopJumpFrames > 0 && --StopJumpFrames == 0 && Character != nullptr)
			{
				Character->StopJumping();
			}

			if (!Waypoints.IsValidIndex(Cursor))
			{
				const double Left = FVector2D::Distance(FVector2D(InPawn.GetActorLocation()), FVector2D(Goal));
				return FailWay(FString::Printf(TEXT("走完路线还差 %.0f 厘米"), Left));
			}

			const FVector Location = InPawn.GetActorLocation();
			const bool bFly = WaypointFly.Contains(Cursor) || IsFlying(InPawn);
			FVector Delta = Waypoints[Cursor] - Location;
			if (!bFly)
			{
				Delta.Z = 0.0;
			}
			const double Distance = Delta.Size();
			const bool bLast = Cursor == Waypoints.Num() - 1;
			float AgentRadius = 34.0f;
			float AgentHalfHeight = 88.0f;
			InPawn.GetSimpleCollisionCylinder(AgentRadius, AgentHalfHeight);
			const double Reach = bLast ? Radius * 0.8 : FMath::Max<double>(AgentRadius, 40.0);

			// 起跳点在「离开这个路点时」跳；下标 -1 表示接上轨迹后的第一段就要跳。
			if (Cursor == 0 && WaypointJumps.Contains(-1) && Character != nullptr)
			{
				WaypointJumps.Remove(-1);
				Jump(*Character);
			}
			if (Distance <= Reach)
			{
				if (WaypointJumps.Contains(Cursor) && Character != nullptr)
				{
					Jump(*Character);
				}
				++Cursor;
				BestDistance = TNumericLimits<double>::Max();
				LastProgressAt = InNow;
				bJumpTried = false;
				*Progress = FString::Printf(TEXT("%s：第 %d/%d 个路点"), WayLabel(CurrentWay()), FMath::Min(Cursor + 1, Waypoints.Num()), Waypoints.Num());
				return false;
			}

			const FVector Direction = Delta.GetSafeNormal();
			Steer(InPawn, Direction, DeltaSeconds);

			if (Distance < BestDistance - StuckProgress)
			{
				BestDistance = Distance;
				LastProgressAt = InNow;
			}
			else if (InNow - LastProgressAt > StuckSeconds)
			{
				if (!bJumpTried && Character != nullptr)
				{
					bJumpTried = true;
					LastProgressAt = InNow;
					Jump(*Character);
					return false;
				}
				bStuck = true;
				StuckAt = Location;
				return FailWay(FString::Printf(TEXT("在 (%.0f, %.0f, %.0f) 卡住了（推了 %.0f 秒没往前走；项目的移动不读 AddMovementInput 时试 drive=keys）"),
					Location.X, Location.Y, Location.Z, StuckSeconds));
			}
			*Progress = FString::Printf(TEXT("%s：第 %d/%d 个路点，离终点 %.0f 厘米"), WayLabel(CurrentWay()), Cursor + 1, Waypoints.Num(),
				FVector::Dist(Location, Goal));
			return false;
		}

		/** 往一个方向推：input 驱动推摇杆，keys 驱动按住前进键并把镜头转过去。镜头都跟着转，到了再转回录制时的朝向。 */
		void Steer(APawn& InPawn, const FVector& InDirection, float InDeltaSeconds)
		{
			AController* Controller = InPawn.GetController();
			if (Controller != nullptr && !InDirection.IsNearlyZero())
			{
				const FRotator Current = Controller->GetControlRotation();
				FRotator Target = Current;
				Target.Yaw = InDirection.Rotation().Yaw;
				const bool bKeys = Settings.Drive == TEXT("keys");
				Controller->SetControlRotation(bKeys ? Target : FMath::RInterpTo(Current, Target, InDeltaSeconds, 6.0f));
			}

			if (Settings.Drive == TEXT("keys"))
			{
				if (!bKeyHeld)
				{
					FString Message;
					bKeyHeld = GameTestInput::SendKey(FKey(*Settings.ForwardKey), /*bInDown=*/true, Message);
				}
				return;
			}
			InPawn.AddMovementInput(InDirection, 1.0f, /*bForce=*/true);
		}

		void Jump(ACharacter& InCharacter)
		{
			InCharacter.Jump();
			StopJumpFrames = 3;
		}

		bool Teleport(APawn& InPawn, double InNow, FString& OutWhy)
		{
			const FRotator Rotation(InPawn.GetActorRotation().Pitch, Yaw.IsSet() ? Yaw.GetValue() : InPawn.GetActorRotation().Yaw, 0.0);
			if (!Settings.TeleportCommand.IsEmpty())
			{
				FString Command = Settings.TeleportCommand;
				Command.ReplaceInline(TEXT("{x}"), *FString::SanitizeFloat(FMath::RoundToDouble(Goal.X)));
				Command.ReplaceInline(TEXT("{y}"), *FString::SanitizeFloat(FMath::RoundToDouble(Goal.Y)));
				Command.ReplaceInline(TEXT("{z}"), *FString::SanitizeFloat(FMath::RoundToDouble(Goal.Z)));
				Command.ReplaceInline(TEXT("{yaw}"), *FString::SanitizeFloat(FMath::RoundToDouble(Rotation.Yaw)));
				APlayerController* Controller = Cast<APlayerController>(InPawn.GetController());
				if (Controller == nullptr)
				{
					OutWhy = TEXT("没有玩家控制器，执行不了瞬移指令");
					return false;
				}
				Controller->ConsoleCommand(Command, /*bWriteToLog=*/true);
				UE_LOG(LogGameTest, Display, TEXT("move：瞬移指令 %s"), *Command);
			}
			else if (!InPawn.TeleportTo(Goal, Rotation))
			{
				OutWhy = TEXT("瞬移被挡住了（目标点放不下角色）");
				return false;
			}
			Phase = EPhase::Teleporting;
			TeleportAt = InNow;
			return true;
		}

		bool TickTeleport(APawn& InPawn, double InNow)
		{
			// 等几帧：指令瞬移、联机下的位置同步都不是当帧生效。
			if (InNow - TeleportAt < 0.3)
			{
				return false;
			}
			if (IsArrived(InPawn))
			{
				return Finish(InPawn, InNow);
			}
			const bool bClient = World.IsValid() && World->GetNetMode() == NM_Client;
			return FailWay(bClient && Settings.TeleportCommand.IsEmpty()
				? TEXT("客户端上直接瞬移被服务器拉回了：在运行设置的 move.teleportCommand 里配项目自己的 GM 瞬移指令")
				: TEXT("瞬移后角色不在目标附近"));
		}

		bool FailWay(const FString& InWhy)
		{
			ReleaseDrive();
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("via"), WayName(CurrentWay()));
			Item->SetStringField(TEXT("why"), InWhy);
			Tried.Add(MakeShared<FJsonValueObject>(Item));
			UE_LOG(LogGameTest, Display, TEXT("move：%s 不行（%s），换下一种走法"), WayLabel(CurrentWay()), *InWhy);
			Phase = EPhase::Next;
			return false;
		}

		bool Finish(APawn& InPawn, double InNow)
		{
			ReleaseDrive();
			if (AController* Controller = InPawn.GetController())
			{
				if (Yaw.IsSet() || Pitch.IsSet())
				{
					FRotator Rotation = Controller->GetControlRotation();
					Rotation.Yaw = Yaw.Get(Rotation.Yaw);
					Rotation.Pitch = Pitch.Get(Rotation.Pitch);
					Controller->SetControlRotation(Rotation);
				}
			}
			const EWay Used = Ways.IsValidIndex(WayIndex) ? Ways[WayIndex] : EWay::Path;
			WritePayload(InPawn, InNow);
			if (Payload.IsValid())
			{
				// 还没开始选走法就已经在终点附近：一步没走。
				Payload->SetStringField(TEXT("used"), WayIndex < 0 ? TEXT("none") : WayName(Used));
				Payload->SetBoolField(TEXT("reached"), true);
			}
			bDone = true;
			return true;
		}

		bool Fail(const FString& InMessage)
		{
			ReleaseDrive();
			if (APawn* PawnPtr = Pawn.Get())
			{
				WritePayload(*PawnPtr, GameTestClock::Now());
			}
			*Failure = InMessage;
			bDone = true;
			return true;
		}

		void WritePayload(const APawn& InPawn, double InNow)
		{
			if (!Payload.IsValid())
			{
				return;
			}
			Payload->SetField(TEXT("to"), GameTestTrajectory::PointToJson(Goal));
			Payload->SetNumberField(TEXT("distance"), FMath::RoundToInt(FVector::Dist(InPawn.GetActorLocation(), Goal)));
			Payload->SetNumberField(TEXT("ms"), FMath::RoundToInt((InNow - StartedAt) * 1000.0));
			if (Tried.Num() > 0)
			{
				Payload->SetArrayField(TEXT("tried"), Tried);
			}
			if (bStuck)
			{
				Payload->SetField(TEXT("stuck"), GameTestTrajectory::PointToJson(StuckAt));
			}
			TArray<FVector> Points = Trail;
			Points.Add(InPawn.GetActorLocation());
			const TArray<int32> Kept = GameTestTrajectory::SimplifyToLimit(Points, 30.0, MaxTrailPoints, {});
			TArray<FVector> Simplified;
			for (const int32 Index : Kept)
			{
				Simplified.Add(Points[Index]);
			}
			Payload->SetArrayField(TEXT("trail"), GameTestTrajectory::PathToJson(Simplified));
		}

		void ReleaseDrive()
		{
			if (bKeyHeld)
			{
				FString Message;
				GameTestInput::SendKey(FKey(*Settings.ForwardKey), /*bInDown=*/false, Message);
				bKeyHeld = false;
			}
			if (APawn* PawnPtr = Pawn.Get())
			{
				// 清掉这一帧没被消费的移动输入：不让角色在下一步里多滑一下。
				PawnPtr->ConsumeMovementInputVector();
				if (ACharacter* Character = Cast<ACharacter>(PawnPtr))
				{
					Character->StopJumping();
				}
			}
			StopJumpFrames = 0;
		}
	};

	/** 角色不在 / 目标 actor 找不到时给的半径：目标 actor 的碰撞半径 + 角色半径 + 30。 */
	double ActorGoalRadius(const AActor& InTarget, const APawn& InPawn)
	{
		float TargetRadius = 0.0f;
		float TargetHalfHeight = 0.0f;
		InTarget.GetSimpleCollisionCylinder(TargetRadius, TargetHalfHeight);
		float PawnRadius = 34.0f;
		float PawnHalfHeight = 88.0f;
		InPawn.GetSimpleCollisionCylinder(PawnRadius, PawnHalfHeight);
		return FMath::Max<double>(TargetRadius, 0.0) + PawnRadius + 30.0;
	}

	/** 没写 timeoutMs 时的上限：按录制用时或「距离 ÷ 走速」的 3 倍，再加 10 秒（导航网格生成、探路的时间）。 */
	int32 EstimateTimeoutMs(const FJsonObject& InParams, const APawn& InPawn, const FVector& InGoal, const TArray<FVector>& InRecorded)
	{
		if (const TSharedPtr<FJsonObject> Hint = GameTestJson::TryGetObject(InParams, TEXT("hint")))
		{
			double DurationMs = 0.0;
			if (GameTestJson::TryGetNumber(*Hint, TEXT("durationMs"), DurationMs) && DurationMs > 0.0)
			{
				return FMath::Clamp(FMath::RoundToInt(DurationMs * 3.0) + 10000, 15000, 600000);
			}
		}
		double Speed = 300.0;
		if (const UPawnMovementComponent* Movement = InPawn.GetMovementComponent())
		{
			Speed = FMath::Max<double>(Movement->GetMaxSpeed(), 100.0);
		}
		const double Distance = InRecorded.Num() >= 2
			? GameTestTrajectory::Length(InRecorded) + FVector::Dist(InPawn.GetActorLocation(), InRecorded[0])
			: FVector::Dist(InPawn.GetActorLocation(), InGoal) * 1.5;
		return FMath::Clamp(FMath::RoundToInt(Distance / Speed * 3000.0) + 10000, 15000, 600000);
	}
}

bool GameTestMove::ValidateDefaults(const FJsonObject& InJson, FString& OutError)
{
	static const TCHAR* Allowed[] = {TEXT("fallback"), TEXT("teleportCommand"), TEXT("radius"), TEXT("drive"), TEXT("forwardKey"), TEXT("hotkeys")};
	for (const auto& Pair : InJson.Values)
	{
		const FString Key = GAMETEST_JSON_KEY(Pair.Key);
		bool bKnown = false;
		for (const TCHAR* Name : Allowed)
		{
			bKnown |= Key == Name;
		}
		if (!bKnown)
		{
			OutError = FString::Printf(TEXT("move 段不认识字段 %s（可用：fallback / teleportCommand / radius / drive / forwardKey / hotkeys）"), *Key);
			return false;
		}
	}
	FString Text;
	if (GameTestJson::TryGetString(InJson, TEXT("fallback"), Text) && !GameTestMoveDetail::OneOf(Text, {TEXT("none"), TEXT("teleport")}))
	{
		OutError = TEXT("move.fallback 只能是 none（走不到就失败）或 teleport（瞬移兜底）");
		return false;
	}
	if (GameTestJson::TryGetString(InJson, TEXT("drive"), Text) && !GameTestMoveDetail::OneOf(Text, {TEXT("input"), TEXT("keys")}))
	{
		OutError = TEXT("move.drive 只能是 input（推摇杆，默认）或 keys（按住前进键并转镜头）");
		return false;
	}
	if (GameTestJson::TryGetString(InJson, TEXT("forwardKey"), Text) && !Text.IsEmpty() && !FKey(*Text).IsValid())
	{
		OutError = FString::Printf(TEXT("move.forwardKey 不是 UE 的键名：%s（例：W、Gamepad_LeftY）"), *Text);
		return false;
	}
	double Radius = 0.0;
	if (InJson.HasField(TEXT("radius")) && (!GameTestJson::TryGetNumber(InJson, TEXT("radius"), Radius) || Radius < 0.0))
	{
		OutError = TEXT("move.radius 要写 >= 0 的数（厘米，0 = 按角色大小）");
		return false;
	}
	return true;
}

APawn* GameTestMove::FindLocalPawn(UWorld& InWorld)
{
	APlayerController* Controller = InWorld.GetFirstPlayerController();
	return Controller != nullptr ? Controller->GetPawn() : nullptr;
}

TSharedPtr<FJsonObject> GameTestMove::MergeDefaults(const TSharedPtr<FJsonObject>& InBase, const TSharedPtr<FJsonObject>& InOverride)
{
	if (!InBase.IsValid() && !InOverride.IsValid())
	{
		return nullptr;
	}
	const TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	for (const TSharedPtr<FJsonObject>& Source : {InBase, InOverride})
	{
		if (!Source.IsValid())
		{
			continue;
		}
		for (const auto& Pair : Source->Values)
		{
			Out->SetField(GAMETEST_JSON_KEY(Pair.Key), Pair.Value);
		}
	}
	return Out;
}

namespace GameTestHandlers
{
	EGameTestStepStatus HandleMove(FGameTestStepContext& InContext)
	{
		using namespace GameTestMoveDetail;
		const FJsonObject& Params = *InContext.Step->Params;

		FString Error;
		UWorld* World = ResolveStepWorld(InContext, Error);
		if (World == nullptr)
		{
			InContext.Message = Error;
			return StepFailed;
		}
		APawn* Pawn = GameTestMove::FindLocalPawn(*World);
		if (Pawn == nullptr)
		{
			InContext.Message = FString::Printf(TEXT("%s 里没有本地玩家的角色（还没生成，或在观战 / 菜单里）"), *GameTestWorld::DescribeWorld(World));
			return StepFailed;
		}

		const TSharedRef<FMover> Mover = MakeShared<FMover>();
		Mover->World = World;
		Mover->Pawn = Pawn;
		Mover->Payload = InContext.Payload;

		// 设置：运行设置 / 场景的 move 段在前，步骤字段盖在上面。
		ReadSettings(InContext.MoveDefaults.Get(), Mover->Settings);
		ReadSettings(&Params, Mover->Settings);
		FMoveSettings& Settings = Mover->Settings;
		if (!OneOf(Settings.Via, {TEXT("auto"), TEXT("nav"), TEXT("path"), TEXT("probe"), TEXT("teleport")}))
		{
			InContext.Message = FString::Printf(TEXT("move 的 via 不认识 %s（auto / nav / path / probe / teleport）"), *Settings.Via);
			return StepFailed;
		}
		if (!OneOf(Settings.Fallback, {TEXT("none"), TEXT("teleport")}) || !OneOf(Settings.Drive, {TEXT("input"), TEXT("keys")}))
		{
			InContext.Message = TEXT("move 的 fallback 只能是 none / teleport，drive 只能是 input / keys");
			return StepFailed;
		}
		if (Settings.Drive == TEXT("keys") && !FKey(*Settings.ForwardKey).IsValid())
		{
			InContext.Message = FString::Printf(TEXT("move 的 forwardKey 不是 UE 的键名：%s"), *Settings.ForwardKey);
			return StepFailed;
		}

		// 目标：to 坐标，或 toActor / toTag 找一个 actor（每帧跟着它的位置）。
		FString ActorClass;
		FString ActorTag;
		GameTestJson::TryGetString(Params, TEXT("toActor"), ActorClass);
		GameTestJson::TryGetString(Params, TEXT("toTag"), ActorTag);
		const TSharedPtr<FJsonValue>* ToValue = Params.Values.Find(TEXT("to"));
		FString GoalLabel;
		if (ToValue != nullptr)
		{
			if (!GameTestTrajectory::PointFromJson(*ToValue, Mover->Goal))
			{
				InContext.Message = TEXT("move 的 to 要写 [x, y, z]（世界坐标，厘米）");
				return StepFailed;
			}
			GoalLabel = FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Mover->Goal.X, Mover->Goal.Y, Mover->Goal.Z);
		}
		else if (!ActorClass.IsEmpty() || !ActorTag.IsEmpty())
		{
			AActor* Target = GameTestActor::ResolveOne(*World, ActorClass, ActorTag, TOptional<bool>(), Error);
			if (Target == nullptr)
			{
				InContext.Message = FString::Printf(TEXT("move 找目标 actor：%s"), *Error);
				return StepFailed;
			}
			Mover->GoalActor = Target;
			Mover->Goal = Target->GetActorLocation();
			GoalLabel = Target->GetName();
		}
		else
		{
			InContext.Message = TEXT("move 需要 to（[x, y, z]）或 toActor / toTag（走到某个 actor 旁边）");
			return StepFailed;
		}

		double Number = 0.0;
		if (GameTestJson::TryGetNumber(Params, TEXT("yaw"), Number))
		{
			Mover->Yaw = Number;
		}
		if (GameTestJson::TryGetNumber(Params, TEXT("pitch"), Number))
		{
			Mover->Pitch = Number;
		}

		float PawnRadius = 34.0f;
		float PawnHalfHeight = 88.0f;
		Pawn->GetSimpleCollisionCylinder(PawnRadius, PawnHalfHeight);
		Mover->Radius = Settings.Radius > 0.0 ? Settings.Radius
			: Mover->GoalActor.IsValid() ? ActorGoalRadius(*Mover->GoalActor.Get(), *Pawn)
			: FMath::Max<double>(PawnRadius * 2.0, 60.0);

		if (const TArray<TSharedPtr<FJsonValue>>* PathValues = GameTestJson::TryGetArray(Params, TEXT("path")))
		{
			if (!GameTestTrajectory::PathFromJson(*PathValues, Mover->Recorded, Error))
			{
				InContext.Message = FString::Printf(TEXT("move：%s"), *Error);
				return StepFailed;
			}
			Mover->RecordedJumps = GameTestTrajectory::IndicesFromJson(GameTestJson::TryGetArray(Params, TEXT("jumps")), Mover->Recorded.Num());
			Mover->RecordedFly = GameTestTrajectory::IndicesFromJson(GameTestJson::TryGetArray(Params, TEXT("fly")), Mover->Recorded.Num());
		}

		// 走法顺序：auto = 导航网格 → 按轨迹 → 探路；fallback=teleport 时最后瞬移。
		if (Settings.Via == TEXT("auto"))
		{
			Mover->Ways = {EWay::Nav};
			if (Mover->Recorded.Num() >= 2)
			{
				Mover->Ways.Add(EWay::Path);
			}
			Mover->Ways.Add(EWay::Probe);
		}
		else if (Settings.Via == TEXT("nav"))
		{
			Mover->Ways = {EWay::Nav};
		}
		else if (Settings.Via == TEXT("path"))
		{
			Mover->Ways = {EWay::Path};
		}
		else if (Settings.Via == TEXT("probe"))
		{
			Mover->Ways = {EWay::Probe};
		}
		if (Settings.Via == TEXT("teleport") || Settings.Fallback == TEXT("teleport"))
		{
			Mover->Ways.Add(EWay::Teleport);
		}

		InContext.DefaultTimeoutMs = EstimateTimeoutMs(Params, *Pawn, Mover->Goal, Mover->Recorded);
		InContext.PendingProgress = Mover->Progress;
		InContext.PendingFailure = Mover->Failure;
		InContext.PendingPredicate = [Mover](double InNow) { return Mover->Tick(InNow); };
		InContext.PendingCleanup = [Mover]() { Mover->Cleanup(); };
		InContext.Message = FString::Printf(TEXT("走到 %s"), *GoalLabel);
		return StepPending;
	}
}
