// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/**
 * AssetForge 的测试模块。
 *
 * 也放示例测试界面的运行时代码（`Sample/`：跳页父类、示例地图的 GameMode）：只在编辑器和 PIE 里用，
 * 和 GameTest 的测试探针控件一样不进打包。
 *
 * 它只在 Editor target 下存在——由 `AssetForge.uplugin` 里的 `UncookedOnly` +
 * `TargetAllowList: ["Editor"]` 决定，不靠每个文件里的 #if。依赖方向单向：Tests → AssetForge。
 */
public class AssetForgeTests : ModuleRules
{
	public AssetForgeTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// 被测模块：测试要看见 Core/ Registry/ State/ Kinds/ 等目录下的公开头文件。
		PublicDependencyModuleNames.AddRange(new string[] { "AssetForge" });

		// CQTest 由引擎自带（Engine/Source/Developer/CQTest），提供 TEST_CLASS / BEFORE_EACH / ASSERT_THAT。
		// Json / JsonUtilities 需要显式列出：用例要直接构造与读取 FJsonObject 来喂给被测函数。
		// UMG：类解析的用例要引用 UWidget / UUserWidget / UTextBlock 做断言。
		// StateTree 族：state-tree 的用例要构造内存态资产（UStateTree / EditorData / State）
		// 与可绑定结构描述符。刻意不依赖 GameplayStateTree——那是默认禁用的运行时组件插件，
		// 测试不该为一个 schema 把它绑进项目的插件集合（用 StateTreeModule 的 schema 类路径即可）。
		PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "UMG", "Json", "JsonUtilities", "CQTest" });
		// Slate / SlateCore：示例测试界面（Sample/）的父类要绑下拉框、输入框的委托，参数类型 ESelectInfo / ETextCommit 在 SlateCore
		PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		// 状态树只在 UE 5.8 上编（和 AssetForge.Build.cs 同一个判断，用例按 ASSETFORGE_WITH_STATETREE 跳过）
		if (Target.Version.MajorVersion > 5 || (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 8))
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "StateTreeModule", "StateTreeEditorModule", "PropertyBindingUtils" });
		}

		// 与其它模块同理：UBT 不会自动把模块根加进 include 路径。
		PublicIncludePaths.Add(ModuleDirectory);
	}
}
