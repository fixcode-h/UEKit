// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

namespace GameTestStateLocationImpl
{
	/** `:location`：世界位置。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("location");
		}

		virtual EGameTestStateScope GetScope() const override
		{
			return EGameTestStateScope::Actor;
		}

		virtual EGameTestKind GetKind() const override
		{
			return EGameTestKind::Vector;
		}

		virtual FString GetSummary() const override
		{
			return TEXT("世界位置");
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
				return GameTestStateUtil::NotApplicable(InObject, TEXT("location"));
			}
			return GameTestStateUtil::Of(FGameTestValue::FromVector(Location));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateLocation()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateLocationImpl::FState>());
	}
}
