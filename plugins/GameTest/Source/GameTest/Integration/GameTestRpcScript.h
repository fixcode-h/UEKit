// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FGameTestScenarioRun;

/** 脚本化（1.9）的查询操作：`gt.api`（接口库）、`gt.vars`（某个对象能读写什么）、`gt.validate`（场景写得对不对）。操作表见 docs/game-test-script.md 第 10 节。 */
namespace GameTestRpcScript
{
	bool Api(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	bool Vars(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	bool Validate(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);

	//~ 调试（GameTestRpcDebug.cpp）
	/** gt.run 的 params / from / to / breakpoints / pauseOnFail → 要跑的场景。 */
	bool ReadRunOptions(const TSharedPtr<FJsonObject>& InArgs, FGameTestScenarioRun& InOutRun, FString& OutError);
	/** gt.debug：state / pause / resume / step（into）/ breakpoints（ids）。 */
	bool Debug(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	/** gt.resolve 带了 conds / exprs。 */
	bool HasScriptResolve(const TSharedPtr<FJsonObject>& InArgs);
	/** gt.resolve 的 conds / exprs（vars 只在这次核对里生效）：只看这一刻，不等待、不稳住。 */
	bool ResolveScript(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	/** gt.step 的步骤是块（if / loop / group / call…）：要当临时 run 在后台跑。 */
	bool IsScriptStep(const FJsonObject& InStep);
}
