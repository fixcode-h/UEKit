// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

#include "Components/Widget.h"
#include "Widgets/SWidget.h"

namespace GameTestStateHoveredImpl
{
	/** `:hovered`：光标在上面。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("hovered");
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
			return TEXT("光标在上面");
		}

		virtual bool IsStable() const override
		{
			return false;
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			const UWidget* Widget = GameTestStateUtil::WidgetOf(InObject);
			const TSharedPtr<SWidget> Slate = Widget != nullptr ? Widget->GetCachedWidget() : nullptr;
			if (Widget == nullptr)
			{
				return GameTestStateUtil::NotApplicable(InObject, TEXT("hovered"));
			}
			return GameTestStateUtil::Of(FGameTestValue::FromBool(Slate.IsValid() && Slate->IsHovered()));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateHovered()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateHoveredImpl::FState>());
	}
}
