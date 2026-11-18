// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/Blocks/GameTestBlock.h"

#include "Core/Script/GameTestBuiltins.h"
#include "Dom/JsonObject.h"

TSharedRef<FGameTestBlockState> IGameTestBlock::Enter(const FGameTestNode& InNode, FGameTestFlowContext& InContext) const
{
	return MakeShared<FGameTestBlockState>();
}

TGameTestRegistry<IGameTestBlock>& GameTestBlocks::Get()
{
	static TGameTestRegistry<IGameTestBlock> Registry;
	GameTestBuiltins::EnsureRegistered();
	return Registry;
}

const IGameTestBlock* GameTestBlocks::FindFor(const FJsonObject& InJson, FName InAction)
{
	if (InAction.IsNone())
	{
		return nullptr;
	}
	for (const TSharedRef<IGameTestBlock>& Block : Get().GetAll())
	{
		if (Block->Claims(InJson, InAction))
		{
			return &Block.Get();
		}
	}
	return nullptr;
}
