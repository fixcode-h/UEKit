// Copyright Epic Games, Inc. All Rights Reserved.

// 取值来源：@cvar.<名字>（协议第 4.3 节）。控制台变量名本身带点（r.ScreenPercentage），所以吃掉后面全部的成员段。

#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Sources/GameTestValueSource.h"

#include "HAL/IConsoleManager.h"

namespace
{
	class FCVarSource final : public IGameTestValueSource
	{
	public:
		virtual FName GetKey() const override { return TEXT("cvar"); }
		virtual FString GetSummary() const override { return TEXT("控制台变量"); }
		virtual FString GetSyntax() const override { return TEXT("@cvar.<名字>（如 @cvar.r.ScreenPercentage）"); }

		virtual FGameTestValue Resolve(const FString& InSelector, TArrayView<const FGameTestPathSegment> InRest, int32& OutConsumed,
			FGameTestFlowContext& InContext) const override
		{
			OutConsumed = 0;
			TArray<FString> Parts;
			for (const FGameTestPathSegment& Segment : InRest)
			{
				if (Segment.Kind != FGameTestPathSegment::EKind::Member)
				{
					break;
				}
				Parts.Add(Segment.Name);
				++OutConsumed;
			}
			const FString Name = FString::Join(Parts, TEXT("."));
			if (Name.IsEmpty())
			{
				return FGameTestValue::Unresolved(TEXT("@cvar 后面要写变量名：@cvar.r.ScreenPercentage"), TEXT("expr_syntax"));
			}
			return Read(Name);
		}

		/**
		 * 按文字给值：像数字的是数字，true / false 是布尔，其余是文字。
		 * 不用 IsVariableInt 这类按类型的接口：各引擎版本上有没有不一定，文字在哪个版本都一样。
		 */
		static FGameTestValue Read(const FString& InName)
		{
			IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(*InName);
			if (Var == nullptr)
			{
				return FGameTestValue::Unresolved(FString::Printf(TEXT("没有控制台变量 %s"), *InName), TEXT("path_missing"));
			}
			const FString Text = Var->GetString();
			if (Text.IsNumeric())
			{
				return FGameTestValue::FromNumber(FCString::Atod(*Text));
			}
			if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("false"), ESearchCase::IgnoreCase))
			{
				return FGameTestValue::FromBool(Text.Equals(TEXT("true"), ESearchCase::IgnoreCase));
			}
			return FGameTestValue::FromString(Text);
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterSourceCVar()
	{
		GameTestValueSources::Get().Register(MakeShared<FCVarSource>());
	}
}
