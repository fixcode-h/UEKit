// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

struct FAssetData;

/**
 * kind 登记表：**全插件唯一知道「有哪些资产类型」的地方**。
 *
 * 加一种资产类型 = 在 `ForgeKindRegistry.cpp` 的表里加一行，写清：
 * kind 名、中文名、配置子目录、对应的资产类、（可选）细分判定、导入 / 导出函数、属性差异支持。
 * 管道、状态判定、面板操作、命令行、右键菜单都只读这张表，不再各自写 IsA 链或 switch。
 */
namespace ForgeKindRegistry
{
	/** 导入入口；各处理器签名一致。会就地归一化文档，所以不是 const。 */
	using FImportDocumentFn = bool (*)(FForgeDocument&, bool /*bDryRun*/, FForgeReport&);

	/** 导出入口；各处理器签名一致。 */
	using FExportAssetFn = bool (*)(const FString& /*AssetPath*/, FForgeDocument&, FForgeReport&);

	/** 资产类 getter（函数而不是 UClass*：表是静态初始化的，那时 UClass 还没注册）。 */
	using FAssetClassFn = UClass* (*)();

	/**
	 * 细分判定：同一个资产类对应多个 kind 时用（Actor 蓝图 / 对象蓝图都是 UBlueprint）。
	 * `InNativeParent` 是蓝图的原生父类（非蓝图资产为空）。返回 false = 不归这个 kind。
	 */
	using FAcceptsFn = bool (*)(const UClass* InAssetClass, const UClass* InNativeParent);

	/** 属性差异视图要从哪拿「被比较的那个对象」。 */
	enum class EForgeDiffObjectSource : uint8
	{
		/** 拿不到——该 kind 不做对象级属性差异。 */
		None,
		/** 资产自身（`LoadObject`）。 */
		AssetItself,
		/** 蓝图生成的类的 CDO。 */
		GeneratedClassDefaultObject,
		/** 地图的 `AWorldSettings`。 */
		WorldSettings,
	};

	/**
	 * 「这个 kind 能否做对象级属性差异」的答案。
	 *
	 * `bSupported` 为假时 `Reason` 必非空——那是给用户看的**结构性**理由（"它的值不是 UObject 属性"），
	 * 不是"没实现"。宁可在界面上说清为什么，也不显示一个空面板让人误以为"没有差异"。
	 */
	struct ASSETFORGE_API FForgeDiffSupport
	{
		bool bSupported = false;
		EForgeDiffObjectSource Source = EForgeDiffObjectSource::None;

		/**
		 * 应用配置前是否先把副本重置到默认——**必须与真实导入一致**
		 * （`data-asset` / `texture` / `map` 先重置；`skeletal-mesh` 等纳管型只写声明的属性）。
		 */
		bool bResetBeforeApply = false;

		/** 编解码属性时用「父类」而不是「对象的类」（只有 `object-blueprint`：它写的是相对父类的增量）。 */
		bool bUseParentStruct = false;

		/** 不支持的原因（面向用户）或支持时的备注。 */
		FString Reason;
	};

	/** 一种资产的接入点。 */
	struct ASSETFORGE_API FForgeKindInfo
	{
		const TCHAR* Kind = nullptr;
		const TCHAR* DisplayName = nullptr;
		/** 配置根下的子目录名（`umg`、`data`……）。 */
		const TCHAR* ConfigDirName = nullptr;
		FAssetClassFn GetAssetClass = nullptr;
		/** 可空：空 = 资产类匹配即接受。 */
		FAcceptsFn Accepts = nullptr;
		FImportDocumentFn Import = nullptr;
		FExportAssetFn Export = nullptr;
		FForgeDiffSupport DiffSupport;
	};

	/** 全部已接入的 kind；顺序即展示顺序。 */
	ASSETFORGE_API TConstArrayView<FForgeKindInfo> GetAll();

	/** 按 kind 查；未登记返回空。 */
	ASSETFORGE_API const FForgeKindInfo* Find(const FString& InKind);

	/** 已接入的 kind 名单。 */
	ASSETFORGE_API TArray<FString> GetSupportedKinds();

	/** 是否已接入。 */
	ASSETFORGE_API bool IsSupportedKind(const FString& InKind);

	/** 中文显示名；未登记的原样返回。 */
	ASSETFORGE_API FString GetDisplayName(const FString& InKind);

	/** 配置子目录名；未登记的返回 kind 本身（至少不会把文件写到别处）。 */
	ASSETFORGE_API FString GetConfigDirName(const FString& InKind);

	/**
	 * 资产类（+ 蓝图的原生父类）→ kind；空 = 不支持。
	 *
	 * 在所有资产类匹配的项里取**继承层级最深**者，再过细分判定——
	 * 不再依赖手写 IsA 链的顺序（`UWidgetBlueprint` 是 `UBlueprint`、`UStateTree` 是 `UDataAsset`，
	 * 深度天然把它们排在前面）。
	 */
	ASSETFORGE_API FString ClassifyAssetClass(const UClass* InAssetClass, const UClass* InNativeParent = nullptr);

	/** 已加载资产 → kind（蓝图会取其原生父类参与判定）。 */
	ASSETFORGE_API FString ClassifyAsset(const UObject* InAsset);

	/** 资产注册表条目 → kind，**不加载资产**（蓝图读 `NativeParentClass` 标签）。 */
	ASSETFORGE_API FString ClassifyAssetData(const FAssetData& InAssetData);

	/** kind → 属性差异支持情况；未登记的 kind 给出明确理由。 */
	ASSETFORGE_API FForgeDiffSupport GetPropertyDiffSupport(const FString& InKind);
}
