// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

#include "Components/CheckBox.h"

namespace GameTestStateCheckedImpl
{
	/** `:checked`：CheckBox 的勾选状态（ECheckBoxState：Unchecked / Checked / Undetermined）。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("checked");
		}

		virtual EGameTestStateScope GetScope() const override
		{
			return EGameTestStateScope::Widget;
		}

		virtual EGameTestKind GetKind() const override
		{
			return EGameTestKind::Enum;
		}

		virtual FString GetSummary() const override
		{
			return TEXT("CheckBox 的勾选状态（ECheckBoxState：Unchecked / Checked / Undetermined）");
		}

		virtual bool IsStable() const override
		{
			return false;
		}

		virtual bool AppliesTo(const UObject* InObject) const override
		{
			return InObject != nullptr && InObject->IsA<UCheckBox>();
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			const UCheckBox* CheckBox = Cast<UCheckBox>(InObject);
			if (CheckBox == nullptr)
			{
				return GameTestStateUtil::NotApplicable(InObject, TEXT("checked"));
			}
			const UEnum* Enum = StaticEnum<ECheckBoxState>();
			const int64 Raw = static_cast<int64>(CheckBox->GetCheckedState());
			return GameTestStateUtil::Of(FGameTestValue::FromString(Enum != nullptr ? Enum->GetNameStringByValue(Raw) : FString::FromInt(static_cast<int32>(Raw))));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateChecked()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateCheckedImpl::FState>());
	}
}
