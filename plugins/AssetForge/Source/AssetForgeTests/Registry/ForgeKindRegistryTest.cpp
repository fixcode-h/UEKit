// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Registry/ForgeKindRegistry.h"

#include "Animation/AnimBlueprint.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "Engine/FontFace.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceConstant.h"
#include "StateTree.h"

#if WITH_AUTOMATION_WORKER

/**
 * kind 登记表的守门用例。
 *
 * 登记表是「加一种资产类型只改一处」的那一处：kind / 目录必须唯一，资产类 → kind 的判定
 * 按继承深度取最具体者（不再依赖手写 IsA 链的顺序），属性差异配方必须与真实导入一致。
 */
TEST_CLASS_WITH_FLAGS(ForgeKindRegistryTest, "AssetForge.Registry",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	TEST_METHOD(kind与配置目录唯一)
	{
		TSet<FString> Kinds;
		TSet<FString> Dirs;
		for (const ForgeKindRegistry::FForgeKindInfo& Info : ForgeKindRegistry::GetAll())
		{
			ASSERT_THAT(IsFalse(Kinds.Contains(Info.Kind), FString::Printf(TEXT("kind 重复：%s"), Info.Kind)));
			ASSERT_THAT(IsFalse(Dirs.Contains(Info.ConfigDirName), FString::Printf(TEXT("配置目录重复：%s"), Info.ConfigDirName)));
			ASSERT_THAT(IsTrue(Info.Import != nullptr && Info.Export != nullptr, FString::Printf(TEXT("%s 缺导入/导出函数"), Info.Kind)));
			ASSERT_THAT(IsTrue(Info.GetAssetClass != nullptr && Info.GetAssetClass() != nullptr, FString::Printf(TEXT("%s 缺资产类"), Info.Kind)));
			Kinds.Add(Info.Kind);
			Dirs.Add(Info.ConfigDirName);
		}
	}

	TEST_METHOD(资产类判定取最具体的kind)
	{
		// UStateTree 是 UDataAsset 的子类：旧实现靠 IsA 链顺序保证，现在靠继承深度。
		ASSERT_THAT(AreEqual(FString(TEXT("state-tree")), ForgeKindRegistry::ClassifyAssetClass(UStateTree::StaticClass()),
			TEXT("状态树不能被判成 data-asset")));
		ASSERT_THAT(AreEqual(FString(TEXT("data-asset")), ForgeKindRegistry::ClassifyAssetClass(UPrimaryDataAsset::StaticClass()),
			TEXT("UDataAsset 的普通子类是 data-asset")));
		// UAnimBlueprint 也是 UBlueprint。
		ASSERT_THAT(AreEqual(FString(TEXT("anim-blueprint")),
			ForgeKindRegistry::ClassifyAssetClass(UAnimBlueprint::StaticClass(), AActor::StaticClass()),
			TEXT("动画蓝图不能被判成 actor-blueprint")));
	}

	TEST_METHOD(蓝图按原生父类分流)
	{
		ASSERT_THAT(AreEqual(FString(TEXT("actor-blueprint")),
			ForgeKindRegistry::ClassifyAssetClass(UBlueprint::StaticClass(), AActor::StaticClass()),
			TEXT("父类是 Actor 的蓝图走 actor-blueprint")));
		ASSERT_THAT(AreEqual(FString(TEXT("object-blueprint")),
			ForgeKindRegistry::ClassifyAssetClass(UBlueprint::StaticClass(), UObject::StaticClass()),
			TEXT("父类不是 Actor 的蓝图走 object-blueprint")));
		ASSERT_THAT(AreEqual(FString(TEXT("object-blueprint")),
			ForgeKindRegistry::ClassifyAssetClass(UBlueprint::StaticClass(), nullptr),
			TEXT("父类未知时按对象蓝图处理（与旧导出分派一致）")));
	}

	TEST_METHOD(不支持的资产类返回空)
	{
		ASSERT_THAT(IsTrue(ForgeKindRegistry::ClassifyAssetClass(UFontFace::StaticClass()).IsEmpty(),
			TEXT("字体面是 UFont 的附属资产，不单独纳管")));
		ASSERT_THAT(IsTrue(ForgeKindRegistry::ClassifyAssetClass(UMaterialInstanceConstant::StaticClass()).IsEmpty(),
			TEXT("材质实例不是 UMaterial")));
		ASSERT_THAT(IsTrue(ForgeKindRegistry::ClassifyAssetClass(nullptr).IsEmpty(), TEXT("空类返回空")));
	}

	TEST_METHOD(未登记的kind原样回显)
	{
		ASSERT_THAT(AreEqual(FString(TEXT("some-future-kind")), ForgeKindRegistry::GetDisplayName(TEXT("some-future-kind")), TEXT("显示名原样回显")));
		ASSERT_THAT(AreEqual(FString(TEXT("some-future-kind")), ForgeKindRegistry::GetConfigDirName(TEXT("some-future-kind")), TEXT("目录名原样回显")));
		ASSERT_THAT(IsFalse(ForgeKindRegistry::IsSupportedKind(TEXT("some-future-kind")), TEXT("未登记的 kind 不算支持")));
	}

	/** 属性差异视图：支持的 8 种 kind 必须给出「能」与正确的对象来源。 */
	TEST_METHOD(单对象kind支持属性差异)
	{
		using ForgeKindRegistry::EForgeDiffObjectSource;

		const TArray<FString> AssetItselfKinds = {
			TEXT("data-asset"), TEXT("texture"), TEXT("skeletal-mesh"),
			TEXT("skeleton"), TEXT("physics-asset"), TEXT("anim-sequence"),
		};

		for (const FString& Kind : AssetItselfKinds)
		{
			const ForgeKindRegistry::FForgeDiffSupport Support =
				ForgeKindRegistry::GetPropertyDiffSupport(Kind);
			ASSERT_THAT(IsTrue(Support.bSupported, FString::Printf(TEXT("%s 应支持属性差异"), *Kind)));
			ASSERT_THAT(IsTrue(
				Support.Source == EForgeDiffObjectSource::AssetItself,
				FString::Printf(TEXT("%s 的对象来源应是资产自身"), *Kind)));
		}

		// 蓝图比的是生成类 CDO，不是 UBlueprint 自己。
		const ForgeKindRegistry::FForgeDiffSupport ObjectBp =
			ForgeKindRegistry::GetPropertyDiffSupport(TEXT("object-blueprint"));
		ASSERT_THAT(IsTrue(ObjectBp.bSupported, TEXT("object-blueprint 应支持")));
		ASSERT_THAT(IsTrue(
			ObjectBp.Source == EForgeDiffObjectSource::GeneratedClassDefaultObject,
			TEXT("object-blueprint 要比生成类 CDO")));

		// 地图比的是 AWorldSettings，不是 UWorld。
		const ForgeKindRegistry::FForgeDiffSupport Map =
			ForgeKindRegistry::GetPropertyDiffSupport(TEXT("map"));
		ASSERT_THAT(IsTrue(Map.bSupported, TEXT("map 应支持")));
		ASSERT_THAT(IsTrue(
			Map.Source == EForgeDiffObjectSource::WorldSettings,
			TEXT("map 要比 WorldSettings")));
	}

	/**
	 * 配方标志必须与真实导入代码一致——这是本视图唯一可能"看起来对、其实不对"的地方。
	 *
	 * 两边分组的依据是各自 kind 实现里有没有 `ResetToDefaults`：
	 * `data-asset` / `texture` / `map` 有（配置没写的属性回默认）；
	 * `skeletal-mesh` / `skeleton` / `physics-asset` / `anim-sequence` 没有（只写配置声明的）。
	 * 改了任一侧的导入代码，这条用例应当失败——它守的就是"面板显示的是导入后的样子"。
	 */
	TEST_METHOD(应用配方与真实导入一致)
	{
		const TArray<FString> ResetKinds = { TEXT("data-asset"), TEXT("texture"), TEXT("map") };
		for (const FString& Kind : ResetKinds)
		{
			const ForgeKindRegistry::FForgeDiffSupport Support =
				ForgeKindRegistry::GetPropertyDiffSupport(Kind);
			ASSERT_THAT(IsTrue(
				Support.bResetBeforeApply,
				FString::Printf(TEXT("%s 的导入会 ResetToDefaults，配方必须一致"), *Kind)));
			ASSERT_THAT(IsFalse(
				Support.bUseParentStruct,
				FString::Printf(TEXT("%s 用对象自己的类编解码"), *Kind)));
		}

		const TArray<FString> NoResetKinds = {
			TEXT("skeletal-mesh"), TEXT("skeleton"), TEXT("physics-asset"), TEXT("anim-sequence"),
		};
		for (const FString& Kind : NoResetKinds)
		{
			const ForgeKindRegistry::FForgeDiffSupport Support =
				ForgeKindRegistry::GetPropertyDiffSupport(Kind);
			ASSERT_THAT(IsFalse(
				Support.bResetBeforeApply,
				FString::Printf(TEXT("%s 的导入不重置，配方必须一致"), *Kind)));
		}

		// 蓝图是唯一的父类基准：配置写的是相对父类的增量。
		const ForgeKindRegistry::FForgeDiffSupport ObjectBp =
			ForgeKindRegistry::GetPropertyDiffSupport(TEXT("object-blueprint"));
		ASSERT_THAT(IsTrue(ObjectBp.bResetBeforeApply, TEXT("object-blueprint 用父类默认重置")));
		ASSERT_THAT(IsTrue(ObjectBp.bUseParentStruct, TEXT("object-blueprint 的 struct 是父类")));
	}

	/**
	 * 不支持的 8 种必须给出非空理由。
	 *
	 * 理由不能是"暂不支持"这种空话——它是结构性事实（值不是 UObject / 需要先选节点）。
	 * 这条断言同时防止未来有人把不支持的分支写成只返回 false 而不给说明。
	 */
	TEST_METHOD(不支持的kind必须给出结构性理由)
	{
		const TArray<FString> UnsupportedKinds = {
			TEXT("data-table"), TEXT("anim-blueprint"), TEXT("niagara"), TEXT("material"),
			TEXT("umg-widget"), TEXT("actor-blueprint"), TEXT("behavior-tree"), TEXT("state-tree"),
		};

		for (const FString& Kind : UnsupportedKinds)
		{
			const ForgeKindRegistry::FForgeDiffSupport Support =
				ForgeKindRegistry::GetPropertyDiffSupport(Kind);
			ASSERT_THAT(IsFalse(Support.bSupported, FString::Printf(TEXT("%s 不应支持属性差异"), *Kind)));
			ASSERT_THAT(IsFalse(
				Support.Reason.IsEmpty(),
				FString::Printf(TEXT("%s 必须给出不支持的理由（不能静默）"), *Kind)));
			ASSERT_THAT(IsTrue(
				Support.Source == ForgeKindRegistry::EForgeDiffObjectSource::None,
				FString::Printf(TEXT("%s 不支持时不该给对象来源"), *Kind)));
		}
	}

	/** 未登记的 kind 也要给理由——拼错 kind 或配置来自更新版本的插件时，界面要直说而不是给空面板。 */
	TEST_METHOD(未登记的kind给出可诊断的理由)
	{
		const ForgeKindRegistry::FForgeDiffSupport Support =
			ForgeKindRegistry::GetPropertyDiffSupport(TEXT("some-future-kind"));

		ASSERT_THAT(IsFalse(Support.bSupported, TEXT("未登记的 kind 默认不支持")));
		ASSERT_THAT(IsTrue(
			Support.Reason.Contains(TEXT("some-future-kind")),
			TEXT("理由里应带上那个 kind，方便定位是漏登记了")));
	}

	/** 支持范围与管道登记的 kind 名单必须能对上：两者不一致说明表漂移了。 */
	TEST_METHOD(支持范围覆盖管道登记的全部kind)
	{
		const TArray<FString> AllKinds = ForgeKindRegistry::GetSupportedKinds();
		ASSERT_THAT(AreEqual(17, AllKinds.Num(), TEXT("登记表当前有 17 种 kind；新增时同步本断言并为它填好 DiffSupport")));

		int32 Supported = 0;
		for (const FString& Kind : AllKinds)
		{
			const ForgeKindRegistry::FForgeDiffSupport Support =
				ForgeKindRegistry::GetPropertyDiffSupport(Kind);
			if (Support.bSupported)
			{
				++Supported;
			}
			else
			{
				// 不支持的必须说得出为什么——包括未来新增的那种（走"未登记"分支也算说了）。
				ASSERT_THAT(IsFalse(Support.Reason.IsEmpty(), FString::Printf(TEXT("%s 必须给出理由"), *Kind)));
			}
		}

		ASSERT_THAT(AreEqual(8, Supported, TEXT("当前支持 8 种；数量变了说明登记表的 DiffSupport 改了，确认与导入代码一致")));
	}
};

#endif // WITH_AUTOMATION_WORKER
