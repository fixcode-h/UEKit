// Copyright Epic Games, Inc. All Rights Reserved.

#include "Settings/AssetForgeSettings.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
	/**
	 * 项目的文档根（和 Agent 面板的约定一致）：P4 / SVN 项目的文档常放在项目外层同级的 ProjectDoc/docs 里，
	 * 有这个目录就用它；否则用项目里的 docs。不按版本管理类型判断：引擎里拿不准，看目录在不在最稳。
	 */
	FString ResolveDocsRoot()
	{
		const FString Sibling = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../ProjectDoc/docs"));
		if (IFileManager::Get().DirectoryExists(*Sibling))
		{
			return Sibling;
		}
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("docs"));
	}
}

UAssetForgeSettings::UAssetForgeSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Asset Forge");
	UnmanagedScanRoots.Add(TEXT("/Game"));
}

FName UAssetForgeSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

FString UAssetForgeSettings::ResolveDocsRootDir()
{
	return ResolveDocsRoot();
}

FString UAssetForgeSettings::ResolveConfigRootDir() const
{
	FString Root = ConfigRoot.Path.TrimStartAndEnd();
	if (Root.IsEmpty())
	{
		// 留空自动：和 GameTest 一样跟着项目的文档根走（ProjectDoc/docs 或项目里的 docs）
		return ResolveDocsRoot() / TEXT("AssetForge");
	}

	if (FPaths::IsRelative(Root))
	{
		Root = FPaths::Combine(FPaths::ProjectDir(), Root);
	}

	Root = FPaths::ConvertRelativePathToFull(Root);
	FPaths::NormalizeDirectoryName(Root);
	return Root;
}
