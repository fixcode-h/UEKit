// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

#include "Components/Widget.h"

namespace GameTestStateEnabledImpl
{
	/** `:enabled`：自己和父链都 enabled。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("enabled");
		}

		virtual EGameTestStateScope GetScope() const override
		{
			return EGameTestStateScope::Widget;
		}

		virtual EGameTestKind GetKind() const override
		{
			return EGameTestKind::Bool;
		}

		virtual FString GetSummary() const override
		{
			return TEXT("自己和父链都 enabled");
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			UWidget* Widget = GameTestStateUtil::WidgetOf(InObject);
			if (Widget == nullptr)
			{
				return GameTestStateUtil::NotApplicable(InObject, TEXT("enabled"));
			}
			int32 Guard = 0;
			for (const UWidget* Current = Widget; Current != nullptr && Guard < 256; Current = GameTestStateUtil::ParentOf(Current), ++Guard)
			{
				if (!Current->GetIsEnabled())
				{
					return GameTestStateUtil::Of(FGameTestValue::FromBool(false));
				}
			}
			return GameTestStateUtil::Of(FGameTestValue::FromBool(true));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateEnabled()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateEnabledImpl::FState>());
	}
}
