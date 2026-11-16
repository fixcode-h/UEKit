// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * 按反射列出一个类（或结构体）在配置 `props` 里能写的属性：名字、JSON 类型、UE 类型、枚举取值。
 *
 * 和 `ForgePropertyCodec` 用同一个判据（`IsCodecProperty`）：这里列出来的就是导入真认的，
 * 没列出的写了会被演练拒掉。值的写法和 `FJsonObjectConverter` 一致：枚举写名字字符串，
 * 对象引用写资产路径，结构体写对象（只写想改的字段）。
 */
namespace ForgePropertySchema
{
	struct FQuery
	{
		/** 类或结构体：全路径（/Script/Engine.WorldSettings、/Game/X/BP_Y.BP_Y_C）或短名（TextBlock）。 */
		FString Type;
		/** 按属性名筛（不分大小写的包含匹配），空 = 全部。 */
		FString Filter;
		int32 Offset = 0;
		int32 Limit = 60;
	};

	/**
	 * 写进 OutJson：`class`（解析到的全路径）、`isStruct`、`properties`（名字 → JSON Schema 片段，
	 * 带 `x-ue` 写 UE 类型）、`total`、`offset`、`truncated`。
	 * 解析不了时返回 false，OutError 写中文原因。
	 */
	ASSETFORGE_API bool Describe(const FQuery& InQuery, const TSharedPtr<FJsonObject>& OutJson, FString& OutError);
}
