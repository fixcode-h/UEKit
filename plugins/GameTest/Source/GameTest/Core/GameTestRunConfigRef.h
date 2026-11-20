// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FGameTestScriptOptions;

/**
 * `_settings.json` 里对象引用相关的段（docs/game-test-ref.md 第 6 节）：identRules、closeRules、refDefaults、allowDestroy。
 * 1.x 的 itemKeys / objectKeys 读到就报 settings_old（面板「升级脚本」一起迁移）。
 */
namespace GameTestRunConfigRef
{
	/** 读并校验进 OutOptions；写错返回 false，OutError 以错误码开头（`settings_old：…`）。 */
	GAMETEST_API bool Parse(const FJsonObject& InJson, FGameTestScriptOptions& OutOptions, FString& OutError);

	/** 写回（和默认值一样的不写）。 */
	GAMETEST_API void Write(const FGameTestScriptOptions& InOptions, FJsonObject& OutJson);

	/** 往 identRules 加一条（同类的旧规则换掉），录制时勾「以后都按这个认」用。 */
	GAMETEST_API bool SaveIdentRule(const FString& InClass, const TArray<FString>& InProps, FString& OutError);
}
