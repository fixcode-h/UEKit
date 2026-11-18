// Copyright Epic Games, Inc. All Rights Reserved.

// 取值来源：@sub.<类名>（协议第 4.3 节）。GameInstance / World / LocalPlayer / Engine 四类子系统按类名找，U 前缀可省。

#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Sources/GameTestObjectRoots.h"
#include "Observe/Sources/GameTestValueSource.h"

namespace
{
	class FSubsystemSource final : public IGameTestValueSource
	{
	public:
		virtual FName GetKey() const override { return TEXT("sub"); }
		virtual FString GetSummary() const override { return TEXT("子系统（GameInstance / World / LocalPlayer / Engine，按类名找）"); }
		virtual FString GetSyntax() const override { return TEXT("@sub.<类名>.<属性>"); }

		virtual FGameTestValue Resolve(const FString& InSelector, TArrayView<const FGameTestPathSegment> InRest, int32& OutConsumed,
			FGameTestFlowContext& InContext) const override
		{
			OutConsumed = 0;
			if (InRest.Num() == 0 || InRest[0].Kind != FGameTestPathSegment::EKind::Member)
			{
				return FGameTestValue::Unresolved(TEXT("@sub 后面要写子系统的类名：@sub.UQuestSubsystem.ActiveQuests"), TEXT("expr_syntax"));
			}
			OutConsumed = 1;
			if (InContext.World == nullptr)
			{
				return FGameTestValue::Unresolved(TEXT("没有游戏世界（游戏没开？）"));
			}
			FString Error;
			UObject* Subsystem = GameTestObjectRoots::Subsystem(*InContext.World, InRest[0].Name, Error);
			return Subsystem != nullptr ? FGameTestValue::FromObject(Subsystem) : FGameTestValue::Unresolved(Error, TEXT("path_missing"));
		}

		virtual void Validate(const FString& InSelector, TArrayView<const FGameTestPathSegment> InRest, FGameTestDiagSink& InSink) const override
		{
			if (InRest.Num() == 0 || InRest[0].Kind != FGameTestPathSegment::EKind::Member)
			{
				InSink.Error(TEXT("expr_syntax"), TEXT("@sub 后面要写子系统的类名：@sub.UQuestSubsystem.ActiveQuests"));
			}
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterSourceSubsystem()
	{
		GameTestValueSources::Get().Register(MakeShared<FSubsystemSource>());
	}
}
