// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"
#include "Observe/GameTestSlate.h"

#include "Components/Widget.h"
#include "Widgets/SWidget.h"

namespace GameTestStateSizeImpl
{
	/** `:size`：视口归一化大小（0–1）。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("size");
		}

		virtual EGameTestStateScope GetScope() const override
		{
			return EGameTestStateScope::Widget;
		}

		virtual EGameTestKind GetKind() const override
		{
			return EGameTestKind::Vector2;
		}

		virtual FString GetSummary() const override
		{
			return TEXT("视口归一化大小（0–1）");
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
				return GameTestStateUtil::NotApplicable(InObject, TEXT("size"));
			}
			FVector2D Min = FVector2D::ZeroVector;
			FVector2D Size = FVector2D::ZeroVector;
			if (!Slate.IsValid() || !GameTestSlate::GetViewportRect(*Slate, Min, Size))
			{
				return GameTestStateUtil::Of(FGameTestValue::Unresolved(FString::Printf(TEXT("%s 不在视口里（没画出来或没有尺寸）"), *Widget->GetName())));
			}
			const FVector2D Value = Size;
			TMap<FString, FGameTestValue> Fields;
			Fields.Add(TEXT("X"), FGameTestValue::FromNumber(Value.X));
			Fields.Add(TEXT("Y"), FGameTestValue::FromNumber(Value.Y));
			return GameTestStateUtil::Of(FGameTestValue::FromMap(MoveTemp(Fields)));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateSize()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateSizeImpl::FState>());
	}
}
