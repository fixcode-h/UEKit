// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AssetForgeCommandlet.generated.h"

/**
 * 不开编辑器界面时的入口。所有操作都调 `ForgeService`，与面板操作（forge.*）、右键菜单同一份实现。
 *
 * 用法（`UnrealEditor-Cmd.exe MyGame.uproject -run=AssetForge ...`）：
 *
 *   -status [-deep] [-unmanaged]      列出全部条目与同步状态
 *   -config=<文件>[,<文件>...]        导入指定配置
 *   -pending                          导入所有需要导入的配置
 *   -all                              导入全部配置
 *   -export=<资产>[,<资产>...]        把资产导出到配置（纳管 / 重新导出）
 *   -dry-run                          与导入类参数合用：只预览计划，不改资产
 *   -json                             结果以 JSON 打到标准输出（一行，前缀 `ASSETFORGE_JSON:`）
 *
 * 退出码：0 = 成功；1 = 有失败项；2 = 参数错误。
 *
 * `-run=` 下引擎会创建 `UEditorEngine`（UMGEditor 可用），但**不会创建 `FSlateApplication`**，
 * 所以这条路径上不能碰 TakeWidget、缩略图与弹窗（Service 的进度框在 commandlet 下自动不弹）。
 */
UCLASS()
class UAssetForgeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UAssetForgeCommandlet();

	virtual int32 Main(const FString& Params) override;

	enum EExitCode : int32
	{
		ExitOk = 0,
		ExitFailures = 1,
		ExitUsage = 2,
	};
};
