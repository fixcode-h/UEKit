// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 给编辑器用的 JSON Schema 文件：`<配置根>/.schema/<kind>.schema.json`，另有片段用的 `fragment.schema.json`。
 *
 * 配置头部的 `$schema` 指向它们（导出时自动写），VS Code / Rider 改配置时就能边写边检查：字段拼错标红、
 * 枚举给补全；面板的配置原文编辑器也用同一份。内容就是 `forge.kinds kind=` 回的那份 schema，
 * 由 `ForgeKindSchema` 生成，所以插件升级改了格式时文件跟着变。
 */
namespace ForgeSchemaFiles
{
	/** 写全部 schema 文件；内容和磁盘上一样的不重写（不动修改时间）。编辑器启动时调。返回这次写了几个文件。 */
	ASSETFORGE_API int32 Install();
}
