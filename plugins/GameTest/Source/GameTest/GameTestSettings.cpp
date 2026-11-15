// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameTestSettings.h"

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

UGameTestSettings::UGameTestSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Game Test");
}

FName UGameTestSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

FString UGameTestSettings::ResolveScenarioRootDir()
{
	const UGameTestSettings* Settings = GetDefault<UGameTestSettings>();
	const FString Configured = Settings != nullptr ? Settings->ScenarioRootDir.TrimStartAndEnd() : FString();
	if (Configured.IsEmpty())
	{
		return ResolveDocsRoot() / TEXT("GameTest");
	}
	FString Root = FPaths::IsRelative(Configured) ? FPaths::ProjectDir() / Configured : Configured;
	Root = FPaths::ConvertRelativePathToFull(Root);
	FPaths::NormalizeDirectoryName(Root);
	return Root;
}
