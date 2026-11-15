// Copyright Epic Games, Inc. All Rights Reserved.

#include "Settings/AssetForgeSettings.h"

#include "Misc/Paths.h"

UAssetForgeSettings::UAssetForgeSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Asset Forge");
	ConfigRoot.Path = TEXT("Docs/AssetForge");
	UnmanagedScanRoots.Add(TEXT("/Game"));
}

FName UAssetForgeSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

FString UAssetForgeSettings::ResolveConfigRootDir() const
{
	FString Root = ConfigRoot.Path.TrimStartAndEnd();
	if (Root.IsEmpty())
	{
		Root = TEXT("Docs/AssetForge");
	}

	if (FPaths::IsRelative(Root))
	{
		Root = FPaths::Combine(FPaths::ProjectDir(), Root);
	}

	Root = FPaths::ConvertRelativePathToFull(Root);
	FPaths::NormalizeDirectoryName(Root);
	return Root;
}
