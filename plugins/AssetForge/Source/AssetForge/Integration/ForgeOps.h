// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * AssetForge 挂到 UELink `/rpc` 上的操作（`forge.*`），Agent 面板的 AssetForge 页和 `ue_forge` 工具都调它们。
 * 操作列表和参数见仓库的 `docs/asset-forge.md`。
 */
namespace ForgeOps
{
	void Register();
	void Unregister();
}
