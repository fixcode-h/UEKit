// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Observe/Checks/GameTestCheck.h"
#include "Observe/Checks/GameTestWidgetProbe.h"

/**
 * 判断：界面在不在屏上 `{"target": "WBP_Guide:.", "onScreen": true}`（守护最常用）。
 * 按继承 UUserWidget 认（子类也算，用户 2026-11-18），同一界面开了几个实例时有一个在屏上就算在。
 * `onScreen: false` 在界面根本没开时照常成立（协议第 4.2 节），所以不算反面判断。
 */
namespace
{
	FString ScreenName(const FString& InTarget)
	{
		const FString FromSelector = GameTestWidgetProbe::ScreenOf(InTarget);
		return FromSelector.IsEmpty() ? InTarget : FromSelector;
	}

	class FCheckOnScreen : public IGameTestCheck
	{
	public:
		virtual FName GetKey() const override { return TEXT("onScreen"); }
		virtual FString GetSummary() const override { return TEXT("界面在不在屏上（在视口里、整条父链可见、有尺寸）"); }
		virtual bool Matches(const FJsonObject& InJson) const override { return InJson.HasField(TEXT("onScreen")); }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("target"), TEXT("onScreen") };
			return Fields;
		}

		virtual void Validate(const FJsonObject& InJson, FGameTestDiagSink& InSink) const override
		{
			FString Target;
			bool bOnScreen = false;
			if (!InJson.TryGetStringField(TEXT("target"), Target) || Target.IsEmpty())
			{
				InSink.Error(TEXT("bad_type"), TEXT("onScreen 要配 target（界面：WBP_X:. 或类名）"));
			}
			if (!InJson.TryGetBoolField(TEXT("onScreen"), bOnScreen))
			{
				InSink.Error(TEXT("bad_type"), TEXT("onScreen 要写 true / false"));
			}
		}

		virtual void Evaluate(const FJsonObject& InJson, FGameTestFlowContext& InContext, FGameTestCheckResult& OutResult) const override
		{
			FString Raw;
			FString Target;
			FString Error;
			bool bWant = true;
			InJson.TryGetStringField(TEXT("target"), Raw);
			InJson.TryGetBoolField(TEXT("onScreen"), bWant);
			if (!GameTestRefs::EvalText(Raw, InContext, Target, Error))
			{
				OutResult.Unresolved = Error;
				return;
			}
			int32 Instances = 0;
			const bool bShown = GameTestWidgetProbe::IsScreenShown(ScreenName(Target), GameTestWidgetProbe::WorldOf(InContext), &Instances);
			OutResult.Actual = FGameTestValue::FromBool(bShown);
			OutResult.bHolds = bShown == bWant;
			OutResult.Say = bShown ? FString::Printf(TEXT("%s 在屏上（%d 个实例）"), *ScreenName(Target), Instances)
				: Instances > 0 ? FString::Printf(TEXT("%s 开着但不在屏上（隐藏、没尺寸或被父级挡掉）"), *ScreenName(Target))
				: FString::Printf(TEXT("%s 没打开"), *ScreenName(Target));
		}

		virtual FString Describe(const FJsonObject& InJson) const override
		{
			FString Target;
			bool bWant = true;
			InJson.TryGetStringField(TEXT("target"), Target);
			InJson.TryGetBoolField(TEXT("onScreen"), bWant);
			return FString::Printf(TEXT("界面 %s %s"), *ScreenName(Target), bWant ? TEXT("在屏上") : TEXT("不在屏上"));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCheckOnScreen()
	{
		GameTestChecks::Get().Register(MakeShared<FCheckOnScreen>());
	}
}
