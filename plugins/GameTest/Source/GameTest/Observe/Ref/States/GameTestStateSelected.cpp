// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

#include "Components/ListView.h"

namespace GameTestStateSelectedImpl
{
	/** `:selected`：列表条目是否选中。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("selected");
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
			return TEXT("列表条目是否选中");
		}

		virtual bool IsStable() const override
		{
			return false;
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			UObject* Item = InContext.Item;
			UObject* EntryItem = nullptr;
			const UListView* List = GameTestStateUtil::OwningList(InObject, EntryItem);
			Item = Item != nullptr ? Item : EntryItem;
			if (List == nullptr || Item == nullptr)
			{
				return GameTestStateUtil::NotApplicable(InObject, TEXT("selected"));
			}
			return GameTestStateUtil::Of(FGameTestValue::FromBool(List->GetSelectedItem() == Item));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateSelected()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateSelectedImpl::FState>());
	}
}
