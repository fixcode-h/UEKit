// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/**
 * GameTestTests：纯逻辑单测（CQTest）和测试用的探针控件（Probe/）。
 *
 * UncookedOnly + 只允许 Editor target：测试和探针不进 Game 与 Shipping 构件。
 * 单测测的是"选择器解析 / 步骤解析 / 谓词判定"这些不碰引擎对象的部分；
 * 需要真实 UMG 的验证用探针控件当靶子，写成场景跑（场景目录默认 Config/GameTest）。
 * 探针里有 CommonUI 的层级栈，所以这里依赖 CommonUI（只在编辑器里）。
 */
public class GameTestTests : ModuleRules
{
	public GameTestTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"GameTest",
			"Core",
			"CoreUObject",
			"Engine",
			// 控件树模型里带 TWeakObjectPtr<UWidget>，测试引用它就要看得见 UMG 的类型。
			"UMG",
			"Slate",
			"SlateCore",
			"CommonUI",
			"Json",
			"JsonUtilities",
			"CQTest",
		});

		PublicIncludePaths.Add(ModuleDirectory);
	}
}
