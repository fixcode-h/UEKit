// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ForgeObjectProbe.generated.h"

/**
 * `object-blueprint` 用例的**测试锚点类**：非 Actor、可蓝图化、非抽象，且带一个默认值明确的属性。
 *
 * 有了它，「写一个非默认值 → 计划里必须出现动作」就是可控的——不依赖任何插件（GameplayEffect 的
 * 那些属性默认值各是什么，不该是测试要记住的事）。
 */
UCLASS(Blueprintable)
class UForgeObjectProbe : public UObject
{
	GENERATED_BODY()

public:
	/** 测试用属性：默认 0，写别的值即产生计划动作。 */
	UPROPERTY(EditAnywhere, Category = "AssetForge|Test")
	int32 ProbeValue = 0;
};
