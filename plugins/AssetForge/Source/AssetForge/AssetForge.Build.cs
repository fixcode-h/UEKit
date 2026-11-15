// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/**
 * AssetForge：配置驱动的资产管道。界面在 Agent 面板网页上，操作经 UELink 的 /rpc 调用（forge.*）。
 *
 * 这是一个 Editor 模块——它依赖 UMGEditor（`UWidgetBlueprint` 住在 Editor/UMGEditor），
 * 而 UMGEditor 只在 Editor target 下存在。插件 uplugin 里因此声明 Type = Editor，
 * 不靠每个文件里的 #if WITH_EDITOR 兜底。
 */
public class AssetForge : ModuleRules
{
	public AssetForge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Public：出现在本模块对外头文件签名里的模块。
		// Json / JsonUtilities 是因为契约层直接持有 TSharedPtr<FJsonObject>——
		// 测试模块要 include 这些头文件，所以必须放 Public 侧（判据见 ue-architecture-standards §3）。
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
			// Settings/AssetForgeSettings.h 是公开头（测试引用它），它继承 UDeveloperSettings。
			"DeveloperSettings",
		});

		// UELinkCore：操作挂到 UELink 的 /rpc 上（FAgentPanelBridge），不自己开 HTTP 服务。
		PrivateDependencyModuleNames.Add("UELinkCore");

		// Private：只在 .cpp 内部使用。
		// UnrealEd 提供 FKismetEditorUtilities（蓝图创建与编译）、AssetTools 提供 IAssetTools，
		// UMG 提供 UWidgetTree/UPanelWidget，UMGEditor 提供 UWidgetBlueprint。
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"UMG",
			"UMGEditor",
			"AssetTools",
			// AssetRegistry：列出未纳管资产、按注册表判类型（不加载资产）。
			"AssetRegistry",
			// AIModule：行为树的 UBehaviorTree / UBTCompositeNode 都在这里。
			"AIModule",
			// AnimGraph：动画蓝图的状态机与图节点（UAnimationStateMachineGraph / UAnimStateNode / UAnimGraphNode_*）都在这里。
			"AnimGraph",
			// AnimGraphRuntime：动画图节点持有的运行时节点结构（FAnimNode_StateMachine 等）。
			"AnimGraphRuntime",
			// MaterialEditor：材质图的编辑接口——UMaterialEditingLibrary（建表达式、连线、排版、重编译）
			// 与 FMaterialEditorUtilities（图重建之后刷新打开的材质编辑器）。
			"MaterialEditor",
			// Slate / SlateCore：内容浏览器右键菜单（FMenuBuilder）和完成通知（SNotificationList）。
			"Slate",
			"SlateCore",
			// EditorSubsystem：资产健康检查走 UEditorValidatorSubsystem。
			"EditorSubsystem",
			// ContentBrowser：内容浏览器资产的右键菜单扩展（FAssetForgeAssetMenu 的注册点）。
			"ContentBrowser",
			// Niagara / NiagaraEditor：niagara kind——模板 Emitter 复制与系统层用户参数覆盖
			// （FNiagaraEditorUtilities::AddEmitterToSystem、UNiagaraSystemFactoryNew 与 UserParameters 系列）。
			"Niagara",
			"NiagaraEditor",
			// DataValidation：资产健康检查的通用路径——对所有资产类型跑编辑器验证器
			// （错误与警告都要，见 Core/ForgeAssetHealth.cpp）。
			"DataValidation",
			// ImageWrapper：texture kind——解析源图（PNG/TGA/EXR…）成 FImage 再灌进纹理 Source
			// （FImageUtils::DecompressImage 的实现模块）。
			"ImageWrapper",
			// RHI：读材质编译错误要拿当前着色平台的资源（`GMaxRHIShaderPlatform`）。
			"RHI",
			// MovieScene / MovieSceneTracks：UMG 动画的生成——UMovieScene、属性轨道
			// 与 float 通道（UMovieSceneFloatTrack/Section）都在这里
			//（margin 轨道与 UWidgetAnimation 本身在 UMG 模块，已在上方）。
			"MovieScene",
			"MovieSceneTracks",
			// StateTreeModule / StateTreeEditorModule：state-tree kind——UStateTree 与
			// UStateTreeEditorData 在 Runtime 侧；编译入口（UStateTreeEditingSubsystem::CompileStateTree）
			// 与可绑定描述符枚举在 Editor 侧（UncookedOnly，Editor 模块可依赖）。
			// PropertyBindingUtils（+Editor）：绑定路径 / 绑定集合的类型与实现；GameplayTags：状态标签与事件。
			"StateTreeModule",
			"StateTreeEditorModule",
			"PropertyBindingUtils",
			"PropertyBindingUtilsEditor",
			"GameplayTags",
		});

		// 本模块按层建子目录（Core/ Registry/ State/ Service/ Settings/ Integration/ Kinds/<Kind>/）。
		// UBT 不会自动把模块根加进 include 路径——
		// 不加这一行，跨子目录的 #include "Core/ForgeDoc.h" 会报 C1083。
		PublicIncludePaths.Add(ModuleDirectory);
	}
}
