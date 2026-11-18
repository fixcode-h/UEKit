// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** 脚本化（1.9）的查询操作：`gt.api`（接口库）、`gt.vars`（某个对象能读写什么）、`gt.validate`（场景写得对不对）。操作表见 docs/game-test-script.md 第 10 节。 */
namespace GameTestRpcScript
{
	bool Api(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	bool Vars(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	bool Validate(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
}
