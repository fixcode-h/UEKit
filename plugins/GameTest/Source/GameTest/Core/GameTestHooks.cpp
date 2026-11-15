// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestHooks.h"

FSimpleMulticastDelegate& GameTestHooks::OnRunFinished()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

GameTestHooks::FOnHealth& GameTestHooks::OnHealth()
{
	static FOnHealth Delegate;
	return Delegate;
}

GameTestHooks::FOnJobChanged& GameTestHooks::OnJobChanged()
{
	static FOnJobChanged Delegate;
	return Delegate;
}

GameTestHooks::FOnJobChanged& GameTestHooks::OnRecordChanged()
{
	static FOnJobChanged Delegate;
	return Delegate;
}
