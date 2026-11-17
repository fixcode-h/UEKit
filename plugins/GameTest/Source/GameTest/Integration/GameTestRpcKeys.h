// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * 条目标识相关的 `/rpc` 操作（1.4.0）：`gt.record.key`（录制中给一步定怎么认）、
 * `gt.keys`（条目的全部字段和唯一性）、`gt.keys.preview`（按条件在游戏里框出命中的）。
 * 由 GameTestRpc 注册；签名和别的 gt.* 操作一样。
 */
namespace GameTestRpcKeys
{
	bool RecordKey(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error);
	bool Keys(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error);
	bool Preview(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error);
}
