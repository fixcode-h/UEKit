// Copyright Epic Games, Inc. All Rights Reserved.

// 自带接口：玩家、角色移动、Actor、动画（协议第 8.1 节 Player.* / Character.* / Actor.* / Anim.*）。

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Api/Builtin/GameTestApiArgs.h"
#include "Api/GameTestApi.h"
#include "Components/SkeletalMeshComponent.h"
#include "Core/Script/GameTestFlowContext.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Observe/Sources/GameTestObjectRoots.h"

namespace GA = GameTestApiArgs;
using GameTestApiArgs::Opt;
using GameTestApiArgs::Req;

namespace
{
	using FCall = FGameTestApiCall;
	using FCtx = FGameTestFlowContext;
	using EState = EGameTestCallState;

	/** @pc / @pawn / @ps：`client` 参数指定哪个客户端（省略 = 当前步骤的）。 */
	EState PlayerRoot(FCall& C, FCtx& X, const TCHAR* InWhich)
	{
		FString Error;
		UWorld* World = GA::ClientWorld(C.Arg(0), X, Error);
		if (World == nullptr)
		{
			return C.Fail(TEXT("engine_unavailable"), Error);
		}
		UObject* Found = GameTestObjectRoots::Player(*World, InWhich, Error);
		return Found ? C.Done(FGameTestValue::FromObject(Found)) : C.Fail(TEXT("object_not_found"), Error);
	}

	APawn* LocalPawn(FCall& C, FCtx& X)
	{
		FString Error;
		AActor* Pawn = GA::Actor(FGameTestValue::Null(), X, true, Error);
		if (Pawn == nullptr)
		{
			C.Fail(TEXT("object_not_found"), Error);
		}
		return Cast<APawn>(Pawn);
	}

	AActor* NeedActor(FCall& C, int32 InIndex, FCtx& X, bool bDefaultPawn)
	{
		FString Error;
		AActor* Found = GA::Actor(C.Arg(InIndex), X, bDefaultPawn, Error);
		if (Found == nullptr)
		{
			C.Fail(TEXT("arg_object"), FString::Printf(TEXT("第 %d 个参数：%s"), InIndex + 1, *Error));
		}
		return Found;
	}

	UCharacterMovementComponent* NeedMovement(FCall& C, FCtx& X)
	{
		AActor* Found = NeedActor(C, 0, X, true);
		if (Found == nullptr)
		{
			return nullptr;
		}
		const ACharacter* Character = Cast<ACharacter>(Found);
		UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
		if (Movement == nullptr)
		{
			C.Fail(TEXT("arg_object"), FString::Printf(TEXT("%s 不是带 CharacterMovement 的角色"), *GA::NameOf(Found)));
		}
		return Movement;
	}

	FGameTestValue Rotator(const FRotator& InRotator)
	{
		TMap<FString, FGameTestValue> Fields;
		Fields.Add(TEXT("Pitch"), FGameTestValue::FromNumber(InRotator.Pitch));
		Fields.Add(TEXT("Yaw"), FGameTestValue::FromNumber(InRotator.Yaw));
		Fields.Add(TEXT("Roll"), FGameTestValue::FromNumber(InRotator.Roll));
		return FGameTestValue::FromMap(MoveTemp(Fields));
	}

	/** 朝向参数：`[pitch, yaw, roll]`、`{Pitch, Yaw, Roll}` 或只给一个数当 yaw。 */
	bool ReadRotator(const FGameTestValue& InArg, FRotator& OutRotator)
	{
		double Yaw = 0.0;
		if (InArg.TryNumber(Yaw))
		{
			OutRotator = FRotator(0.0, Yaw, 0.0);
			return true;
		}
		const TArray<FGameTestValue>& Items = InArg.GetArray();
		if (Items.Num() == 3)
		{
			double P = 0, Y = 0, R = 0;
			if (Items[0].TryNumber(P) && Items[1].TryNumber(Y) && Items[2].TryNumber(R))
			{
				OutRotator = FRotator(P, Y, R);
				return true;
			}
		}
		FGameTestValue P, Y, R;
		if (InArg.TryGetField(TEXT("Yaw"), Y))
		{
			double Pitch = 0, YawValue = 0, Roll = 0;
			Y.TryNumber(YawValue);
			if (InArg.TryGetField(TEXT("Pitch"), P)) { P.TryNumber(Pitch); }
			if (InArg.TryGetField(TEXT("Roll"), R)) { R.TryNumber(Roll); }
			OutRotator = FRotator(Pitch, YawValue, Roll);
			return true;
		}
		return false;
	}

	EState Teleport(FCall& C, FCtx& X)
	{
		APawn* Pawn = LocalPawn(C, X);
		if (Pawn == nullptr)
		{
			return EState::Failed;
		}
		FVector Location;
		FString Error;
		if (!GA::Vector(C.Arg(0), X, Location, Error))
		{
			GA::ArgError(C, 0, TEXT("vec"), Error);
			return EState::Failed;
		}
		FRotator Rotation = Pawn->GetActorRotation();
		if (!C.Arg(1).IsNull() && !ReadRotator(C.Arg(1), Rotation))
		{
			GA::ArgError(C, 1, TEXT("rot"), TEXT("朝向写 yaw 数字、[pitch, yaw, roll] 或 {Pitch, Yaw, Roll}"));
			return EState::Failed;
		}
		// TeleportTo 会做碰撞推挤（落在墙里会被挪开），比 SetActorLocation 更接近「站到这里」；放不下时如实报失败
		if (!Pawn->TeleportTo(Location, Rotation))
		{
			return C.Fail(TEXT("unresolved"), FString::Printf(TEXT("%s 放不到 %s（被挡住了）"), *GA::NameOf(Pawn), *Location.ToString()));
		}
		if (AController* Controller = Pawn->GetController())
		{
			Controller->SetControlRotation(Rotation);
		}
		return C.Done(FGameTestValue::Null());
	}

	EState Near(FCall& C, FCtx& X)
	{
		if (GA::World(X, C) == nullptr)
		{
			return EState::Failed;
		}
		FString Error;
		UClass* Class = GA::Class(C.Arg(0), Error);
		if (Class == nullptr)
		{
			GA::ArgError(C, 0, TEXT("cls"), Error);
			return EState::Failed;
		}
		FVector Point;
		if (!GA::Vector(C.Arg(1), X, Point, Error))
		{
			GA::ArgError(C, 1, TEXT("point"), Error);
			return EState::Failed;
		}
		double Radius = 0.0;
		if (!C.Arg(2).TryNumber(Radius) || Radius <= 0.0)
		{
			GA::ArgError(C, 2, TEXT("radius"), TEXT("要正数（厘米）"));
			return EState::Failed;
		}
		TArray<TPair<double, AActor*>> Found;
		const double RadiusSq = Radius * Radius;
		for (TActorIterator<AActor> It(X.World, Class); It; ++It)
		{
			const double DistSq = FVector::DistSquared(It->GetActorLocation(), Point);
			if (DistSq <= RadiusSq)
			{
				Found.Emplace(DistSq, *It);
			}
		}
		// 近的在前：脚本常取第一个（离得最近的那个怪、那个 NPC）
		Found.Sort([](const TPair<double, AActor*>& A, const TPair<double, AActor*>& B) { return A.Key < B.Key; });
		TArray<FGameTestValue> Items;
		for (const TPair<double, AActor*>& Pair : Found)
		{
			Items.Add(FGameTestValue::FromObject(Pair.Value));
		}
		return C.Done(FGameTestValue::FromArray(MoveTemp(Items)));
	}

	UAnimInstance* NeedAnim(FCall& C, FCtx& X)
	{
		AActor* Found = NeedActor(C, 0, X, true);
		if (Found == nullptr)
		{
			return nullptr;
		}
		const ACharacter* Character = Cast<ACharacter>(Found);
		USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : Found->FindComponentByClass<USkeletalMeshComponent>();
		UAnimInstance* Anim = Mesh ? Mesh->GetAnimInstance() : nullptr;
		if (Anim == nullptr)
		{
			C.Fail(TEXT("arg_object"), FString::Printf(TEXT("%s 没有带动画蓝图的骨骼网格"), *GA::NameOf(Found)));
		}
		return Anim;
	}
}

namespace GameTestBuiltin
{
	void RegisterApiPlayer()
	{
		const TCHAR* Cat = TEXT("玩家");
		GameTestApis::Add(TEXT("Player.Pawn"), Cat, TEXT("本地玩家的 Pawn"), { Opt(TEXT("client"), TEXT("int"), TEXT("省略 = 当前客户端")) },
			TEXT("obj"), true, [](FCall& C, FCtx& X) { return PlayerRoot(C, X, TEXT("pawn")); });
		GameTestApis::Add(TEXT("Player.Controller"), Cat, TEXT("本地玩家的 PlayerController"), { Opt(TEXT("client"), TEXT("int")) },
			TEXT("obj"), true, [](FCall& C, FCtx& X) { return PlayerRoot(C, X, TEXT("pc")); });
		GameTestApis::Add(TEXT("Player.State"), Cat, TEXT("本地玩家的 PlayerState"), { Opt(TEXT("client"), TEXT("int")) },
			TEXT("obj"), true, [](FCall& C, FCtx& X) { return PlayerRoot(C, X, TEXT("ps")); });
		GameTestApis::Add(TEXT("Player.Location"), Cat, TEXT("本地 Pawn 的位置"), {}, TEXT("vec"), true,
			[](FCall& C, FCtx& X)
			{
				const APawn* Pawn = LocalPawn(C, X);
				return Pawn ? C.Done(FGameTestValue::FromVector(Pawn->GetActorLocation())) : EState::Failed;
			});
		GameTestApis::Add(TEXT("Player.Rotation"), Cat, TEXT("本地玩家的镜头朝向（控制器朝向）"), {}, TEXT("{Pitch, Yaw, Roll}"), true,
			[](FCall& C, FCtx& X)
			{
				const APawn* Pawn = LocalPawn(C, X);
				if (Pawn == nullptr)
				{
					return EState::Failed;
				}
				const AController* Controller = Pawn->GetController();
				return C.Done(Rotator(Controller ? Controller->GetControlRotation() : Pawn->GetActorRotation()));
			});
		GameTestApis::Add(TEXT("Player.Teleport"), Cat, TEXT("把本地 Pawn 放到某处（准备状态用；测走路用 move）"),
			{ Req(TEXT("vec"), TEXT("vector")), Opt(TEXT("rot"), TEXT("any"), TEXT("yaw 或 [pitch, yaw, roll]")) }, TEXT(""), false, &Teleport);
		GameTestApis::Add(TEXT("Player.Possessed"), Cat, TEXT("本地玩家现在控制的 Pawn 的类名（上载具、换角色后会变）"), {}, TEXT("string"), true,
			[](FCall& C, FCtx& X)
			{
				const APawn* Pawn = LocalPawn(C, X);
				return Pawn ? C.Done(FGameTestValue::FromString(Pawn->GetClass()->GetName())) : EState::Failed;
			});

		const TCHAR* MoveCat = TEXT("角色移动");
		GameTestApis::Add(TEXT("Character.Velocity"), MoveCat, TEXT("角色速度（默认本地 Pawn）"), { Opt(TEXT("obj"), TEXT("obj")) }, TEXT("vec"), true,
			[](FCall& C, FCtx& X)
			{
				const UCharacterMovementComponent* Movement = NeedMovement(C, X);
				return Movement ? C.Done(FGameTestValue::FromVector(Movement->Velocity)) : EState::Failed;
			});
		GameTestApis::Add(TEXT("Character.Speed"), MoveCat, TEXT("水平速度大小（厘米/秒）"), { Opt(TEXT("obj"), TEXT("obj")) }, TEXT("number"), true,
			[](FCall& C, FCtx& X)
			{
				const UCharacterMovementComponent* Movement = NeedMovement(C, X);
				return Movement ? C.Done(FGameTestValue::FromNumber(Movement->Velocity.Size2D())) : EState::Failed;
			});
		GameTestApis::Add(TEXT("Character.IsFalling"), MoveCat, TEXT("在不在空中（跳起、掉落）"), { Opt(TEXT("obj"), TEXT("obj")) }, TEXT("bool"), true,
			[](FCall& C, FCtx& X)
			{
				const UCharacterMovementComponent* Movement = NeedMovement(C, X);
				return Movement ? C.Done(FGameTestValue::FromBool(Movement->IsFalling())) : EState::Failed;
			});
		GameTestApis::Add(TEXT("Character.MovementMode"), MoveCat, TEXT("移动模式：Walking / Falling / Swimming / Flying / Custom / None"),
			{ Opt(TEXT("obj"), TEXT("obj")) }, TEXT("string"), true,
			[](FCall& C, FCtx& X)
			{
				const UCharacterMovementComponent* Movement = NeedMovement(C, X);
				if (Movement == nullptr)
				{
					return EState::Failed;
				}
				const UEnum* Enum = StaticEnum<EMovementMode>();
				FString Name = Enum ? Enum->GetNameStringByValue(Movement->MovementMode.GetValue()) : FString();
				Name.RemoveFromStart(TEXT("MOVE_"));
				return C.Done(FGameTestValue::FromString(Name));
			});

		const TCHAR* ActorCat = TEXT("Actor");
		GameTestApis::Add(TEXT("Actor.Location"), ActorCat, TEXT("Actor 的位置"), { Req(TEXT("obj"), TEXT("obj")) }, TEXT("vec"), true,
			[](FCall& C, FCtx& X)
			{
				const AActor* Found = NeedActor(C, 0, X, false);
				return Found ? C.Done(FGameTestValue::FromVector(Found->GetActorLocation())) : EState::Failed;
			});
		GameTestApis::Add(TEXT("Actor.Distance"), ActorCat, TEXT("两点 / 两个 Actor 之间的距离（厘米）"),
			{ Req(TEXT("a"), TEXT("any"), TEXT("Actor、位置或 @pawn")), Req(TEXT("b"), TEXT("any")) }, TEXT("number"), true,
			[](FCall& C, FCtx& X)
			{
				FVector A, B;
				FString Error;
				if (!GA::Vector(C.Arg(0), X, A, Error))
				{
					GA::ArgError(C, 0, TEXT("a"), Error);
					return EState::Failed;
				}
				if (!GA::Vector(C.Arg(1), X, B, Error))
				{
					GA::ArgError(C, 1, TEXT("b"), Error);
					return EState::Failed;
				}
				return C.Done(FGameTestValue::FromNumber(FVector::Dist(A, B)));
			});
		GameTestApis::Add(TEXT("Actor.Near"), ActorCat, TEXT("某点半径内的这类 Actor，近的在前"),
			{ Req(TEXT("cls"), TEXT("cls")), Req(TEXT("point"), TEXT("any"), TEXT("位置、Actor 或 @pawn")), Req(TEXT("radius"), TEXT("number")) },
			TEXT("[obj]"), true, &Near);
		GameTestApis::Add(TEXT("Actor.HasTag"), ActorCat, TEXT("Actor 有没有这个 Tag（Actor 的 Tags 数组，不是 GameplayTag）"),
			{ Req(TEXT("obj"), TEXT("obj")), Req(TEXT("tag"), TEXT("string")) }, TEXT("bool"), true,
			[](FCall& C, FCtx& X)
			{
				const AActor* Found = NeedActor(C, 0, X, false);
				return Found ? C.Done(FGameTestValue::FromBool(Found->ActorHasTag(FName(*GA::Text(C.Arg(1)))))) : EState::Failed;
			});
		GameTestApis::Add(TEXT("Actor.IsHidden"), ActorCat, TEXT("Actor 是否隐藏（游戏里隐藏）"), { Req(TEXT("obj"), TEXT("obj")) }, TEXT("bool"), true,
			[](FCall& C, FCtx& X)
			{
				const AActor* Found = NeedActor(C, 0, X, false);
				return Found ? C.Done(FGameTestValue::FromBool(Found->IsHidden())) : EState::Failed;
			});
		GameTestApis::Add(TEXT("Actor.Destroy"), ActorCat, TEXT("销毁 Actor（清场用）"), { Req(TEXT("obj"), TEXT("obj")) }, TEXT(""), false,
			[](FCall& C, FCtx& X)
			{
				AActor* Found = NeedActor(C, 0, X, false);
				if (Found == nullptr)
				{
					return EState::Failed;
				}
				return Found->Destroy() ? C.Done(FGameTestValue::Null())
					: C.Fail(TEXT("unresolved"), FString::Printf(TEXT("%s 销毁不了（客户端上的同步 Actor 要在服务器销毁）"), *GA::NameOf(Found)));
			});

		const TCHAR* AnimCat = TEXT("动画");
		GameTestApis::Add(TEXT("Anim.IsMontagePlaying"), AnimCat, TEXT("在不在播蒙太奇；给 montage 时只看这一个（名字或路径）"),
			{ Opt(TEXT("obj"), TEXT("obj"), TEXT("默认本地 Pawn")), Opt(TEXT("montage"), TEXT("string")) }, TEXT("bool"), true,
			[](FCall& C, FCtx& X)
			{
				const UAnimInstance* Anim = NeedAnim(C, X);
				if (Anim == nullptr)
				{
					return EState::Failed;
				}
				const FString Want = GA::Text(C.Arg(1));
				if (Want.IsEmpty())
				{
					return C.Done(FGameTestValue::FromBool(Anim->IsAnyMontagePlaying()));
				}
				const UAnimMontage* Current = Anim->GetCurrentActiveMontage();
				const bool bMatch = Current != nullptr && Anim->Montage_IsPlaying(Current)
					&& (Current->GetName().Equals(Want, ESearchCase::IgnoreCase) || Current->GetPathName().Equals(Want, ESearchCase::IgnoreCase));
				return C.Done(FGameTestValue::FromBool(bMatch));
			});
		GameTestApis::Add(TEXT("Anim.CurrentMontage"), AnimCat, TEXT("正在播的蒙太奇名字；没在播时为空字符串"),
			{ Opt(TEXT("obj"), TEXT("obj"), TEXT("默认本地 Pawn")) }, TEXT("string"), true,
			[](FCall& C, FCtx& X)
			{
				const UAnimInstance* Anim = NeedAnim(C, X);
				if (Anim == nullptr)
				{
					return EState::Failed;
				}
				const UAnimMontage* Current = Anim->GetCurrentActiveMontage();
				return C.Done(FGameTestValue::FromString(Current ? Current->GetName() : FString()));
			});
	}
}
