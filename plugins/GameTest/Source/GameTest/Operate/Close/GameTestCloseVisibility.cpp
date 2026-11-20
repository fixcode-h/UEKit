// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/Close/GameTestCloser.h"

#include "Core/Script/GameTestDiag.h"
#include "Dom/JsonObject.h"

#include "Components/SlateWrapperTypes.h"
#include "Components/Widget.h"

namespace GameTestCloseVisibilityImpl
{
	class FCloser final : public IGameTestCloser
	{
	public:
		virtual FName GetKey() const override { return TEXT("visibility"); }
		virtual FString GetSummary() const override { return TEXT("SetVisibility（value：Collapsed 默认 / Hidden）"); }
		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("value"), TEXT("part") };
			return Fields;
		}

		virtual void Validate(const FJsonObject& InHow, FGameTestDiagSink& InSink) const override
		{
			FString Value;
			if (InHow.TryGetStringField(TEXT("value"), Value) && Value != TEXT("Collapsed") && Value != TEXT("Hidden"))
			{
				InSink.Error(TEXT("bad_type"), FString::Printf(TEXT("by=visibility 的 value 只能是 Collapsed 或 Hidden，写的是 %s"), *Value));
			}
		}

		virtual bool Close(FGameTestCloseRun& InRun) const override
		{
			UWidget* Widget = Cast<UWidget>(InRun.PartOrSelf());
			if (Widget == nullptr)
			{
				if (InRun.Message.IsEmpty())
				{
					InRun.Message = TEXT("by=visibility 只能用在界面控件上");
				}
				return false;
			}
			FString Value = TEXT("Collapsed");
			InRun.How.TryGetStringField(TEXT("value"), Value);
			Widget->SetVisibility(Value == TEXT("Hidden") ? ESlateVisibility::Hidden : ESlateVisibility::Collapsed);
			InRun.Message = FString::Printf(TEXT("把 %s 设成 %s"), *Widget->GetName(), *Value);
			return true;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCloseVisibility()
	{
		GameTestClosers::Get().Register(MakeShared<GameTestCloseVisibilityImpl::FCloser>());
	}
}
