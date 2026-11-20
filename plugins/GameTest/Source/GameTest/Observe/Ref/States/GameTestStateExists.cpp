// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

namespace GameTestStateExistsImpl
{
	/** `:exists`：引用能解析到（部件层 = 部件在）。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("exists");
		}

		virtual EGameTestStateScope GetScope() const override
		{
			return EGameTestStateScope::Any;
		}

		virtual EGameTestKind GetKind() const override
		{
			return EGameTestKind::Bool;
		}

		virtual FString GetSummary() const override
		{
			return TEXT("引用能解析到（部件层 = 部件在）");
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			return GameTestStateUtil::Of(FGameTestValue::FromBool(InObject != nullptr));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateExists()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateExistsImpl::FState>());
	}
}
