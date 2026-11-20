// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

#include "Components/Image.h"

namespace GameTestStateImageImpl
{
	/** `:image`：Image 当前 Brush 的资源名。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("image");
		}

		virtual EGameTestStateScope GetScope() const override
		{
			return EGameTestStateScope::Widget;
		}

		virtual EGameTestKind GetKind() const override
		{
			return EGameTestKind::Asset;
		}

		virtual FString GetSummary() const override
		{
			return TEXT("Image 当前 Brush 的资源名");
		}

		virtual bool AppliesTo(const UObject* InObject) const override
		{
			return InObject != nullptr && InObject->IsA<UImage>();
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			const UImage* Image = Cast<UImage>(InObject);
			if (Image == nullptr)
			{
				return GameTestStateUtil::NotApplicable(InObject, TEXT("image"));
			}
			UObject* Resource = Image->GetBrush().GetResourceObject();
			return GameTestStateUtil::Of(Resource != nullptr ? FGameTestValue::FromString(Resource->GetPathName()) : FGameTestValue::Null());
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateImage()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateImageImpl::FState>());
	}
}
