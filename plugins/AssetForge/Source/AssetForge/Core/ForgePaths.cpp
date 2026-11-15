// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ForgePaths.h"

#include "Misc/Paths.h"

namespace ForgePaths
{

FString CollapseToAbsolute(const FString& InPath)
{
	FString Full = FPaths::IsRelative(InPath)
		? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), InPath))
		: FPaths::ConvertRelativePathToFull(InPath);
	FPaths::CollapseRelativeDirectories(Full);
	Full.ReplaceInline(TEXT("\\"), TEXT("/"));

	while (true)
	{
		const int32 DotIndex = Full.Find(TEXT("/../"));
		if (DotIndex == INDEX_NONE)
		{
			break;
		}

		int32 SegmentStart = INDEX_NONE;
		for (int32 Index = DotIndex - 1; Index >= 0; --Index)
		{
			if (Full[Index] == TEXT('/'))
			{
				SegmentStart = Index;
				break;
			}
		}

		if (SegmentStart == INDEX_NONE)
		{
			// 前面没有可退的段（`E:/../` 这种）：直接吃掉这个 `..`。
			Full = Full.Left(DotIndex) + Full.RightChop(DotIndex + 3);
		}
		else
		{
			// 退掉前一段：`/a/../b` → `/b`。
			Full = Full.Left(SegmentStart) + Full.Mid(DotIndex + 3);
		}
	}

	return Full;
}

FString MakeProjectRelative(const FString& InAbsolutePath)
{
	const FString FullPath = FPaths::ConvertRelativePathToFull(InAbsolutePath).Replace(TEXT("\\"), TEXT("/"));

	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()).Replace(TEXT("\\"), TEXT("/"));
	if (!ProjectDir.EndsWith(TEXT("/")))
	{
		ProjectDir += TEXT("/");
	}

	if (FullPath.StartsWith(ProjectDir, ESearchCase::IgnoreCase))
	{
		return FullPath.RightChop(ProjectDir.Len());
	}

	return FullPath;
}

} // namespace ForgePaths
