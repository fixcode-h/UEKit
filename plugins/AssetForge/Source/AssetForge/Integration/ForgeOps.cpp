// Copyright Epic Games, Inc. All Rights Reserved.

#include "Integration/ForgeOps.h"

#include "AgentPanelBridge.h"
#include "AssetForgeVersion.h"
#include "Core/ForgeDiffSnapshot.h"
#include "Core/ForgeDocSource.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgePropertySchema.h"
#include "Core/ForgeSections.h"
#include "Core/ForgeTypes.h"
#include "Integration/ForgeJobs.h"
#include "Registry/ForgeKindRegistry.h"
#include "Registry/ForgeKindSchema.h"
#include "Service/ForgeBatch.h"
#include "Service/ForgeService.h"
#include "Settings/AssetForgeSettings.h"
#include "State/ForgeState.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace ForgeOps
{
namespace
{
	// 返回长度的上限（和面板 ADR 0067 一致）：列表默认一页 200 条，结构内容最多 12000 字
	constexpr int32 DefaultPageSize = 200;
	constexpr int32 MaxPageSize = 1000;
	constexpr int32 MaxConfigTextChars = 12000;
	constexpr int32 MaxReportItems = 200;
	constexpr int32 MaxDiffChanges = 300;
	constexpr int32 MaxJobReports = 50;

	TArray<FString> RegisteredOps;

	FString Str(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name)
	{
		FString Value;
		if (Args.IsValid())
		{
			Args->TryGetStringField(Name, Value);
		}
		return Value.TrimStartAndEnd();
	}

	bool Bool(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name, const bool bDefault = false)
	{
		bool bValue = bDefault;
		if (Args.IsValid())
		{
			Args->TryGetBoolField(Name, bValue);
		}
		return bValue;
	}

	int32 Int(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name, const int32 Default)
	{
		int32 Value = Default;
		if (Args.IsValid())
		{
			Args->TryGetNumberField(Name, Value);
		}
		return Value;
	}

	TArray<FString> StrList(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Args.IsValid() && Args->TryGetArrayField(Name, Values) && Values != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Item;
				if (Value.IsValid() && Value->TryGetString(Item) && !Item.TrimStartAndEnd().IsEmpty())
				{
					Out.Add(Item.TrimStartAndEnd());
				}
			}
		}
		return Out;
	}

	/** 报告 → JSON，动作和诊断各最多 MaxReportItems 条（整张大 UMG 的计划可能上千条）。 */
	TSharedPtr<FJsonObject> CappedReport(const FForgeReport& InReport)
	{
		const TSharedPtr<FJsonObject> Json = ForgePipeline::ReportToJson(InReport);
		for (const TCHAR* Field : {TEXT("actions"), TEXT("diagnostics")})
		{
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (Json->TryGetArrayField(Field, Items) && Items->Num() > MaxReportItems)
			{
				TArray<TSharedPtr<FJsonValue>> Kept(Items->GetData(), MaxReportItems);
				const int32 Omitted = Items->Num() - MaxReportItems;
				Json->SetArrayField(Field, Kept);
				Json->SetNumberField(FString(Field) + TEXT("Omitted"), Omitted);
			}
		}
		return Json;
	}

	/** 配置参数：相对配置根或项目根的路径都行；找不到文件时说清楚在哪找的。 */
	bool ResolveExistingConfig(const TSharedPtr<FJsonObject>& Args, FString& OutPath, FString& Error)
	{
		const FString Config = Str(Args, TEXT("config"));
		if (Config.IsEmpty())
		{
			Error = TEXT("缺少 config：配置文件路径（相对配置根，例如 umg/UI/WBP_HUD.json）");
			return false;
		}
		OutPath = ForgePipeline::ResolveConfigPath(Config);
		if (!FPaths::FileExists(OutPath))
		{
			Error = FString::Printf(TEXT("找不到配置 %s（按项目根和配置根 %s 都找过）"), *Config, *ForgePipeline::GetConfigRootDir());
			return false;
		}
		return true;
	}

	/** 读当前配置：有 configText 用它（网页编辑器里还没存盘的内容），否则读文件。 */
	bool LoadCurrent(const TSharedPtr<FJsonObject>& Args, const FString& InPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
	{
		const FString Text = Str(Args, TEXT("configText"));
		return Text.IsEmpty()
			? ForgePipeline::LoadDocument(InPath, OutDocument, OutReport)
			: ForgePipeline::LoadDocumentFromText(Text, InPath, OutDocument, OutReport);
	}

	TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& S : In)
		{
			Out.Add(MakeShared<FJsonValueString>(S));
		}
		return Out;
	}

	void Add(const TCHAR* Name, FAgentPanelOp Op)
	{
		if (FAgentPanelBridge::Get().AddExtensionOp(Name, MoveTemp(Op)))
		{
			RegisteredOps.Add(Name);
		}
	}

	// ---- forge.health：装没装、版本、配置根、有没有作业在跑
	bool Health(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		const UAssetForgeSettings* Settings = GetDefault<UAssetForgeSettings>();
		Out->SetStringField(TEXT("version"), ASSETFORGE_VERSION);
		Out->SetStringField(TEXT("configRoot"), ForgePipeline::GetConfigRootDir());
		Out->SetBoolField(TEXT("configRootExists"), FPaths::DirectoryExists(ForgePipeline::GetConfigRootDir()));
		Out->SetArrayField(TEXT("scanRoots"), StringValues(Settings->UnmanagedScanRoots));
		Out->SetNumberField(TEXT("kinds"), ForgeKindRegistry::GetAll().Num());
		Out->SetBoolField(TEXT("jobRunning"), FForgeJobs::Get().IsRunning());
		return true;
	}

	// ---- forge.kinds：支持的资产类型；给 kind 回这种配置的 JSON Schema，给 class 回那个类在 props 里能写的属性
	// （Agent 写配置前先取，少一轮「写错字段被演练拒掉」）。不给参数时只回列表，17 份 schema 一起回会超过返回上限。
	bool Kinds(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		const FString KindArg = Str(Args, TEXT("kind"));
		const FString ClassArg = Str(Args, TEXT("class"));
		if (!KindArg.IsEmpty())
		{
			const TSharedPtr<FJsonObject> Schema = ForgeKindSchema::Build(KindArg);
			if (!Schema.IsValid())
			{
				Error = FString::Printf(TEXT("不认识的 kind：%s（可用的：%s）"), *KindArg,
					*FString::Join(ForgeKindRegistry::GetSupportedKinds(), TEXT("、")));
				return false;
			}
			Out->SetStringField(TEXT("kind"), KindArg);
			Out->SetObjectField(TEXT("schema"), Schema);
		}
		if (!ClassArg.IsEmpty())
		{
			ForgePropertySchema::FQuery Query;
			Query.Type = ClassArg;
			Query.Filter = Str(Args, TEXT("filter"));
			Query.Offset = Int(Args, TEXT("offset"), 0);
			Query.Limit = Int(Args, TEXT("limit"), Query.Limit);
			const TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
			if (!ForgePropertySchema::Describe(Query, Props, Error))
			{
				return false;
			}
			Out->SetObjectField(TEXT("props"), Props);
		}
		if (!KindArg.IsEmpty() || !ClassArg.IsEmpty())
		{
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const ForgeKindRegistry::FForgeKindInfo& Info : ForgeKindRegistry::GetAll())
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("kind"), Info.Kind);
			Item->SetStringField(TEXT("displayName"), Info.DisplayName);
			Item->SetStringField(TEXT("dir"), Info.ConfigDirName);
			if (UClass* Class = Info.GetAssetClass != nullptr ? Info.GetAssetClass() : nullptr)
			{
				Item->SetStringField(TEXT("assetClass"), Class->GetPathName());
			}
			Item->SetBoolField(TEXT("propertyDiff"), Info.DiffSupport.bSupported);
			if (!Info.DiffSupport.bSupported && !Info.DiffSupport.Reason.IsEmpty())
			{
				Item->SetStringField(TEXT("propertyDiffReason"), Info.DiffSupport.Reason);
			}
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		Out->SetArrayField(TEXT("kinds"), Items);
		return true;
	}

	// ---- forge.entries：配置和资产的同步状态，按 kind / 状态 / 关键字筛，分页
	bool Entries(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		TArray<FForgeEntry> All = ForgeService::ScanAll(Bool(Args, TEXT("unmanaged"), true));
		if (Bool(Args, TEXT("deep")))
		{
			// 深度检查真跑一次演练，配置多时很慢：只在用户点「深度检查」时做
			ForgeService::DeepCheckEntries(All, /*bInShowProgress=*/false);
		}

		const FString Kind = Str(Args, TEXT("kind"));
		const FString State = Str(Args, TEXT("state"));
		const FString Query = Str(Args, TEXT("query"));
		const int32 Offset = FMath::Max(0, Int(Args, TEXT("offset"), 0));
		const int32 Limit = FMath::Clamp(Int(Args, TEXT("limit"), DefaultPageSize), 1, MaxPageSize);

		TMap<FString, int32> StateCounts;
		TMap<FString, int32> KindCounts;
		TArray<const FForgeEntry*> Matched;
		for (const FForgeEntry& Entry : All)
		{
			const FString StateId = ForgeState::ToId(Entry.State);
			StateCounts.FindOrAdd(StateId)++;
			KindCounts.FindOrAdd(Entry.Kind.IsEmpty() ? TEXT("foreign") : Entry.Kind)++;
			if (!Kind.IsEmpty() && Entry.Kind != Kind)
			{
				continue;
			}
			if (!State.IsEmpty() && !(State == TEXT("needsImport") ? ForgeState::NeedsImport(Entry.State) : StateId == State))
			{
				continue;
			}
			if (!Query.IsEmpty() && !Entry.Key.Contains(Query) && !Entry.AssetPath.Contains(Query))
			{
				continue;
			}
			Matched.Add(&Entry);
		}

		TArray<TSharedPtr<FJsonValue>> Items;
		for (int32 Index = Offset; Index < Matched.Num() && Items.Num() < Limit; ++Index)
		{
			const TSharedPtr<FJsonObject> Json = Matched[Index]->ToJson();
			Json->SetStringField(TEXT("stateLabel"), ForgeState::GetLabel(Matched[Index]->State).ToString());
			Items.Add(MakeShared<FJsonValueObject>(Json));
		}

		const auto CountsJson = [](const TMap<FString, int32>& In)
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			for (const TPair<FString, int32>& Pair : In)
			{
				Json->SetNumberField(Pair.Key, Pair.Value);
			}
			return Json;
		};
		Out->SetArrayField(TEXT("entries"), Items);
		Out->SetNumberField(TEXT("total"), Matched.Num());
		Out->SetNumberField(TEXT("offset"), Offset);
		Out->SetObjectField(TEXT("states"), CountsJson(StateCounts));
		Out->SetObjectField(TEXT("kinds"), CountsJson(KindCounts));
		return true;
	}

	// ---- forge.describe：一份配置（或一个资产）的条目、状态说明和配置原文
	bool Describe(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		FString ConfigPath;
		const FString Asset = Str(Args, TEXT("asset"));
		if (!Asset.IsEmpty() && Str(Args, TEXT("config")).IsEmpty())
		{
			const FString Kind = ForgeService::KindOfAsset(Asset);
			ConfigPath = ForgeState::FindConfigForAsset(ForgeState::NormalizeAssetPath(Asset), Kind);
			if (ConfigPath.IsEmpty())
			{
				// 还没纳管：网页上显示「导出到配置」
				Out->SetBoolField(TEXT("managed"), false);
				Out->SetBoolField(TEXT("exportable"), !Kind.IsEmpty());
				if (!Kind.IsEmpty())
				{
					Out->SetStringField(TEXT("kind"), Kind);
				}
				Out->SetStringField(TEXT("asset"), ForgeState::NormalizeAssetPath(Asset));
				return true;
			}
		}
		else if (!ResolveExistingConfig(Args, ConfigPath, Error))
		{
			return false;
		}

		const FForgeEntry Entry = ForgeService::DescribeConfig(ConfigPath);
		Out->SetBoolField(TEXT("managed"), true);
		Out->SetObjectField(TEXT("entry"), Entry.ToJson());
		Out->SetStringField(TEXT("stateLabel"), ForgeState::GetLabel(Entry.State).ToString());
		Out->SetStringField(TEXT("stateDescription"), ForgeState::GetDescription(Entry.State).ToString());

		// 段：配置长时 Agent 只取一段（section），面板按段导航。include 对这里透明：读的是展开后的文档。
		const FString Section = Str(Args, TEXT("section"));
		ForgeDocSource::FExpanded Expanded;
		TArray<FForgeDiagnostic> ExpandDiagnostics;
		const bool bExpanded = ForgeDocSource::LoadFile(ConfigPath, Expanded, ExpandDiagnostics);
		if (bExpanded)
		{
			const TArray<FString> BodySections = ForgeKindSchema::GetBodySections(Entry.Kind);
			TArray<TSharedPtr<FJsonValue>> SectionItems;
			for (const FString& Name : ForgeSections::ListPresent(Expanded.Json, BodySections))
			{
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Name);
				if (const FString* Fragment = Expanded.Origins.Find(Name))
				{
					// 整段放在片段文件里（相对主文件）
					Item->SetStringField(TEXT("include"), *Fragment);
				}
				SectionItems.Add(MakeShared<FJsonValueObject>(Item));
			}
			Out->SetArrayField(TEXT("sections"), SectionItems);

			if (!Section.IsEmpty())
			{
				const TSharedPtr<FJsonObject> Part = ForgeSections::Extract(Expanded.Json, Section, BodySections);
				if (!Part.IsValid())
				{
					Error = FString::Printf(TEXT("这份配置没有「%s」段（有的段：%s）"), *Section,
						*FString::Join(ForgeSections::ListPresent(Expanded.Json, BodySections), TEXT("、")));
					return false;
				}
				const FString SectionText = ForgeJsonText::ToPrettyString(Part);
				const bool bSectionTruncated = SectionText.Len() > MaxConfigTextChars;
				Out->SetStringField(TEXT("section"), Section);
				Out->SetStringField(TEXT("sectionText"), bSectionTruncated ? SectionText.Left(MaxConfigTextChars) : SectionText);
				if (bSectionTruncated)
				{
					Out->SetBoolField(TEXT("sectionTextTruncated"), true);
					Out->SetNumberField(TEXT("sectionTextLength"), SectionText.Len());
				}
				// 只要一段时不再带整份原文：省返回长度，这正是按段读的目的。
				return true;
			}
		}
		else if (!Section.IsEmpty())
		{
			Error = FString::Printf(TEXT("配置读不了，取不了段：%s"),
				ExpandDiagnostics.Num() > 0 ? *ExpandDiagnostics[0].Message : TEXT("未知原因"));
			return false;
		}

		FString Text;
		if (FFileHelper::LoadFileToString(Text, *ConfigPath))
		{
			const bool bTruncated = Text.Len() > MaxConfigTextChars;
			Out->SetStringField(TEXT("configText"), bTruncated ? Text.Left(MaxConfigTextChars) : Text);
			if (bTruncated)
			{
				// 截断的原文不能拿去编辑：网页上提示用外部编辑器打开文件
				Out->SetBoolField(TEXT("configTextTruncated"), true);
				Out->SetNumberField(TEXT("configTextLength"), Text.Len());
			}
		}
		return true;
	}

	// ---- forge.plan：演练（不改资产），configText 可以是还没存盘的内容
	bool Plan(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		FString ConfigPath;
		if (!ResolveExistingConfig(Args, ConfigPath, Error))
		{
			return false;
		}
		const FString Text = Str(Args, TEXT("configText"));
		EForgeSyncState State = EForgeSyncState::Unknown;
		const FForgeReport Report = Text.IsEmpty() ? ForgeService::Plan(ConfigPath, &State) : ForgeService::PlanText(ConfigPath, Text);
		Out->SetObjectField(TEXT("report"), CappedReport(Report));
		if (Text.IsEmpty())
		{
			Out->SetStringField(TEXT("state"), ForgeState::ToId(State));
		}
		return true;
	}

	// ---- forge.diff：导入后属性会怎么变（JSON 并排），baselineText 给了就和那份配置比
	bool Diff(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		FString ConfigPath;
		if (!ResolveExistingConfig(Args, ConfigPath, Error))
		{
			return false;
		}
		FForgeReport Report;
		FForgeDocument Current;
		if (!LoadCurrent(Args, ConfigPath, Current, Report))
		{
			Error = FString::Printf(TEXT("配置解析失败：%s"), *ForgePipeline::ReportToText(Report));
			return false;
		}

		const FString BaselineText = Str(Args, TEXT("baselineText"));
		FForgeDocument Baseline;
		if (!BaselineText.IsEmpty())
		{
			FForgeReport BaselineReport;
			if (!ForgePipeline::LoadDocumentFromText(BaselineText, ConfigPath, Baseline, BaselineReport))
			{
				Error = FString::Printf(TEXT("基线配置（baselineText）解析失败：%s"), *ForgePipeline::ReportToText(BaselineReport));
				return false;
			}
		}

		const TSharedRef<FJsonObject> Diff = ForgeDiffSnapshot::DiffProperties(
			ForgeState::NormalizeAssetPath(Current.Asset), Current, BaselineText.IsEmpty() ? nullptr : &Baseline, MaxDiffChanges);
		Out->SetStringField(TEXT("asset"), Current.Asset);
		Out->SetObjectField(TEXT("diff"), Diff);
		return true;
	}

	// ---- forge.import：起导入作业（configs 列表，或 pending=true 导入所有要导入的）
	bool Import(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		TArray<FString> Configs;
		for (const FString& Config : StrList(Args, TEXT("configs")))
		{
			Configs.Add(ForgePipeline::ResolveConfigPath(Config));
		}
		if (Bool(Args, TEXT("pending")))
		{
			for (const FString& Config : ForgeService::CollectPendingConfigs())
			{
				Configs.AddUnique(Config);
			}
		}
		if (Configs.Num() == 0)
		{
			Error = Bool(Args, TEXT("pending"))
				? TEXT("没有要导入的配置：所有配置都和资产一致")
				: TEXT("缺少 configs（配置路径列表），或传 pending: true 导入所有待导入的");
			return false;
		}

		const bool bDryRun = Bool(Args, TEXT("dryRun"));
		const FString Title = FString::Printf(TEXT("%s %d 份配置"), bDryRun ? TEXT("演练") : TEXT("导入"), Configs.Num());
		const FString Id = FForgeJobs::Get().Start(FForgeBatchRunner::MakeImport(Configs, bDryRun), Title, Error);
		if (Id.IsEmpty())
		{
			return false;
		}
		Out->SetStringField(TEXT("job"), Id);
		Out->SetNumberField(TEXT("total"), Configs.Num());
		return true;
	}

	// ---- forge.export：起导出作业（资产 → 配置，纳管也是它）
	bool Export(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		const TArray<FString> Assets = StrList(Args, TEXT("assets"));
		if (Assets.Num() == 0)
		{
			Error = TEXT("缺少 assets：资产路径列表（例如 /Game/UI/WBP_HUD）");
			return false;
		}
		const FString Title = FString::Printf(TEXT("导出 %d 个资产"), Assets.Num());
		const FString Id = FForgeJobs::Get().Start(FForgeBatchRunner::MakeExport(Assets), Title, Error);
		if (Id.IsEmpty())
		{
			return false;
		}
		Out->SetStringField(TEXT("job"), Id);
		Out->SetNumberField(TEXT("total"), Assets.Num());
		return true;
	}

	bool Job(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		const FString Id = Str(Args, TEXT("id"));
		const TSharedPtr<FJsonObject> Json = FForgeJobs::Get().Describe(Id, FMath::Clamp(Int(Args, TEXT("reports"), MaxJobReports), 0, MaxJobReports));
		if (!Json.IsValid())
		{
			Error = Id.IsEmpty() ? TEXT("还没有作业") : FString::Printf(TEXT("没有作业 %s（编辑器重启后作业记录会清空）"), *Id);
			return false;
		}
		Out->SetObjectField(TEXT("job"), Json);
		return true;
	}

	bool Stop(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		const FString Id = Str(Args, TEXT("id"));
		if (!FForgeJobs::Get().Stop(Id))
		{
			Error = FString::Printf(TEXT("作业 %s 不存在或已经结束"), *Id);
			return false;
		}
		Out->SetStringField(TEXT("job"), Id);
		return true;
	}
}

void Register()
{
	Add(TEXT("forge.health"), &Health);
	Add(TEXT("forge.kinds"), &Kinds);
	Add(TEXT("forge.entries"), &Entries);
	Add(TEXT("forge.describe"), &Describe);
	Add(TEXT("forge.plan"), &Plan);
	Add(TEXT("forge.diff"), &Diff);
	Add(TEXT("forge.import"), &Import);
	Add(TEXT("forge.export"), &Export);
	Add(TEXT("forge.job"), &Job);
	Add(TEXT("forge.stop"), &Stop);
}

void Unregister()
{
	for (const FString& Name : RegisteredOps)
	{
		FAgentPanelBridge::Get().RemoveOp(Name);
	}
	RegisteredOps.Reset();
	FForgeJobs::Get().Shutdown();
}
}
