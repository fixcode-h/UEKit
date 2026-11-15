// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "ForgeTestRow.generated.h"

/**
 * 管道自检用的行结构体。
 *
 * DataTable 的 `rowStruct` 必须是 `FTableRowBase` 的派生，而这类结构体属于**项目自己的游戏代码**
 * （真正的表要在主模块里定义它的行结构体）。这里这一份只服务于端到端验证，随测试模块一起
 * 不进游戏与打包构件。
 */
USTRUCT(BlueprintType)
struct FForgeTestRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AssetForge|Test")
	float Damage = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AssetForge|Test")
	FString DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AssetForge|Test")
	bool bEnabled = false;
};
