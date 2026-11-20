// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"
#include "Observe/Sources/GameTestObjectRoots.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace GameTestStateDistanceImpl
{
	/** `:distance`：离本地 Pawn 的距离（厘米）。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("distance");
		}

		virtual EGameTestStateScope GetScope() const override
		{
			return EGameTestStateScope::Actor;
		}

		virtual EGameTestKind GetKind() const override
		{
			return EGameTestKind::Float;
		}

		virtual FString GetSummary() const override
		{
			return TEXT("离本地 Pawn 的距离（厘米）");
		}

		virtual bool IsStable() const override
		{
			return false;
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			FVector Location = FVector::ZeroVector;
			if (!GameTestStateUtil::LocationOf(InObject, Location))
			{
				return GameTestStateUtil::NotApplicable(InObject, TEXT("distance"));
			}
			UWorld* World = InObject->GetWorld();
			FString Error;
			const AActor* Pawn = World != nullptr ? Cast<AActor>(GameTestObjectRoots::Player(*World, TEXT("pawn"), Error)) : nullptr;
			if (Pawn == nullptr)
			{
				return GameTestStateUtil::Of(FGameTestValue::Unresolved(Error.IsEmpty() ? FString(TEXT("没有本地 Pawn，算不了距离")) : Error));
			}
			return GameTestStateUtil::Of(FGameTestValue::FromNumber(FVector::Dist(Location, Pawn->GetActorLocation())));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateDistance()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateDistanceImpl::FState>());
	}
}
