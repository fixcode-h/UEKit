// Copyright Epic Games, Inc. All Rights Reserved.

#include "Settings/AssetForgeSettings.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/**
	 * 旧规则的文档根：项目外层同级有 ProjectDoc/docs 就用它，否则用项目里的 docs。
	 * Agent 面板没写提示文件时用；升级前的配置、场景也在这里。
	 */
	FString LegacyDocsRoot()
	{
		const FString Sibling = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../ProjectDoc/docs"));
		if (IFileManager::Get().DirectoryExists(*Sibling))
		{
			return Sibling;
		}
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("docs"));
	}

	/**
	 * Agent 面板算好的文档根：面板 UE 插件写在 Saved/AgentPanel/docs-root.txt（一行绝对路径，见 docs/protocol.md）。
	 * P4 / SVN 项目各分支共用一份文档根，可能在别的分支目录下、面板项目设置里还能改，引擎这边按目录猜不出来（用户 2026-11-20 定统一到它）。
	 * 文件没有、不是绝对路径、目录不在都返回空，调用方退回旧规则。
	 */
	FString PanelDocsRoot()
	{
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AgentPanel") / TEXT("docs-root.txt"));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File))
		{
			return FString();
		}
		Text.TrimStartAndEndInline();
		if (Text.IsEmpty() || FPaths::IsRelative(Text))
		{
			return FString();
		}
		FPaths::NormalizeDirectoryName(Text);
		return IFileManager::Get().DirectoryExists(*Text) ? Text : FString();
	}

	/**
	 * 文档根下的子目录（AssetForge、GameTest）：优先面板给的文档根；那里还没有这个子目录、旧规则的位置有时
	 * （面板还没来得及把旧内容搬过去、搬失败了）先用旧的，不让已有的配置和场景突然看不见。
	 */
	FString ResolveDocsSubDir(const TCHAR* Name)
	{
		const FString Legacy = LegacyDocsRoot() / Name;
		const FString Panel = PanelDocsRoot();
		if (Panel.IsEmpty())
		{
			return Legacy;
		}
		const FString Dir = Panel / Name;
		if (!IFileManager::Get().DirectoryExists(*Dir) && IFileManager::Get().DirectoryExists(*Legacy))
		{
			return Legacy;
		}
		return Dir;
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

FString UAssetForgeSettings::ResolveDocsSubDirPath(const TCHAR* Name)
{
	return ResolveDocsSubDir(Name);
}

FString UAssetForgeSettings::ResolveConfigRootDir() const
{
	FString Root = ConfigRoot.Path.TrimStartAndEnd();
	if (Root.IsEmpty())
	{
		// 留空自动：和 GameTest 一样跟着项目的文档根走（ProjectDoc/docs 或项目里的 docs）
		return ResolveDocsSubDir(TEXT("AssetForge"));
	}

	if (FPaths::IsRelative(Root))
	{
		Root = FPaths::Combine(FPaths::ProjectDir(), Root);
	}

	Root = FPaths::ConvertRelativePathToFull(Root);
	FPaths::NormalizeDirectoryName(Root);
	return Root;
}
