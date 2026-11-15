// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/**
 * GameTestCommonUI：CommonUI 层级容器的动作和快照状态。
 *
 * 和主模块分开，是为了主模块不依赖 CommonUI：项目没开 CommonUI 时，.uplugin 里这个依赖是 Optional，
 * 只少了 stack.push / stack.pop 两个动作，写了它们的场景会被告知要开哪个插件。
 */
public class GameTestCommonUI : ModuleRules
{
	public GameTestCommonUI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"UMG",
			"CommonUI",
			"GameTest",
		});
	}
}
