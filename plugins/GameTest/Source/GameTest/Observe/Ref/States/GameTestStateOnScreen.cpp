// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Observe/GameTestWidgetNode.h"

#include "Components/Widget.h"

namespace GameTestStateOnScreenImpl
{
	/** `:onScreen`：在视口里、整条父链可见、有尺寸。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("onScreen");
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
			return TEXT("在视口里、整条父链可见、有尺寸");
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			UWidget* Widget = GameTestStateUtil::WidgetOf(InObject);
			if (Widget == nullptr)
			{
				return GameTestStateUtil::NotApplicable(InObject, TEXT("onScreen"));
			}
			FGameTestWidgetNode Node;
			Node.Widget = Widget;
			return GameTestStateUtil::Of(FGameTestValue::FromBool(GameTestWidgetProbe::IsOnScreen(Node)));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateOnScreen()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateOnScreenImpl::FState>());
	}
}
