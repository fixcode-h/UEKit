// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/GameTestNode.h"

#include "Core/Flow/GameTestGuards.h"

namespace
{
	const FGameTestNode* FindIn(const FGameTestNodeList& InList, const FString& InKey)
	{
		for (const TSharedRef<FGameTestNode>& Node : InList.Nodes)
		{
			if (Node->Id == InKey || Node->Path == InKey)
			{
				return &Node.Get();
			}
			for (const FGameTestNodeList& Child : Node->Lists)
			{
				if (const FGameTestNode* Found = FindIn(Child, InKey))
				{
					return Found;
				}
			}
			// 守护的 do 也能下断点（路径 `guards.0.do.1`）。
			for (const TSharedRef<FGameTestGuardDef>& Guard : Node->Guards)
			{
				if (const FGameTestNode* Found = FindIn(Guard->Do, InKey))
				{
					return Found;
				}
			}
		}
		return nullptr;
	}

	int32 CountIn(const FGameTestNodeList& InList)
	{
		int32 Count = 0;
		for (const TSharedRef<FGameTestNode>& Node : InList.Nodes)
		{
			Count += Node->CountLeaves();
		}
		return Count;
	}
}

const FGameTestNodeList* FGameTestNode::FindList(const FString& InKey) const
{
	for (const FGameTestNodeList& List : Lists)
	{
		if (List.Key == InKey)
		{
			return &List;
		}
	}
	return nullptr;
}

int32 FGameTestNode::CountLeaves() const
{
	if (bDisabled)
	{
		return 0;
	}
	if (!IsBlock())
	{
		return 1;
	}
	// 分支（then / else）只会走一条：取最多的那条，比全部加起来更接近实际步数。
	int32 Max = 0;
	for (const FGameTestNodeList& List : Lists)
	{
		Max = FMath::Max(Max, CountIn(List));
	}
	return FMath::Max(Max, 1);
}

const FGameTestNode* FGameTestScript::FindNode(const FString& InKey) const
{
	if (InKey.IsEmpty())
	{
		return nullptr;
	}
	if (const FGameTestNode* Found = FindIn(Setup, InKey))
	{
		return Found;
	}
	if (const FGameTestNode* Found = FindIn(Steps, InKey))
	{
		return Found;
	}
	for (const TSharedRef<FGameTestGuardDef>& Guard : Guards)
	{
		if (const FGameTestNode* Found = FindIn(Guard->Do, InKey))
		{
			return Found;
		}
	}
	return nullptr;
}

int32 FGameTestScript::CountLeaves() const
{
	return CountIn(Setup) + CountIn(Steps);
}
