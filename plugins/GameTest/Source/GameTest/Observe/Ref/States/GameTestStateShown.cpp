// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

#include "Components/SlateWrapperTypes.h"
#include "Components/Widget.h"

namespace GameTestStateShownImpl
{
	/** `:shown`：自己和父链都不是 Collapsed / Hidden。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("shown");
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
			return TEXT("自己和父链都不是 Collapsed / Hidden");
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			UWidget* Widget = GameTestStateUtil::WidgetOf(InObject);
			if (Widget == nullptr)
			{
				return GameTestStateUtil::NotApplicable(InObject, TEXT("shown"));
			}
			int32 Guard = 0;
			for (const UWidget* Current = Widget; Current != nullptr && Guard < 256; Current = GameTestStateUtil::ParentOf(Current), ++Guard)
			{
				const ESlateVisibility Visibility = Current->GetVisibility();
				if (Visibility == ESlateVisibility::Collapsed || Visibility == ESlateVisibility::Hidden)
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
	void RegisterStateShown()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateShownImpl::FState>());
	}
}
