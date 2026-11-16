// Copyright Epic Games, Inc. All Rights Reserved.

#include "State/ForgeState.h"
#include "AssetForge.h"

#include "Core/ForgeHash.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgeManifest.h"
#include "Core/ForgePipeline.h"

#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "ForgeState"

namespace ForgeState
{

namespace
{
	/** 清单的内存镜像。按「清单路径 + 文件修改时间」失效：别的进程（命令行）写过它也能察觉。 */
	struct FManifestCache
	{
		FString LoadedPath;
		FDateTime LoadedStamp = FDateTime::MinValue();
		bool bLoaded = false;
		TMap<FString, ForgeManifest::FManifestEntry> Entries;
		/** 资产路径（归一化）→ 清单键。 */
		TMap<FString, FString> AssetToKey;
	};

	FManifestCache& Cache()
	{
		static FManifestCache Instance;
		return Instance;
	}

	FDateTime FileStamp(const FString& InPath)
	{
		return FPaths::FileExists(InPath) ? IFileManager::Get().GetTimeStamp(*InPath) : FDateTime::MinValue();
	}

	void RebuildIndex(FManifestCache& InCache)
	{
		InCache.AssetToKey.Reset();
		for (const TPair<FString, ForgeManifest::FManifestEntry>& Pair : InCache.Entries)
		{
			if (!Pair.Value.AssetPath.IsEmpty())
			{
				InCache.AssetToKey.Add(NormalizeAssetPath(Pair.Value.AssetPath), Pair.Key);
			}
		}
	}

	FManifestCache& EnsureLoaded()
	{
		FManifestCache& State = Cache();
		const FString Path = GetManifestPath();
		const FDateTime Stamp = FileStamp(Path);

		if (State.bLoaded && State.LoadedPath == Path && State.LoadedStamp == Stamp)
		{
			return State;
		}

		FString Error;
		if (!ForgeManifest::Load(Path, State.Entries, Error))
		{
			UE_LOG(LogAssetForge, Warning, TEXT("读取清单失败（按空清单处理）：%s"), *Error);
			State.Entries.Reset();
		}

		State.LoadedPath = Path;
		State.LoadedStamp = Stamp;
		State.bLoaded = true;
		RebuildIndex(State);
		return State;
	}

	bool TryComputeContentHash(const FString& InConfigPath, FString& OutHash)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *InConfigPath))
		{
			return false;
		}

		// 按剥离注释后的文本算：只改注释不该触发重新导入。
		OutHash = ForgeHash::HashText(ForgeJsonText::StripComments(Text));
		return true;
	}

	/** 资产文件当前的修改时间（ISO8601）；只做相等判定，时区与精度无关。 */
	FString AssetFileMTime(const FString& InAssetPath)
	{
		if (InAssetPath.IsEmpty())
		{
			return FString();
		}

		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				NormalizeAssetPath(InAssetPath), Filename, FPackageName::GetAssetPackageExtension()))
		{
			return FString();
		}

		if (!FPaths::FileExists(Filename))
		{
			// 地图是 .umap。
			Filename = FPaths::ChangeExtension(Filename, FPackageName::GetMapPackageExtension());
			if (!FPaths::FileExists(Filename))
			{
				return FString();
			}
		}

		return IFileManager::Get().GetTimeStamp(*Filename).ToIso8601();
	}
}

bool NeedsImport(const EForgeSyncState InState)
{
	switch (InState)
	{
	case EForgeSyncState::NeverImported:
	case EForgeSyncState::ConfigChanged:
	case EForgeSyncState::AssetChanged:
	case EForgeSyncState::AssetMissing:
	case EForgeSyncState::Pending:
		return true;
	default:
		return false;
	}
}

bool NeedsDeepCheck(const EForgeSyncState InState)
{
	return InState == EForgeSyncState::ConfigChanged || InState == EForgeSyncState::AssetChanged;
}

bool IsManagedConfig(const EForgeSyncState InState)
{
	switch (InState)
	{
	case EForgeSyncState::Unmanaged:
	case EForgeSyncState::Foreign:
		return false;
	default:
		return true;
	}
}

FText GetLabel(const EForgeSyncState InState)
{
	switch (InState)
	{
	case EForgeSyncState::Synced:        return LOCTEXT("Synced", "已同步");
	case EForgeSyncState::NeverImported: return LOCTEXT("NeverImported", "未导入");
	case EForgeSyncState::ConfigChanged: return LOCTEXT("ConfigChanged", "配置已改");
	case EForgeSyncState::AssetChanged:  return LOCTEXT("AssetChanged", "资产已改");
	case EForgeSyncState::AssetMissing:  return LOCTEXT("AssetMissing", "资产缺失");
	case EForgeSyncState::Pending:       return LOCTEXT("Pending", "待导入");
	case EForgeSyncState::ParseError:    return LOCTEXT("ParseError", "解析失败");
	case EForgeSyncState::Foreign:       return LOCTEXT("Foreign", "外部配置");
	case EForgeSyncState::Unmanaged:     return LOCTEXT("Unmanaged", "未纳管");
	case EForgeSyncState::NoAsset:       return LOCTEXT("NoAsset", "无目标");
	default:                               return LOCTEXT("Unknown", "—");
	}
}

FText GetDescription(const EForgeSyncState InState)
{
	switch (InState)
	{
	case EForgeSyncState::Synced:
		return LOCTEXT("SyncedDesc", "配置与资产一致。");
	case EForgeSyncState::NeverImported:
		return LOCTEXT("NeverImportedDesc", "这份配置还没有导入过：导入会创建或覆盖资产。");
	case EForgeSyncState::ConfigChanged:
		return LOCTEXT("ConfigChangedDesc", "配置自上次导入后改过。可能只是格式变化——预览计划可确认是否真的会改资产。");
	case EForgeSyncState::AssetChanged:
		return LOCTEXT("AssetChangedDesc", "资产在编辑器里被改过。要保留改动请「导出到配置」，要以配置为准请「导入」。");
	case EForgeSyncState::AssetMissing:
		return LOCTEXT("AssetMissingDesc", "配置指向的资产不存在：导入会重新创建它。");
	case EForgeSyncState::Pending:
		return LOCTEXT("PendingDesc", "已确认：导入会改变资产。");
	case EForgeSyncState::ParseError:
		return LOCTEXT("ParseErrorDesc", "配置解析失败，见诊断。");
	case EForgeSyncState::Foreign:
		return LOCTEXT("ForeignDesc", "别的工具的配置（kind 不在本插件登记表里），本工具不处理。");
	case EForgeSyncState::Unmanaged:
		return LOCTEXT("UnmanagedDesc", "有资产、没有配置。「导出到配置」即可纳管。");
	case EForgeSyncState::NoAsset:
		return LOCTEXT("NoAssetDesc", "配置没有写目标资产（asset 字段），无从判断同步状态。");
	default:
		return FText::GetEmpty();
	}
}

const TCHAR* ToId(const EForgeSyncState InState)
{
	switch (InState)
	{
	case EForgeSyncState::Synced:        return TEXT("synced");
	case EForgeSyncState::NeverImported: return TEXT("never-imported");
	case EForgeSyncState::ConfigChanged: return TEXT("config-changed");
	case EForgeSyncState::AssetChanged:  return TEXT("asset-changed");
	case EForgeSyncState::AssetMissing:  return TEXT("asset-missing");
	case EForgeSyncState::Pending:       return TEXT("pending");
	case EForgeSyncState::ParseError:    return TEXT("parse-error");
	case EForgeSyncState::Foreign:       return TEXT("foreign");
	case EForgeSyncState::Unmanaged:     return TEXT("unmanaged");
	case EForgeSyncState::NoAsset:       return TEXT("no-asset");
	default:                               return TEXT("unknown");
	}
}

EForgeSyncState FromId(const FString& InId)
{
	for (uint8 Index = 0; Index < static_cast<uint8>(EForgeSyncState::Count); ++Index)
	{
		const EForgeSyncState State = static_cast<EForgeSyncState>(Index);
		if (InId.Equals(ToId(State), ESearchCase::IgnoreCase))
		{
			return State;
		}
	}
	return EForgeSyncState::Unknown;
}

FString GetManifestPath()
{
	// 清单记的是本机资产文件的修改时间，放进版本管理会让每个人互相覆盖：放 Saved/（不进版本管理）。
	// 原项目放在配置根下的 _forge-state.json：第一次读时挪过来（一次性迁移），之后只认新位置。
	const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetForge/state.json"));
	const FString Legacy = FPaths::Combine(ForgePipeline::GetConfigRootDir(), TEXT("_forge-state.json"));
	if (!FPaths::FileExists(Path) && FPaths::FileExists(Legacy))
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
		if (IFileManager::Get().Copy(*Path, *Legacy) == COPY_OK)
		{
			UE_LOG(LogAssetForge, Log, TEXT("同步清单从 %s 挪到 %s（旧文件留着，确认没问题后可以删掉）"), *Legacy, *Path);
		}
	}
	return FPaths::ConvertRelativePathToFull(Path);
}

FString MakeManifestKey(const FString& InConfigPath)
{
	FString Relative = FPaths::ConvertRelativePathToFull(InConfigPath);
	Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
	FPaths::MakePathRelativeTo(Relative, *(ForgePipeline::GetConfigRootDir() + TEXT("/")));
	return Relative;
}

void InvalidateCache()
{
	Cache().bLoaded = false;
}

void RecordImport(const FForgeDocument& InDocument)
{
	if (InDocument.SourcePath.IsEmpty())
	{
		return;
	}

	FString ContentHash;
	if (!TryComputeContentHash(InDocument.SourcePath, ContentHash))
	{
		return;
	}

	FManifestCache& State = EnsureLoaded();

	ForgeManifest::FManifestEntry Entry;
	Entry.ConfigPath = MakeManifestKey(InDocument.SourcePath);
	Entry.ContentHash = ContentHash;
	Entry.AssetPath = InDocument.Asset;
	Entry.ImportedAtUtc = FDateTime::UtcNow().ToIso8601();
	Entry.AssetMTimeUtc = AssetFileMTime(InDocument.Asset);
	State.Entries.Add(Entry.ConfigPath, MoveTemp(Entry));
	RebuildIndex(State);

	FString Error;
	if (!ForgeManifest::Save(State.LoadedPath, State.Entries, Error))
	{
		UE_LOG(LogAssetForge, Warning, TEXT("写清单失败：%s"), *Error);
	}
	State.LoadedStamp = FileStamp(State.LoadedPath);
}

FString FindConfigForAsset(const FString& InAssetPath, const FString& InKind)
{
	if (InAssetPath.IsEmpty())
	{
		return FString();
	}

	const FManifestCache& State = EnsureLoaded();
	if (const FString* Key = State.AssetToKey.Find(NormalizeAssetPath(InAssetPath)))
	{
		const FString Candidate = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(ForgePipeline::GetConfigRootDir(), *Key));
		if (FPaths::FileExists(Candidate))
		{
			return Candidate;
		}
	}

	if (InKind.IsEmpty())
	{
		return FString();
	}

	const FString DefaultPath = ForgePipeline::MakeDefaultConfigPath(InAssetPath, InKind);
	return FPaths::FileExists(DefaultPath) ? DefaultPath : FString();
}

TSet<FString> GetRecordedAssetPaths()
{
	TSet<FString> Paths;
	for (const TPair<FString, FString>& Pair : EnsureLoaded().AssetToKey)
	{
		Paths.Add(Pair.Key);
	}
	return Paths;
}

EForgeSyncState QuickState(const FString& InConfigPath, const FString& InAssetPath)
{
	if (InAssetPath.IsEmpty())
	{
		return EForgeSyncState::NoAsset;
	}

	// 先看本机导入过没有：新配置（新建的、插件自带的示例、同事提交的）资产本来就不存在，
	// 那是「还没导入」，不是「资产不存在」——后者只给导入过、资产后来没了的情况，否则新配置一出来就被标成出错
	const FManifestCache& State = EnsureLoaded();
	const ForgeManifest::FManifestEntry* Entry = State.Entries.Find(MakeManifestKey(InConfigPath));

	const FString AssetMTime = AssetFileMTime(InAssetPath);
	if (AssetMTime.IsEmpty() && !FPackageName::DoesPackageExist(NormalizeAssetPath(InAssetPath)))
	{
		return Entry == nullptr ? EForgeSyncState::NeverImported : EForgeSyncState::AssetMissing;
	}

	if (Entry == nullptr)
	{
		return EForgeSyncState::NeverImported;
	}

	FString ContentHash;
	if (!TryComputeContentHash(InConfigPath, ContentHash) || Entry->ContentHash != ContentHash)
	{
		return EForgeSyncState::ConfigChanged;
	}

	// 老条目没记修改时间：没有依据，交给深度检查。
	if (Entry->AssetMTimeUtc.IsEmpty() || Entry->AssetMTimeUtc != AssetMTime)
	{
		return EForgeSyncState::AssetChanged;
	}

	return EForgeSyncState::Synced;
}

EForgeSyncState DeepCheck(const FString& InConfigPath, const FString& InAssetPath, FForgeReport* OutReport)
{
	FForgeReport LocalReport;
	FForgeReport& Report = OutReport != nullptr ? *OutReport : LocalReport;
	Report = FForgeReport();
	Report.bDryRun = true;

	FForgeDocument Document;
	if (!ForgePipeline::LoadDocument(InConfigPath, Document, Report))
	{
		return EForgeSyncState::ParseError;
	}

	Report.AssetPath = Document.Asset;
	const bool bOk = ForgePipeline::Import(Document, /*bDryRun=*/true, Report);
	Report.bSucceeded = bOk && !Report.HasErrors();

	const FString AssetPath = InAssetPath.IsEmpty() ? Document.Asset : InAssetPath;
	if (AssetPath.IsEmpty())
	{
		return EForgeSyncState::NoAsset;
	}

	if (!Report.bSucceeded)
	{
		// 预演失败：保留快速判定的结论（解析成功但导入会失败，诊断里有原因）。
		return QuickState(InConfigPath, AssetPath);
	}

	if (Report.Created == 0 && Report.Updated == 0 && Report.Deleted == 0)
	{
		if (!FPackageName::DoesPackageExist(NormalizeAssetPath(AssetPath)))
		{
			return EForgeSyncState::AssetMissing;
		}

		// 语义一致：刷新清单，下次快速判定直接命中「已同步」。
		RecordImport(Document);
		return EForgeSyncState::Synced;
	}

	return EForgeSyncState::Pending;
}

FString MakeChangeStamp(const FString& InConfigPath, const FString& InAssetPath)
{
	const FString ConfigStamp = (!InConfigPath.IsEmpty() && FPaths::FileExists(InConfigPath))
		? IFileManager::Get().GetTimeStamp(*InConfigPath).ToIso8601()
		: FString();
	return ConfigStamp + TEXT("|") + AssetFileMTime(InAssetPath);
}

FString ToObjectPath(const FString& InAssetPath)
{
	const FString PackageName = NormalizeAssetPath(InAssetPath);
	if (PackageName.IsEmpty())
	{
		return PackageName;
	}

	// 原本就带对象名（`/Game/A/B.B`、`/Game/A/B.B_C`）时保留原样。
	const FString Trimmed = InAssetPath.TrimStartAndEnd();
	if (!Trimmed.Contains(TEXT("'")) && Trimmed.Contains(TEXT(".")))
	{
		return Trimmed;
	}

	return PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
}

FString NormalizeAssetPath(const FString& InAssetPath)
{
	FString Path = InAssetPath.TrimStartAndEnd();

	// `/Script/Engine.Blueprint'/Game/A/B.B'` 这类导出文本先剥成对象路径。
	if (Path.Contains(TEXT("'")))
	{
		Path = FPackageName::ExportTextPathToObjectPath(Path);
	}

	// `/Game/A/B.B` → `/Game/A/B`；`_C` 生成类同理。
	return FPackageName::ObjectPathToPackageName(Path);
}

} // namespace ForgeState

#undef LOCTEXT_NAMESPACE
