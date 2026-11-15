// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/**
 * GameTest：配置驱动的游戏测试驱动（Runtime 模块，Shipping 不加载）。
 *
 * 为什么是 Runtime 而不是 Editor：同一份步骤词汇表要能在两种进程里跑——
 * 编辑器进程内的 PIE（`pie.start` / `pie.stop` 只在这种情况存在）与独立的游戏进程。
 * 编辑器专属能力（PIE 启停、编辑器就绪门禁、关卡加载）用 `#if WITH_EDITOR` 与下面的条件依赖挡住：
 * Game target 编译时它们不存在，但动作词汇表仍然完整（那些动作回报"当前进程没有编辑器"）。
 *
 * 对外只走 UELink（UELinkCore 的扩展接口），不自己开 HTTP 服务。
 * CommonUI / GAS / EnhancedInput 不在这里：它们的动作在可选模块 GameTestCommonUI / GameTestGAS /
 * GameTestEnhancedInput 里，本模块不 include 它们的头。
 */
public class GameTest : ModuleRules
{
	public GameTest(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Public：出现在本模块对外头文件签名里的模块。
		// Json / JsonUtilities：契约层直接持有 TSharedPtr<FJsonObject>，测试模块和可选模块要 include 这些头。
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
		});

		// Private：只在 .cpp 里用。
		// UMG：UWidget / UUserWidget / UListView 这些被操作的对象本体。
		// Slate / SlateCore / ApplicationCore：截图、点击入口、录制用的输入预处理器和合成按键。
		// UELinkCore：gt.* 操作和推送事件挂在它上面。
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UMG",
			"Slate",
			"SlateCore",
			"ApplicationCore",
			"InputCore",
			"DeveloperSettings",
			"Projects",
			"UELinkCore",
		});

		// 编辑器专属步骤的宿主模块。判据：UnrealEd 只在 Editor target 下存在，
		// 所以依赖与代码同时用条件编译挡住（Target.bBuildEditor 与 WITH_EDITOR 对齐）。
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"UnrealEd",
				"LevelEditor",
				"AssetRegistry",
			});
		}

		// 本模块按功能域建子目录（Core/、Observe/、Operate/、Input/、Shot/、Editor/、Record/、Integration/）。
		// UBT 不会自动把模块根加进 include 路径——不加这一行，跨子目录 include 会报 C1083。
		PublicIncludePaths.Add(ModuleDirectory);
	}
}
