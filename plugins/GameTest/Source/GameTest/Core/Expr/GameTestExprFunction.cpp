// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Expr/GameTestExprFunction.h"

#include "Core/Script/GameTestBuiltins.h"

TGameTestRegistry<FGameTestExprFunction>& GameTestExprFunctions::Get()
{
	static TGameTestRegistry<FGameTestExprFunction> Registry;
	GameTestBuiltins::EnsureRegistered();
	return Registry;
}

void GameTestExprFunctions::Add(const TCHAR* InName, int32 InMinArgs, int32 InMaxArgs, const TCHAR* InCategory, const TCHAR* InSignature, const TCHAR* InSummary, FGameTestExprFunction::FCallFn&& InFn, bool bAcceptsUnresolved)
{
	TSharedRef<FGameTestExprFunction> Function = MakeShared<FGameTestExprFunction>();
	Function->Name = InName;
	Function->MinArgs = InMinArgs;
	Function->MaxArgs = InMaxArgs;
	Function->Category = InCategory;
	Function->Signature = InSignature;
	Function->Summary = InSummary;
	Function->bAcceptsUnresolved = bAcceptsUnresolved;
	Function->Call = MoveTemp(InFn);
	Get().Register(Function);
}
