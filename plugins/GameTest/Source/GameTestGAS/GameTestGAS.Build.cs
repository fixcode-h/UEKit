// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/**
 * GameTestGAS：技能输入标签（ability.input）。
 *
 * 和主模块分开，是为了主模块不依赖 GameplayAbilities：.uplugin 里这个依赖是 Optional，
 * 没有它时只少这一个动作。项目自己的输入标签入口（AbilityInputTagPressed / Released）按名字反射调用，
 * 所以也不依赖游戏模块。
 */
public class GameTestGAS : ModuleRules
{
	public GameTestGAS(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"GameplayAbilities",
			"GameplayTags",
			"GameTest",
		});
	}
}
