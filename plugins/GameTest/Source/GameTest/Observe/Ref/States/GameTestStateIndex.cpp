// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

#include "Components/ListView.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"

namespace GameTestStateIndexImpl
{
	/** `:index`：条目在容器里第几个（从 0 起；只用于显示，不建议当标识）。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("index");
		}

		virtual EGameTestStateScope GetScope() const override
		{
			return EGameTestStateScope::Widget;
		}

		virtual EGameTestKind GetKind() const override
		{
			return EGameTestKind::Int;
		}

		virtual FString GetSummary() const override
		{
			return TEXT("条目在容器里第几个（从 0 起；只用于显示，不建议当标识）");
		}

		virtual bool IsStable() const override
		{
			return false;
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			if (InContext.Index != INDEX_NONE)
			{
				return GameTestStateUtil::Of(FGameTestValue::FromNumber(InContext.Index));
			}
			UObject* Item = nullptr;
			if (const UListView* List = GameTestStateUtil::OwningList(InObject, Item))
			{
				const int32 Found = Item != nullptr ? List->GetListItems().IndexOfByKey(Item) : INDEX_NONE;
				if (Found != INDEX_NONE)
				{
					return GameTestStateUtil::Of(FGameTestValue::FromNumber(Found));
				}
			}
			const UWidget* Widget = GameTestStateUtil::WidgetOf(InObject);
			if (const UPanelWidget* Parent = Widget != nullptr ? Widget->GetParent() : nullptr)
			{
				return GameTestStateUtil::Of(FGameTestValue::FromNumber(Parent->GetChildIndex(Widget)));
			}
			return GameTestStateUtil::NotApplicable(InObject, TEXT("index"));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateIndex()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateIndexImpl::FState>());
	}
}
