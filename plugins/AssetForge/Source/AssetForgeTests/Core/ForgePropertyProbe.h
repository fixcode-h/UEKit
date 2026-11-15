// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "ForgePropertyProbe.generated.h"

/**
 * 属性编解码的测试探针。
 *
 * `ProtectedValue` 刻意凑齐旧排除规则的三个条件（`CPF_Edit` + `BlueprintReadOnly` +
 * `DisableEditOnInstance` + `BlueprintProtected` 标签）——旧规则会把它挡在配置之外，
 * 而按 spec「细节面板可见即可配」它必须可配。改动 `IsCodecProperty` 时先跑 `AssetForge.PropertyCodec`。
 */
UCLASS()
class UForgePropertyProbe : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta = (BlueprintProtected))
	int32 ProtectedValue = 0;
};
