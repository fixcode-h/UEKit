// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/**
 * GameTestEditor：只在编辑器里才做得到的 GameTest 能力（1.9.0 起）。
 *
 * - C++ 项目接口扫描：读类的源码元数据（ModuleRelativePath、ToolTip、Category），写场景根 `_api.json` 的 cpp 段。
 *   打包的游戏没有这些元数据，所以扫描放在编辑器，运行时只读 `_api.json`、按类 + 函数名反射调。
 * - `gt.widgets`：读 WBP 资产的设计器控件树（UWidgetBlueprint::WidgetTree），不用开游戏。
 * - 录 UE 编辑器操作（`gt.editrec.*`）和回放用的 `editor.*` 动作（1.11.0，定稿 10.1）。
 *
 * 模块类型是 Editor：游戏进程（含 Shipping）不加载。操作照样挂在 UELink 的 `/rpc` 上（`gt.` 前缀）。
 */
public class GameTestEditor : ModuleRules
{
	public GameTestEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"UMG",
			"UMGEditor",
			"UnrealEd",
			"AssetRegistry",
			// editor.* 动作和编辑器录制（1.11.0）：Slate 控件查找、输入预处理、编辑器命令、关卡编辑器命令表、蓝图编辑器
			"Slate",
			"SlateCore",
			"InputCore",
			"LevelEditor",
			"Kismet",
			"UELinkCore",
			"GameTest",
		});

		// 按功能建子目录（Api/、Widgets/），跨子目录 include 要模块根在 include 路径里
		PrivateIncludePaths.Add(ModuleDirectory);
	}
}
