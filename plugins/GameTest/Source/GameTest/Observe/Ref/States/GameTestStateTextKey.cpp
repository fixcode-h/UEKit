// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"
#include "GameTestCompat.h"

#include "Components/Widget.h"

namespace GameTestStateTextKeyImpl
{
	/** `:textKey`：FText 的本地化键「命名空间,键」（没有键时取不到）。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("textKey");
		}

		virtual EGameTestStateScope GetScope() const override
		{
			return EGameTestStateScope::Widget;
		}

		virtual EGameTestKind GetKind() const override
		{
			return EGameTestKind::String;
		}

		virtual FString GetSummary() const override
		{
			return TEXT("FText 的本地化键「命名空间,键」（没有键时取不到）");
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			UWidget* Widget = GameTestStateUtil::WidgetOf(InObject);
			FText Text;
			if (Widget == nullptr || !GameTestStateUtil::TextWidgetText(Widget, Text))
			{
				return GameTestStateUtil::NotApplicable(InObject, TEXT("textKey"));
			}
			FString Namespace;
			FString Key;
			if (!GameTestCompat::TextKeyOf(Text, Namespace, Key))
			{
				return GameTestStateUtil::Of(FGameTestValue::Unresolved(FString::Printf(TEXT("%s 的文字没有本地化键（是运行时拼出来的或不可本地化的字）"), *Widget->GetName())));
			}
			FGameTestStateValue Out;
			Out.Value = FGameTestValue::FromString(Namespace + TEXT(",") + Key);
			Out.TextKey = Out.Value.GetString();
			return Out;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateTextKey()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateTextKeyImpl::FState>());
	}
}
