// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 算法层：配置内容哈希。
 *
 * 增量导入**以哈希判据，不以 revision 判据**——revision 是人/AI 手写的，会忘；
 * 哈希是算出来的，不会。两者都留着是为了互补：哈希决定"要不要重跑"，
 * revision 决定"作者以为改过没有"，不一致时给一条警告（见 ForgeDoc 的校验）。
 */
namespace ForgeHash
{
	/** 文本内容的 32 位 CRC，返回 8 位十六进制字符串。 */
	ASSETFORGE_API FString HashText(const FString& InText);
}
