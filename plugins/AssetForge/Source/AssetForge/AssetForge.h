// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/** 本插件的日志分类。 */
DECLARE_LOG_CATEGORY_EXTERN(LogAssetForge, Log, All);

class FAssetForgeAssetMenu;

/**
 * AssetForge 模块：把 `forge.*` 操作挂到 UELink 上、注册内容浏览器右键菜单。
 *
 * 管理界面在 Agent 面板的网页上（AssetForge 页），UE 里不再有窗口、停靠页和自己的 HTTP 服务：
 * 通信、令牌、多实例都交给 UELink，这里只登记操作。
 */
class FAssetForgeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** 在 .cpp 里定义：头文件只前向声明了 FAssetForgeAssetMenu，TUniquePtr 的析构需要完整类型。 */
	virtual ~FAssetForgeModule() override;

private:
	/** 内容浏览器资产右键菜单（从配置导入 / 导出为配置）。 */
	TUniquePtr<FAssetForgeAssetMenu> AssetMenu;
};
