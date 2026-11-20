// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

#include "Components/DynamicEntryBox.h"
#include "Components/ListView.h"
#include "Components/PanelWidget.h"

namespace GameTestStateCountImpl
{
	/** `:count`：容器的子项数 / 列表的条目数。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("count");
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
			return TEXT("容器的子项数 / 列表的条目数");
		}

		virtual bool AppliesTo(const UObject* InObject) const override
		{
			return InObject != nullptr && (InObject->IsA<UListView>() || InObject->IsA<UDynamicEntryBox>() || InObject->IsA<UPanelWidget>());
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			if (const UListView* List = Cast<UListView>(InObject))
			{
				return GameTestStateUtil::Of(FGameTestValue::FromNumber(List->GetNumItems()));
			}
			if (const UDynamicEntryBox* Box = Cast<UDynamicEntryBox>(InObject))
			{
				return GameTestStateUtil::Of(FGameTestValue::FromNumber(Box->GetNumEntries()));
			}
			if (const UPanelWidget* Panel = Cast<UPanelWidget>(InObject))
			{
				return GameTestStateUtil::Of(FGameTestValue::FromNumber(Panel->GetChildrenCount()));
			}
			return GameTestStateUtil::NotApplicable(InObject, TEXT("count"));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateCount()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateCountImpl::FState>());
	}
}
