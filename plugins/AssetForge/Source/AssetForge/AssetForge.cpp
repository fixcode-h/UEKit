// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetForge.h"

#include "AgentPanelBridge.h"
#include "AssetForgeCompat.h"
#include "AssetForgeVersion.h"
#include "Integration/AssetForgeAssetMenu.h"
#include "Integration/ForgeOps.h"
#include "Service/ForgeSamples.h"
#include "Service/ForgeSchemaFiles.h"

DEFINE_LOG_CATEGORY(LogAssetForge);

FAssetForgeModule::~FAssetForgeModule() = default;

void FAssetForgeModule::StartupModule()
{
	// 后台编辑器（-run=AgentPanel）里也登记：面板可以不开编辑器界面就批量导入
	FAgentPanelBridge::Get().RegisterExtension(TEXT("AssetForge"), ASSETFORGE_VERSION);
	ForgeOps::Register();

	// 插件自带的示例测试界面：配置装进配置根 Test/，配套场景装进 GameTest 场景根（项目设置里可关）
	ForgeSamples::Install();

	// 配置根 .schema/ 下的 JSON Schema：配置头部的 $schema 指向它，编辑器改配置时边写边检查（内容没变不重写）
	ForgeSchemaFiles::Install();

	// 右键菜单只在有界面的编辑器里有意义
	if (!IsRunningCommandlet())
	{
		AssetMenu = MakeUnique<FAssetForgeAssetMenu>();
		AssetMenu->Register();
	}
}

void FAssetForgeModule::ShutdownModule()
{
	// 先注销菜单：注销后内容浏览器不再持有指向本模块对象的委托
	if (AssetMenu.IsValid())
	{
		AssetMenu->Unregister();
		AssetMenu.Reset();
	}
	ForgeOps::Unregister();
	FAgentPanelBridge::Get().UnregisterExtension(TEXT("AssetForge"));
}

IMPLEMENT_MODULE(FAssetForgeModule, AssetForge)
