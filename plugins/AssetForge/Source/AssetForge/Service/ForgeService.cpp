// Copyright Epic Games, Inc. All Rights Reserved.

#include "Service/ForgeService.h"
#include "AssetForge.h"

#include "Core/ForgePipeline.h"
#include "Registry/ForgeKindRegistry.h"
#include "Service/ForgeBatch.h"
#include "Service/ForgeEditorUtils.h"
#include "Settings/AssetForgeSettings.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"

#define LOCTEXT_NAMESPACE "ForgeService"

// ---------------------------------------------------------------- FForgeEntry

FString FForgeEntry::GetDisplayName() const
{
	if (IsConfig())
	{
		return FPaths::GetBaseFilename(ConfigPath);
	}
	return FPaths::GetBaseFilename(AssetPath);
}

bool FForgeEntry::CanImport() const
{
	return IsConfig()
		&& State != EForgeSyncState::ParseError
		&& State != EForgeSyncState::Foreign
		&& ForgeKindRegistry::IsSupportedKind(Kind);
}

bool FForgeEntry::CanExport() const
{
	return HasAsset()
		&& State != EForgeSyncState::Foreign
		&& State != EForgeSyncState::AssetMissing
		&& ForgeKindRegistry::IsSupportedKind(Kind);
}

TSharedPtr<FJsonObject> FForgeEntry::ToJson() const
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("key"), Key);
	Json->SetStringField(TEXT("name"), GetDisplayName());
	Json->SetStringField(TEXT("state"), ForgeState::ToId(State));
	Json->SetBoolField(TEXT("needsImport"), ForgeState::NeedsImport(State));
	if (IsConfig())
	{
		Json->SetStringField(TEXT("config"), RelativePath);
		Json->SetStringField(TEXT("configPath"), ConfigPath);
	}
	if (HasAsset())
	{
		Json->SetStringField(TEXT("asset"), AssetPath);
	}
	if (!Kind.IsEmpty())
	{
		Json->SetStringField(TEXT("kind"), Kind);
	}
	if (!ForeignKind.IsEmpty())
	{
		Json->SetStringField(TEXT("foreignKind"), ForeignKind);
	}
	if (IsConfig())
	{
		Json->SetNumberField(TEXT("revision"), Revision);
	}
	if (!Message.IsEmpty())
	{
		Json->SetStringField(TEXT("message"), Message);
	}
	return Json;
}

// ---------------------------------------------------------------- FForgeBatchResult

FText FForgeBatchResult::GetSummary(const FText& InVerb) const
{
	FText Text = FText::Format(
		LOCTEXT("BatchSummary", "{0}：成功 {1}，失败 {2}"),
		InVerb,
		FText::AsNumber(Succeeded),
		FText::AsNumber(Failed));

	if (Skipped > 0)
	{
		Text = FText::Format(LOCTEXT("BatchSummarySkipped", "{0}，跳过 {1}"), Text, FText::AsNumber(Skipped));
	}

	if (bCancelled)
	{
		Text = FText::Format(LOCTEXT("BatchSummaryCancelled", "{0}（已取消）"), Text);
	}

	return Text;
}

TSharedPtr<FJsonObject> FForgeBatchResult::ToJson() const
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("ok"), !HasFailures() && !bCancelled);
	Json->SetNumberField(TEXT("succeeded"), Succeeded);
	Json->SetNumberField(TEXT("failed"), Failed);
	Json->SetNumberField(TEXT("skipped"), Skipped);
	Json->SetBoolField(TEXT("cancelled"), bCancelled);

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FForgeReport& Report : Reports)
	{
		Items.Add(MakeShared<FJsonValueObject>(ForgePipeline::ReportToJson(Report)));
	}
	Json->SetArrayField(TEXT("reports"), Items);
	return Json;
}

namespace ForgeService
{

namespace
{
	FString MakeRelativeToRoot(const FString& InAbsolutePath)
	{
		FString Relative = InAbsolutePath;
		Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
		FPaths::MakePathRelativeTo(Relative, *(ForgePipeline::GetConfigRootDir() + TEXT("/")));
		return Relative;
	}

	bool ShouldShowDialog(const bool bInShowProgress)
	{
		return bInShowProgress && !IsRunningCommandlet() && GIsEditor;
	}

	/** 同步跑完一个批量：右键菜单、命令行用；可取消的进度框只在编辑器里弹。 */
	void RunBatch(FForgeBatchRunner& InOutRunner, const FText& InTitle, const bool bInShowProgress)
	{
		FScopedSlowTask Task(static_cast<float>(InOutRunner.GetTotal()), InTitle);
		if (ShouldShowDialog(bInShowProgress))
		{
			Task.MakeDialog(/*bShowCancelButton=*/true);
		}
		int32 Reported = 0;
		while (!InOutRunner.IsDone())
		{
			if (Task.ShouldCancel())
			{
				InOutRunner.Cancel();
				break;
			}
			Task.EnterProgressFrame(0.f, FText::FromString(InOutRunner.GetCurrent()));
			InOutRunner.Step();
			const int32 Finished = InOutRunner.GetFinished();
			Task.EnterProgressFrame(static_cast<float>(Finished - Reported));
			Reported = Finished;
		}
	}

	void LogReport(const TCHAR* InContext, const FForgeReport& InReport)
	{
		if (InReport.bSucceeded)
		{
			UE_LOG(LogAssetForge, Log, TEXT("[%s] %s"), InContext, *ForgePipeline::ReportToText(InReport));
		}
		else
		{
			UE_LOG(LogAssetForge, Warning, TEXT("[%s] %s"), InContext, *ForgePipeline::ReportToText(InReport));
		}
	}
}

TArray<FForgeEntry> ScanConfigs()
{
	const FString Root = ForgePipeline::GetConfigRootDir();

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Root, TEXT("*.json"), /*Files=*/true, /*Directories=*/false);

	TArray<FForgeEntry> Entries;
	Entries.Reserve(Files.Num());

	for (const FString& File : Files)
	{
		// 生成物判定只看**相对配置根**的那一段：绝对路径里恰好有 `_` 开头的目录（`D:/_work/...`）不该误杀。
		if (ForgeEditorUtils::IsGeneratedPath(MakeRelativeToRoot(File)))
		{
			continue;
		}

		Entries.Add(DescribeConfig(File));
	}

	Entries.Sort([](const FForgeEntry& A, const FForgeEntry& B) { return A.RelativePath < B.RelativePath; });
	return Entries;
}

FForgeEntry DescribeConfig(const FString& InConfigPath)
{
	FForgeEntry Entry;
	Entry.ConfigPath = FPaths::ConvertRelativePathToFull(ForgePipeline::ResolveConfigPath(InConfigPath));
	Entry.RelativePath = MakeRelativeToRoot(Entry.ConfigPath);
	Entry.Key = Entry.RelativePath;

	const ForgeEditorUtils::FForgeConfigClassification Classification =
		ForgeEditorUtils::ClassifyConfigAtPath(Entry.ConfigPath);

	switch (Classification.Class)
	{
	case ForgeEditorUtils::EForgeConfigClass::Parsed:
		Entry.Kind = Classification.Kind;
		Entry.AssetPath = ForgeState::NormalizeAssetPath(Classification.AssetPath);
		Entry.ParentClass = Classification.ParentClass;
		Entry.Revision = Classification.Revision;
		Entry.State = ForgeState::QuickState(Entry.ConfigPath, Entry.AssetPath);
		break;

	case ForgeEditorUtils::EForgeConfigClass::ForeignKind:
		Entry.ForeignKind = Classification.ForeignKind;
		Entry.State = EForgeSyncState::Foreign;
		Entry.Message = ForgeState::GetDescription(EForgeSyncState::Foreign).ToString();
		break;

	default:
		Entry.State = EForgeSyncState::ParseError;
		Entry.Message = Classification.ErrorMessage;
		break;
	}

	return Entry;
}

TArray<FForgeEntry> ScanUnmanagedAssets(const TSet<FString>& InManagedAssetPaths)
{
	TArray<FForgeEntry> Entries;

	const IAssetRegistry& AssetRegistry = FAssetRegistryModule::GetRegistry();
	const UAssetForgeSettings* Settings = GetDefault<UAssetForgeSettings>();

	TArray<FString> Roots = Settings->UnmanagedScanRoots;
	if (Roots.Num() == 0)
	{
		Roots.Add(TEXT("/Game"));
	}

	TSet<FString> Seen;
	for (const FString& Root : Roots)
	{
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPath(FName(*Root), Assets, /*bRecursive=*/true);

		for (const FAssetData& AssetData : Assets)
		{
			const FString PackageName = AssetData.PackageName.ToString();
			if (Seen.Contains(PackageName)
				|| InManagedAssetPaths.Contains(PackageName)
				|| ForgeEditorUtils::IsNoiseAssetPath(PackageName))
			{
				continue;
			}

			const FString Kind = ForgeKindRegistry::ClassifyAssetData(AssetData);
			if (Kind.IsEmpty())
			{
				continue;
			}

			Seen.Add(PackageName);

			FForgeEntry& Entry = Entries.AddDefaulted_GetRef();
			Entry.Key = PackageName;
			Entry.AssetPath = PackageName;
			Entry.Kind = Kind;
			Entry.State = EForgeSyncState::Unmanaged;
		}
	}

	Entries.Sort([](const FForgeEntry& A, const FForgeEntry& B) { return A.AssetPath < B.AssetPath; });
	return Entries;
}

TArray<FForgeEntry> ScanAll(const bool bInIncludeUnmanaged)
{
	TArray<FForgeEntry> Entries = ScanConfigs();

	if (bInIncludeUnmanaged)
	{
		// 「已纳管」= 某份配置声明了它，或清单里记得它——两个来源取并集，口径只有这一处。
		TSet<FString> Managed = ForgeState::GetRecordedAssetPaths();
		for (const FForgeEntry& Entry : Entries)
		{
			if (Entry.HasAsset())
			{
				Managed.Add(Entry.AssetPath);
			}
		}

		Entries.Append(ScanUnmanagedAssets(Managed));
	}

	return Entries;
}

FForgeReport Plan(const FString& InConfigPath, EForgeSyncState* OutState)
{
	const FString ConfigPath = ForgePipeline::ResolveConfigPath(InConfigPath);

	FForgeReport Report;
	const EForgeSyncState State = ForgeState::DeepCheck(ConfigPath, FString(), &Report);
	Report.SourcePath = ConfigPath;

	if (OutState != nullptr)
	{
		*OutState = State;
	}
	return Report;
}

FForgeReport PlanText(const FString& InConfigPath, const FString& InConfigText)
{
	const FString ConfigPath = ForgePipeline::ResolveConfigPath(InConfigPath);

	FForgeReport Report;
	Report.bDryRun = true;
	FForgeDocument Document;
	if (ForgePipeline::LoadDocumentFromText(InConfigText, ConfigPath, Document, Report))
	{
		ForgePipeline::Import(Document, /*bDryRun=*/true, Report);
	}
	Report.SourcePath = ConfigPath;
	return Report;
}

FForgeReport ImportConfig(const FString& InConfigPath, const bool bInDryRun)
{
	const FString ConfigPath = ForgePipeline::ResolveConfigPath(InConfigPath);

	FForgeReport Report;
	Report.bDryRun = bInDryRun;

	FForgeDocument Document;
	if (!ForgePipeline::LoadDocument(ConfigPath, Document, Report))
	{
		Report.bSucceeded = false;
		LogReport(TEXT("导入"), Report);
		return Report;
	}

	ForgePipeline::Import(Document, bInDryRun, Report);

	// 真跑成功就记清单；零动作也要记——那正是「已同步」该有的样子。
	if (Report.bSucceeded && !bInDryRun)
	{
		ForgeState::RecordImport(Document);
	}

	LogReport(bInDryRun ? TEXT("预览") : TEXT("导入"), Report);
	return Report;
}

FForgeReport ImportAsset(const FString& InAssetPath, const bool bInDryRun)
{
	const FString AssetPath = ForgeState::NormalizeAssetPath(InAssetPath);

	FString Kind;
	if (const UObject* Asset = LoadObject<UObject>(nullptr, *ForgeState::ToObjectPath(AssetPath)))
	{
		Kind = ForgeKindRegistry::ClassifyAsset(Asset);
	}

	const FString ConfigPath = ForgeState::FindConfigForAsset(AssetPath, Kind);
	if (ConfigPath.IsEmpty())
	{
		FForgeReport Report;
		Report.bDryRun = bInDryRun;
		Report.AssetPath = AssetPath;
		Report.AddError(TEXT("asset"), FString::Printf(TEXT("资产没有对应的配置（清单与默认路径都找不到）：%s"), *AssetPath));
		return Report;
	}

	return ImportConfig(ConfigPath, bInDryRun);
}

FString ChooseExportConfigPath(const FString& InAssetPath, const FString& InKind)
{
	return ForgeEditorUtils::ChooseExportConfigPath(
		ForgeState::FindConfigForAsset(InAssetPath, InKind),
		ForgePipeline::MakeDefaultConfigPath(InAssetPath, InKind));
}

FForgeReport ExportAsset(const FString& InAssetPath, FString* OutConfigPath)
{
	const FString AssetPath = ForgeState::NormalizeAssetPath(InAssetPath);

	FForgeReport Report;
	FForgeDocument Document;
	if (!ForgePipeline::Export(AssetPath, Document, Report))
	{
		Report.bSucceeded = false;
		LogReport(TEXT("导出"), Report);
		return Report;
	}

	const FString ExistingPath = ForgeState::FindConfigForAsset(AssetPath, Document.Kind);
	const FString ConfigPath = ForgeEditorUtils::ChooseExportConfigPath(
		ExistingPath, ForgePipeline::MakeDefaultConfigPath(AssetPath, Document.Kind));

	// 写回既有配置时保留它的 revision：那是人工维护的修订号，导出实现不知道当前值。
	if (!ExistingPath.IsEmpty())
	{
		FForgeDocument Existing;
		FForgeReport ExistingReport;
		if (ForgePipeline::LoadDocument(ExistingPath, Existing, ExistingReport))
		{
			Document.Revision = Existing.Revision;
		}
	}

	Report.SourcePath = ConfigPath;
	if (!ForgePipeline::SaveDocument(ConfigPath, Document, Report))
	{
		Report.bSucceeded = false;
		LogReport(TEXT("导出"), Report);
		return Report;
	}

	// 导出后配置与资产此刻一致：记清单，下次快速判定直接是「已同步」。
	Document.SourcePath = ConfigPath;
	if (Document.Asset.IsEmpty())
	{
		Document.Asset = AssetPath;
	}
	ForgeState::RecordImport(Document);

	Report.bSucceeded = true;
	if (OutConfigPath != nullptr)
	{
		*OutConfigPath = ConfigPath;
	}

	UE_LOG(LogAssetForge, Log, TEXT("[导出] %s → %s%s"),
		*AssetPath, *ConfigPath, ExistingPath.IsEmpty() ? TEXT("（新建配置）") : TEXT("（写回既有配置）"));
	return Report;
}

FForgeBatchResult ImportConfigs(const TArray<FString>& InConfigPaths, const bool bInDryRun, const bool bInShowProgress)
{
	FForgeBatchRunner Runner = FForgeBatchRunner::MakeImport(InConfigPaths, bInDryRun);
	RunBatch(Runner, bInDryRun ? LOCTEXT("PlanningMany", "预览计划…") : LOCTEXT("ImportingMany", "导入配置…"), bInShowProgress);
	return Runner.GetResult();
}

FForgeBatchResult ExportAssets(const TArray<FString>& InAssetPaths, const bool bInShowProgress)
{
	FForgeBatchRunner Runner = FForgeBatchRunner::MakeExport(InAssetPaths);
	RunBatch(Runner, LOCTEXT("ExportingMany", "导出到配置…"), bInShowProgress);
	return Runner.GetResult();
}

FString KindOfAsset(const FString& InAssetPath)
{
	const FString PackageName = ForgeState::NormalizeAssetPath(InAssetPath);
	TArray<FAssetData> Assets;
	FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(FName(*PackageName), Assets);
	for (const FAssetData& AssetData : Assets)
	{
		const FString Kind = ForgeKindRegistry::ClassifyAssetData(AssetData);
		if (!Kind.IsEmpty())
		{
			return Kind;
		}
	}
	return FString();
}

bool IsExportable(const FString& InAssetPath)
{
	return !KindOfAsset(InAssetPath).IsEmpty();
}

int32 DeepCheckEntries(TArray<FForgeEntry>& InOutEntries, const bool bInShowProgress)
{
	TArray<int32> Targets;
	for (int32 Index = 0; Index < InOutEntries.Num(); ++Index)
	{
		if (InOutEntries[Index].IsConfig() && ForgeState::NeedsDeepCheck(InOutEntries[Index].State))
		{
			Targets.Add(Index);
		}
	}

	if (Targets.Num() == 0)
	{
		return 0;
	}

	FScopedSlowTask Task(static_cast<float>(Targets.Num()), LOCTEXT("DeepChecking", "深度检查…"));
	if (ShouldShowDialog(bInShowProgress))
	{
		Task.MakeDialog(/*bShowCancelButton=*/true);
	}

	int32 Checked = 0;
	for (const int32 Index : Targets)
	{
		if (Task.ShouldCancel())
		{
			break;
		}

		FForgeEntry& Entry = InOutEntries[Index];
		Task.EnterProgressFrame(1.f, FText::FromString(Entry.GetDisplayName()));
		Entry.State = ForgeState::DeepCheck(Entry.ConfigPath, Entry.AssetPath);
		++Checked;
	}

	return Checked;
}

TArray<FString> CollectPendingConfigs()
{
	TArray<FString> Paths;
	for (const FForgeEntry& Entry : ScanConfigs())
	{
		if (Entry.CanImport() && ForgeState::NeedsImport(Entry.State))
		{
			Paths.Add(Entry.ConfigPath);
		}
	}
	return Paths;
}

} // namespace ForgeService

#undef LOCTEXT_NAMESPACE
