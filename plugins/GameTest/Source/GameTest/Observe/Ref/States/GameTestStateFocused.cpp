// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

#include "Components/Widget.h"
#include "Widgets/SWidget.h"

namespace GameTestStateFocusedImpl
{
	/** `:focused`：有键盘焦点。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("focused");
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
			return TEXT("有键盘焦点");
		}

		virtual bool IsStable() const override
		{
			return false;
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			const UWidget* Widget = GameTestStateUtil::WidgetOf(InObject);
			if (Widget == nullptr)
			{
				return GameTestStateUtil::NotApplicable(InObject, TEXT("focused"));
			}
			const TSharedPtr<SWidget> Cached = Widget->GetCachedWidget();
			const bool bFocused = Cached.IsValid() && Cached->HasAnyUserFocus().IsSet();
			return GameTestStateUtil::Of(FGameTestValue::FromBool(bFocused));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateFocused()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateFocusedImpl::FState>());
	}
}
