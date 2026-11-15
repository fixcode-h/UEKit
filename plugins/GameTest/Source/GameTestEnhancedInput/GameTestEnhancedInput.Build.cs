// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/**
 * GameTestEnhancedInput：输入资产的持续注入（input.start / input.stop）。
 *
 * 和主模块分开，是为了主模块不依赖 EnhancedInput：.uplugin 里这个依赖是 Optional，
 * 没有它时只少这两个动作。
 */
public class GameTestEnhancedInput : ModuleRules
{
	public GameTestEnhancedInput(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"EnhancedInput",
			"GameTest",
		});
	}
}
