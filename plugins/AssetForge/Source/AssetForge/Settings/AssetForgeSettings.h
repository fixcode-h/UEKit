// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "AssetForgeSettings.generated.h"

/**
 * 项目级设置（入库）：落在 `Config/DefaultEditor.ini` 的 `[/Script/AssetForge.AssetForgeSettings]` 段。
 * 在「项目设置 → 插件 → Asset Forge」里改。
 *
 * 这里只放**全队共享**的约定。界面在 Agent 面板的网页上，界面状态（筛选、分组）由网页自己记，
 * 通信走 UELink（没有自己的端口设置）。
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "Asset Forge"))
class ASSETFORGE_API UAssetForgeSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UAssetForgeSettings();

	virtual FName GetCategoryName() const override;

	/**
	 * 配置根目录：相对项目根（也接受绝对路径）。留空自动：用 Agent 面板的项目文档根下的 `AssetForge`（面板写在 `Saved/AgentPanel/docs-root.txt`，P4 / SVN 项目是各分支共用的 `ProjectDoc/docs`）；
	 * 没有面板时项目外层同级有 `ProjectDoc/docs` 就用 `ProjectDoc/docs/AssetForge`，否则用项目里的 `docs/AssetForge`。
	 * 各 kind 的配置在它下面的子目录里（`umg/`、`data/`……，见 kind 登记表）。
	 * 同步清单不放这里：它记的是本机资产文件的修改时间，放在 `Saved/AssetForge/state.json`（不进版本管理）。
	 */
	UPROPERTY(EditAnywhere, config, Category = "AssetForge", meta = (DisplayName = "配置根目录", RelativeToGameDir))
	FDirectoryPath ConfigRoot;

	/** 「未纳管资产」扫描的内容根（如 `/Game`）。 */
	UPROPERTY(EditAnywhere, config, Category = "AssetForge", meta = (DisplayName = "未纳管资产扫描根"))
	TArray<FString> UnmanagedScanRoots;

	/**
	 * 编辑器启动时把插件自带的示例（`Plugins/AssetForge/docs/AssetForge/Test`：多页面测试界面的配置、配套的 GameTest 场景）
	 * 装进配置根的 `Test/` 和 GameTest 场景根的 `AssetForgeSample/`。只补缺的、只更新没被改过的，删掉的不会再装回来。
	 */
	UPROPERTY(EditAnywhere, config, Category = "AssetForge", meta = (DisplayName = "安装示例测试界面"))
	bool bInstallSamples = true;

	/** 配置根的磁盘绝对路径（已规范化、正斜杠，不带结尾斜杠）。 */
	FString ResolveConfigRootDir() const;

	/**
	 * 项目文档根下的子目录（如 `GameTest`）：文档根优先用 Agent 面板写的 `Saved/AgentPanel/docs-root.txt`，
	 * 没有时按旧规则（`ProjectDoc/docs` 或项目里的 `docs`），和 GameTest 同一个约定。
	 */
	static FString ResolveDocsSubDirPath(const TCHAR* Name);
};
