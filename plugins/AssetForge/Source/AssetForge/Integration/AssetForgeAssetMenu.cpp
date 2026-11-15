// Copyright Epic Games, Inc. All Rights Reserved.

#include "Integration/AssetForgeAssetMenu.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Registry/ForgeKindRegistry.h"
#include "Service/ForgeService.h"
#include "State/ForgeState.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "AssetForge.h"
#include "AgentPanelBridge.h"
#include "Dom/JsonObject.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "AssetForgeAssetMenu"

namespace
{
	/** 右键菜单改了配置或资产：告诉开着 AssetForge 页的面板重新拉列表。 */
	void PublishChanged(const int32 InCount)
	{
		const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
		Event->SetStringField(TEXT("source"), TEXT("assetMenu"));
		Event->SetNumberField(TEXT("count"), InCount);
		FAgentPanelBridge::Get().PublishEvent(TEXT("forge.changed"), Event);
	}
}

namespace
{
	/** 一个可被本管道处理的选中资产。 */
	struct FMenuAsset
	{
		FString AssetPath;
		FString Kind;
	};

	/** 从选中路径里挑出可处理的资产；判断不了类型的直接略过（菜单可见性也靠它）。 */
	TArray<FMenuAsset> ResolveSupportedAssets(const TArray<FString>& InSelectedPaths)
	{
		TArray<FMenuAsset> Result;
		Result.Reserve(InSelectedPaths.Num());

		const IAssetRegistry& AssetRegistry = FAssetRegistryModule::GetRegistry();

		for (const FString& AssetPath : InSelectedPaths)
		{
			// 内容浏览器给的是资产路径字符串，类型要经 AssetRegistry 查——查不到（尚未扫描）就不显示。
			const FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
			if (!AssetData.IsValid())
			{
				continue;
			}

			const FString Kind = ForgeKindRegistry::ClassifyAssetData(AssetData);
			if (!Kind.IsEmpty())
			{
				Result.Add({ AssetData.PackageName.ToString(), Kind });
			}
		}

		return Result;
	}

	void NotifyBatch(const FForgeBatchResult& InResult, const FText& InVerb)
	{
		FText Text = FText::Format(LOCTEXT("NotifyPrefix", "Asset Forge：{0}"), InResult.GetSummary(InVerb));
		if (InResult.HasFailures())
		{
			Text = FText::Format(LOCTEXT("NotifyFailures", "{0}（失败原因见 Output Log，或在 Agent 面板的 AssetForge 页查看诊断）"), Text);
		}

		FNotificationInfo Info(Text);
		Info.ExpireDuration = InResult.HasFailures() ? 10.f : 4.f;
		Info.bUseSuccessFailIcons = true;

		const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(InResult.HasFailures() ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
		}
	}
}

void FAssetForgeAssetMenu::Register()
{
	FContentBrowserModule& ContentBrowserModule =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

	// 资产视图里右键资产的菜单是 AssetView 那组扩展点（参数是选中的 FAssetData）；
	// `GetAllAssetContextMenuExtenders` 那组传的是文件夹路径，挂在那里菜单项永远不出现。
	FContentBrowserMenuExtender_SelectedAssets Delegate =
		FContentBrowserMenuExtender_SelectedAssets::CreateRaw(this, &FAssetForgeAssetMenu::OnExtendAssetContextMenu);

	ExtenderHandle = Delegate.GetHandle();
	ContentBrowserModule.GetAllAssetViewContextMenuExtenders().Add(MoveTemp(Delegate));
}

void FAssetForgeAssetMenu::Unregister()
{
	if (!ExtenderHandle.IsValid())
	{
		return;
	}

	if (FContentBrowserModule* ContentBrowserModule =
		FModuleManager::GetModulePtr<FContentBrowserModule>(TEXT("ContentBrowser")))
	{
		// 按 handle 匹配：委托在数组里的那份拷贝与原对象共享同一个 handle。
		ContentBrowserModule->GetAllAssetViewContextMenuExtenders().RemoveAll(
			[this](const FContentBrowserMenuExtender_SelectedAssets& InDelegate)
			{
				return InDelegate.GetHandle() == ExtenderHandle;
			});
	}

	ExtenderHandle.Reset();
}

TSharedRef<FExtender> FAssetForgeAssetMenu::OnExtendAssetContextMenu(const TArray<FAssetData>& InSelectedAssets)
{
	const TSharedRef<FExtender> Extender = MakeShared<FExtender>();

	TArray<FString> InSelectedPaths;
	InSelectedPaths.Reserve(InSelectedAssets.Num());
	for (const FAssetData& AssetData : InSelectedAssets)
	{
		InSelectedPaths.Add(AssetData.GetSoftObjectPath().ToString());
	}

	// 一项都处理不了的时候不加扩展：菜单里出现两个"点了必然报错"的入口，比不出现更糟。
	if (ResolveSupportedAssets(InSelectedPaths).Num() == 0)
	{
		return Extender;
	}

	Extender->AddMenuExtension(
		TEXT("GetAssetActions"),
		EExtensionHook::After,
		nullptr,
		FMenuExtensionDelegate::CreateRaw(this, &FAssetForgeAssetMenu::OnExtendMenu, InSelectedPaths));

	return Extender;
}

void FAssetForgeAssetMenu::OnExtendMenu(FMenuBuilder& InMenuBuilder, const TArray<FString> InSelectedPaths)
{
	InMenuBuilder.BeginSection(TEXT("AssetForge"), LOCTEXT("Section", "Asset Forge"));

	InMenuBuilder.AddMenuEntry(
		LOCTEXT("ImportFromConfig", "从配置导入"),
		LOCTEXT("ImportFromConfigTooltip", "用该资产对应的 JSON 配置覆盖它（配置是唯一真源）"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Toolbar.Import")),
		FUIAction(FExecuteAction::CreateRaw(this, &FAssetForgeAssetMenu::ExecuteImport, InSelectedPaths)));

	InMenuBuilder.AddMenuEntry(
		LOCTEXT("ExportToConfig", "导出到配置"),
		LOCTEXT("ExportToConfigTooltip", "把该资产写回 JSON 配置：已有配置写回原处（保留 revision），没有则新建（纳管）"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Toolbar.Export")),
		FUIAction(FExecuteAction::CreateRaw(this, &FAssetForgeAssetMenu::ExecuteExport, InSelectedPaths)));

	InMenuBuilder.AddMenuEntry(
		LOCTEXT("RevealInForge", "在 Agent 面板中显示"),
		LOCTEXT("RevealInForgeTooltip", "在 Agent 面板的 AssetForge 页选中该资产（或它的配置）；面板要开着并连上这个编辑器"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Search")),
		FUIAction(FExecuteAction::CreateRaw(this, &FAssetForgeAssetMenu::ExecuteReveal, InSelectedPaths)));

	InMenuBuilder.EndSection();
}

void FAssetForgeAssetMenu::ExecuteImport(TArray<FString> InSelectedPaths)
{
	TArray<FString> Configs;
	int32 Missing = 0;

	for (const FMenuAsset& Asset : ResolveSupportedAssets(InSelectedPaths))
	{
		const FString ConfigPath = ForgeState::FindConfigForAsset(Asset.AssetPath, Asset.Kind);
		if (ConfigPath.IsEmpty())
		{
			UE_LOG(LogAssetForge, Error,
				TEXT("[右键导入] %s：没有对应配置——先「导出到配置」把它纳管。"), *Asset.AssetPath);
			++Missing;
			continue;
		}
		Configs.Add(ConfigPath);
	}

	FForgeBatchResult Result = ForgeService::ImportConfigs(Configs, /*bInDryRun=*/false, /*bInShowProgress=*/true);
	Result.Failed += Missing;
	NotifyBatch(Result, LOCTEXT("ImportVerb", "导入"));
	PublishChanged(Configs.Num());
}

void FAssetForgeAssetMenu::ExecuteExport(TArray<FString> InSelectedPaths)
{
	TArray<FString> AssetPaths;
	for (const FMenuAsset& Asset : ResolveSupportedAssets(InSelectedPaths))
	{
		AssetPaths.Add(Asset.AssetPath);
	}

	const FForgeBatchResult Result = ForgeService::ExportAssets(AssetPaths, /*bInShowProgress=*/true);
	NotifyBatch(Result, LOCTEXT("ExportVerb", "导出"));
	PublishChanged(AssetPaths.Num());
}

void FAssetForgeAssetMenu::ExecuteReveal(TArray<FString> InSelectedPaths)
{
	const TArray<FMenuAsset> Assets = ResolveSupportedAssets(InSelectedPaths);
	if (Assets.Num() > 0)
	{
		// 界面在面板网页上：推一条事件，开着 AssetForge 页的面板会跳到这一项
		const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
		Event->SetStringField(TEXT("asset"), Assets[0].AssetPath);
		Event->SetStringField(TEXT("kind"), Assets[0].Kind);
		FAgentPanelBridge::Get().PublishEvent(TEXT("forge.reveal"), Event);
	}
}

#undef LOCTEXT_NAMESPACE
