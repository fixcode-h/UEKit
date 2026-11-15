// Copyright Epic Games, Inc. All Rights Reserved.

#include "Integration/AssetForgeCommandlet.h"
#include "AssetForge.h"

#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Registry/ForgeKindRegistry.h"
#include "Service/ForgeService.h"
#include "State/ForgeState.h"

#include "Dom/JsonObject.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"

namespace
{
	TArray<FString> SplitCommandletList(const FString& InValue)
	{
		TArray<FString> Out;
		InValue.ParseIntoArray(Out, TEXT(","), /*InCullEmpty=*/true);
		for (FString& Item : Out)
		{
			Item.TrimStartAndEndInline();
		}
		return Out;
	}

	void EmitJson(const TSharedPtr<FJsonObject>& InJson)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(InJson.ToSharedRef(), Writer);
		UE_LOG(LogAssetForge, Display, TEXT("ASSETFORGE_JSON:%s"), *Text);
	}

	void LogBatch(const FForgeBatchResult& InResult, const FText& InVerb)
	{
		for (const FForgeReport& Report : InResult.Reports)
		{
			if (Report.bSucceeded)
			{
				UE_LOG(LogAssetForge, Display, TEXT("%s"), *ForgePipeline::ReportToText(Report));
			}
			else
			{
				UE_LOG(LogAssetForge, Error, TEXT("%s"), *ForgePipeline::ReportToText(Report));
			}
		}
		UE_LOG(LogAssetForge, Display, TEXT("%s"), *InResult.GetSummary(InVerb).ToString());
	}

	int32 RunStatus(const FString& InParams, const bool bInJson)
	{
		TArray<FForgeEntry> Entries = ForgeService::ScanAll(FParse::Param(*InParams, TEXT("unmanaged")));
		if (FParse::Param(*InParams, TEXT("deep")))
		{
			ForgeService::DeepCheckEntries(Entries, /*bInShowProgress=*/false);
		}

		if (bInJson)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FForgeEntry& Entry : Entries)
			{
				Items.Add(MakeShared<FJsonValueObject>(Entry.ToJson()));
			}
			const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetBoolField(TEXT("ok"), true);
			Json->SetStringField(TEXT("configRoot"), ForgePipeline::GetConfigRootDir());
			Json->SetArrayField(TEXT("entries"), Items);
			EmitJson(Json);
			return UAssetForgeCommandlet::ExitOk;
		}

		UE_LOG(LogAssetForge, Display, TEXT("配置根：%s"), *ForgePipeline::GetConfigRootDir());
		for (const FForgeEntry& Entry : Entries)
		{
			UE_LOG(LogAssetForge, Display, TEXT("  %-14s %-16s %s%s%s"),
				ForgeState::ToId(Entry.State),
				Entry.Kind.IsEmpty() ? *Entry.ForeignKind : *Entry.Kind,
				Entry.IsConfig() ? *Entry.RelativePath : *Entry.AssetPath,
				Entry.Message.IsEmpty() ? TEXT("") : TEXT("  — "),
				*Entry.Message);
		}
		UE_LOG(LogAssetForge, Display, TEXT("共 %d 条"), Entries.Num());
		return UAssetForgeCommandlet::ExitOk;
	}
}

UAssetForgeCommandlet::UAssetForgeCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UAssetForgeCommandlet::Main(const FString& Params)
{
	const bool bDryRun = FParse::Param(*Params, TEXT("dry-run"));
	const bool bJson = FParse::Param(*Params, TEXT("json"));

	FString ConfigArg;
	FString ExportArg;
	// `FParse::Value` 默认遇到逗号就截断；关掉以支持逗号分隔的多个值。
	FParse::Value(*Params, TEXT("config="), ConfigArg, /*bShouldStopOnSeparator=*/false);
	FParse::Value(*Params, TEXT("export="), ExportArg, /*bShouldStopOnSeparator=*/false);

	if (FParse::Param(*Params, TEXT("status")))
	{
		return RunStatus(Params, bJson);
	}

	if (!ExportArg.IsEmpty())
	{
		const FForgeBatchResult Result = ForgeService::ExportAssets(SplitCommandletList(ExportArg), /*bInShowProgress=*/false);
		LogBatch(Result, NSLOCTEXT("AssetForgeCommandlet", "Export", "导出"));
		if (bJson)
		{
			EmitJson(Result.ToJson());
		}
		return Result.HasFailures() ? ExitFailures : ExitOk;
	}

	TArray<FString> Configs;
	if (!ConfigArg.IsEmpty())
	{
		for (const FString& Config : SplitCommandletList(ConfigArg))
		{
			// commandlet 的工作目录是引擎目录，相对路径必须显式还原成项目内路径。
			Configs.Add(ForgePipeline::ResolveConfigPath(Config));
		}
	}
	else if (FParse::Param(*Params, TEXT("pending")))
	{
		Configs = ForgeService::CollectPendingConfigs();
	}
	else if (FParse::Param(*Params, TEXT("all")))
	{
		for (const FForgeEntry& Entry : ForgeService::ScanConfigs())
		{
			if (Entry.CanImport())
			{
				Configs.Add(Entry.ConfigPath);
			}
		}
	}
	else
	{
		UE_LOG(LogAssetForge, Error,
			TEXT("缺少参数。用法：-run=AssetForge -status [-deep] [-unmanaged] | -config=<文件>[,...] | -pending | -all | -export=<资产>[,...]  [-dry-run] [-json]"));
		return ExitUsage;
	}

	if (Configs.Num() == 0)
	{
		UE_LOG(LogAssetForge, Display, TEXT("没有需要处理的配置（配置根：%s）"), *ForgePipeline::GetConfigRootDir());
		if (bJson)
		{
			EmitJson(FForgeBatchResult().ToJson());
		}
		return ExitOk;
	}

	const FForgeBatchResult Result = ForgeService::ImportConfigs(Configs, bDryRun, /*bInShowProgress=*/false);
	LogBatch(Result, bDryRun
		? NSLOCTEXT("AssetForgeCommandlet", "Plan", "预览")
		: NSLOCTEXT("AssetForgeCommandlet", "Import", "导入"));
	if (bJson)
	{
		EmitJson(Result.ToJson());
	}
	return Result.HasFailures() ? ExitFailures : ExitOk;
}
