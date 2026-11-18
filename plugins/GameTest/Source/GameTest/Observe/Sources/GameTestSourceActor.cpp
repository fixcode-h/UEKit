// Copyright Epic Games, Inc. All Rights Reserved.

// 取值来源：@actor[tag=X] / [class=BP_X] / [name=大纲名]（协议第 4.3 节）：世界里唯一命中的 Actor，多个时报错并列出。
// 要在多个里挑一个时用对象定位器（objects + key / near），不在这里猜。

#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Sources/GameTestObjectRoots.h"
#include "Observe/Sources/GameTestValueSource.h"

namespace
{
	bool SplitSelector(const FString& InSelector, FString& OutBy, FString& OutValue)
	{
		if (!InSelector.Split(TEXT("="), &OutBy, &OutValue))
		{
			return false;
		}
		OutBy.TrimStartAndEndInline();
		OutValue.TrimStartAndEndInline();
		OutValue.TrimQuotesInline();
		return !OutBy.IsEmpty() && !OutValue.IsEmpty();
	}

	class FActorSource final : public IGameTestValueSource
	{
	public:
		virtual FName GetKey() const override { return TEXT("actor"); }
		virtual FString GetSummary() const override { return TEXT("世界里唯一命中的 Actor（按标签、类、大纲名）"); }
		virtual FString GetSyntax() const override { return TEXT("@actor[tag=X].<属性>、@actor[class=BP_X]、@actor[name=大纲名]"); }
		virtual bool UsesSelector() const override { return true; }

		virtual FGameTestValue Resolve(const FString& InSelector, TArrayView<const FGameTestPathSegment> InRest, int32& OutConsumed,
			FGameTestFlowContext& InContext) const override
		{
			OutConsumed = 0;
			FString By;
			FString Value;
			if (!SplitSelector(InSelector, By, Value))
			{
				return FGameTestValue::Unresolved(TEXT("@actor 要写 [tag=X]、[class=BP_X] 或 [name=大纲名]"), TEXT("expr_syntax"));
			}
			if (InContext.World == nullptr)
			{
				return FGameTestValue::Unresolved(TEXT("没有游戏世界（游戏没开？）"));
			}
			FString Error;
			AActor* Actor = GameTestObjectRoots::UniqueActor(*InContext.World, By, Value, Error);
			return Actor != nullptr ? FGameTestValue::FromObject(Actor) : FGameTestValue::Unresolved(Error, TEXT("object_not_found"));
		}

		virtual void Validate(const FString& InSelector, TArrayView<const FGameTestPathSegment> InRest, FGameTestDiagSink& InSink) const override
		{
			FString By;
			FString Value;
			if (!SplitSelector(InSelector, By, Value))
			{
				InSink.Error(TEXT("expr_syntax"), TEXT("@actor 要写 [tag=X]、[class=BP_X] 或 [name=大纲名]"));
			}
			else if (!By.Equals(TEXT("tag"), ESearchCase::IgnoreCase) && !By.Equals(TEXT("class"), ESearchCase::IgnoreCase)
				&& !By.Equals(TEXT("name"), ESearchCase::IgnoreCase))
			{
				InSink.Error(TEXT("expr_syntax"), FString::Printf(TEXT("@actor[%s=…] 不认识：只能按 tag、class、name 找"), *By));
			}
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterSourceActor()
	{
		GameTestValueSources::Get().Register(MakeShared<FActorSource>());
	}
}
