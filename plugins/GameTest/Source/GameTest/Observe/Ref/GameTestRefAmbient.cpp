// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestRefAmbient.h"

namespace GameTestRefAmbientImpl
{
	/** 只在游戏线程上读写（执行器、RPC 都在游戏线程）。 */
	FGameTestFlowContext* Current = nullptr;
}

FGameTestFlowContext* GameTestRefAmbient::Get()
{
	return GameTestRefAmbientImpl::Current;
}

GameTestRefAmbient::FScope::FScope(FGameTestFlowContext* InContext)
	: Prev(GameTestRefAmbientImpl::Current)
{
	GameTestRefAmbientImpl::Current = InContext;
}

GameTestRefAmbient::FScope::~FScope()
{
	GameTestRefAmbientImpl::Current = Prev;
}
