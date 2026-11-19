// Copyright Epic Games, Inc. All Rights Reserved.

#include "Service/ForgeSchemaFiles.h"
#include "AssetForge.h"

#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Registry/ForgeKindRegistry.h"
#include "Registry/ForgeKindSchema.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace ForgeSchemaFiles
{

namespace
{
	/** 和磁盘上逐字相同就不写：每次启动都重写会让版本管理里一直显示改动、P4 要签出。 */
	bool WriteSchema(const FString& InPath, const TSharedPtr<FJsonObject>& InSchema)
	{
		if (!InSchema.IsValid())
		{
			return false;
		}
		FString Existing;
		if (FFileHelper::LoadFileToString(Existing, *InPath) && Existing == ForgeJsonText::ToPrettyString(InSchema))
		{
			return false;
		}
		FString Error;
		if (!ForgeJsonText::SaveObject(InPath, InSchema, Error))
		{
			UE_LOG(LogAssetForge, Warning, TEXT("写 schema 文件失败：%s"), *Error);
			return false;
		}
		return true;
	}
}

int32 Install()
{
	const FString Dir = ForgePipeline::GetSchemaDir();
	int32 Written = 0;
	for (const ForgeKindRegistry::FForgeKindInfo& Info : ForgeKindRegistry::GetAll())
	{
		const FString Path = FPaths::Combine(Dir, FString(Info.Kind) + TEXT(".schema.json"));
		Written += WriteSchema(Path, ForgeKindSchema::Build(Info.Kind)) ? 1 : 0;
	}
	Written += WriteSchema(FPaths::Combine(Dir, TEXT("fragment.schema.json")), ForgeKindSchema::BuildFragment()) ? 1 : 0;
	if (Written > 0)
	{
		UE_LOG(LogAssetForge, Log, TEXT("写了 %d 个 schema 文件到 %s（编辑器改配置时边写边检查用）"), Written, *Dir);
	}
	return Written;
}

} // namespace ForgeSchemaFiles
