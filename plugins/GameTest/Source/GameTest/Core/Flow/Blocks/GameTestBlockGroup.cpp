// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/Blocks/GameTestBlockUtil.h"

/** `group`：步骤段（录制的段、手工分的段）。只是把几步装在一起，给它们一起加守护、onFail、timeoutMs。 */
namespace
{
	class FBlockGroup : public IGameTestBlock
	{
	public:
		virtual FName GetKey() const override { return TEXT("group"); }
		virtual FString GetSummary() const override { return TEXT("步骤段：几步装在一起，可以一起加守护、超时、失败处理"); }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("name"), TEXT("do"), TEXT("recorded") };
			return Fields;
		}

		virtual void Parse(FGameTestNode& InOutNode, FGameTestScriptParser& InParser) const override
		{
			InParser.ParseList(InOutNode, TEXT("do"), InOutNode.Json->TryGetField(TEXT("do")), true);
		}

		virtual TSharedRef<FGameTestBlockState> Enter(const FGameTestNode& InNode, FGameTestFlowContext& InContext) const override
		{
			return MakeShared<GameTestBlockUtil::FOnceState>();
		}

		virtual EGameTestBlockStep Tick(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext, double InNowSeconds, FGameTestBlockTick& Out) const override
		{
			GameTestBlockUtil::FOnceState& State = static_cast<GameTestBlockUtil::FOnceState&>(InState);
			if (State.bStarted || InNode.Lists.Num() == 0)
			{
				return EGameTestBlockStep::Done;
			}
			State.bStarted = true;
			Out.List = 0;
			return EGameTestBlockStep::RunList;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterBlockGroup()
	{
		GameTestBlocks::Get().Register(MakeShared<FBlockGroup>());
	}
}
