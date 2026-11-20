// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

namespace GameTestStateClassImpl
{
	/** `:class`：实际类（比较用 classIs 按继承、nameIs 按名字）。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("class");
		}

		virtual EGameTestStateScope GetScope() const override
		{
			return EGameTestStateScope::Any;
		}

		virtual EGameTestKind GetKind() const override
		{
			return EGameTestKind::Object;
		}

		virtual FString GetSummary() const override
		{
			return TEXT("实际类（比较用 classIs 按继承、nameIs 按名字）");
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			if (InObject == nullptr)
			{
				return GameTestStateUtil::NotApplicable(InObject, TEXT("class"));
			}
			return GameTestStateUtil::Of(FGameTestValue::FromObject(InObject->GetClass()));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateClass()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateClassImpl::FState>());
	}
}
