// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "CoreMinimal.h"

struct FAssetData;
class FExtender;
class FMenuBuilder;

/**
 * 内容浏览器资产右键菜单：「从配置导入」「导出到配置」「在 Asset Forge 中显示」。
 *
 * 只是入口：导入 / 导出都调 `ForgeService`，与面板操作、命令行同一份实现。
 * 只对可被本管道处理的资产显示——判定读 `ForgeKindRegistry::ClassifyAssetData`（不加载资产）。
 */
class FAssetForgeAssetMenu
{
public:
	/** 注册到内容浏览器的资产右键菜单（模块启动时调用一次）。 */
	void Register();

	/** 注销（模块关闭时；避免留下指向已析构对象的回调）。 */
	void Unregister();

private:
	TSharedRef<FExtender> OnExtendAssetContextMenu(const TArray<FAssetData>& InSelectedAssets);
	void OnExtendMenu(FMenuBuilder& InMenuBuilder, const TArray<FString> InSelectedPaths);

	void ExecuteImport(TArray<FString> InSelectedPaths);
	void ExecuteExport(TArray<FString> InSelectedPaths);
	void ExecuteReveal(TArray<FString> InSelectedPaths);

	/**
	 * 注册时拿到、注销时按它移除。`GetAllAssetViewContextMenuExtenders()` 是普通 TArray，
	 * 而 `TDelegate` 不可比较，所以按委托的 handle 在 `RemoveAll` 的谓词里匹配。
	 */
	FDelegateHandle ExtenderHandle;
};
