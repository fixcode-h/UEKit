// Copyright Epic Games, Inc. All Rights Reserved.

#include "Service/ForgeSamples.h"

#include "AssetForge.h"
#include "AssetForgeCompat.h"
#include "Core/ForgePipeline.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Settings/AssetForgeSettings.h"

namespace
{
	/** 示例里配套 GameTest 场景的子目录（`_` 开头：留在配置根里时 AssetForge 不把它当配置扫） */
	const TCHAR* const GameTestSubDir = TEXT("_GameTest");

	FString RecordPath()
	{
		// 记的是「本机装进去的是哪一版」，和同步清单一样放 Saved（不进版本管理）
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AssetForge/samples.json"));
	}

	FString HashFile(const FString& InPath)
	{
		const FMD5Hash Hash = FMD5Hash::HashFile(*InPath);
		return Hash.IsValid() ? LexToString(Hash) : FString();
	}

	TMap<FString, FString> LoadRecord()
	{
		TMap<FString, FString> Record;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *RecordPath()))
		{
			return Record;
		}
		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
		{
			return Record;
		}
		const TSharedPtr<FJsonObject>* Files = nullptr;
		if (Json->TryGetObjectField(TEXT("files"), Files) && Files != nullptr)
		{
			for (const auto& Pair : (*Files)->Values)
			{
				Record.Add(ASSETFORGE_JSON_KEY(Pair.Key), Pair.Value.IsValid() ? Pair.Value->AsString() : FString());
			}
		}
		return Record;
	}

	void SaveRecord(const TMap<FString, FString>& InRecord)
	{
		const TSharedRef<FJsonObject> Files = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : InRecord)
		{
			Files->SetStringField(Pair.Key, Pair.Value);
		}
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("files"), Files);

		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Root, Writer);
		FFileHelper::SaveStringToFile(Text, *RecordPath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	/** GameTest 的场景根：读它的项目设置（不依赖 GameTest 模块），留空按同一个文档根约定 */
	FString ResolveGameTestRoot()
	{
		FString Configured;
		if (GConfig != nullptr)
		{
			GConfig->GetString(TEXT("/Script/GameTest.GameTestSettings"), TEXT("ScenarioRootDir"), Configured, GGameIni);
		}
		Configured.TrimStartAndEndInline();
		if (Configured.IsEmpty())
		{
			return UAssetForgeSettings::ResolveDocsSubDirPath(TEXT("GameTest"));
		}
		FString Root = FPaths::IsRelative(Configured) ? FPaths::ProjectDir() / Configured : Configured;
		Root = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeDirectoryName(Root);
		return Root;
	}

	bool IsGameTestEnabled()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GameTest"));
		return Plugin.IsValid() && Plugin->IsEnabled();
	}

	/**
	 * 装一个文件。记录的键用「目标里的相对位置」（`AssetForge:Test/umg/...`），换了配置根也不会串。
	 * 返回是否写了文件。
	 */
	bool InstallFile(const FString& InSource, const FString& InTarget, const FString& InKey, TMap<FString, FString>& InOutRecord)
	{
		const FString SourceHash = HashFile(InSource);
		const FString* Installed = InOutRecord.Find(InKey);
		IFileManager& FileManager = IFileManager::Get();

		if (!FileManager.FileExists(*InTarget))
		{
			if (Installed != nullptr)
			{
				// 装过、后来被删了：用户不要它，不再装回来
				return false;
			}
		}
		else
		{
			const FString TargetHash = HashFile(InTarget);
			if (TargetHash == SourceHash)
			{
				InOutRecord.Add(InKey, SourceHash);
				return false;
			}
			// 没有记录（别人提交进版本管理的、更早手动拷的）或记录和现在的不一样（用户改过）：都不覆盖
			if (Installed == nullptr || *Installed != TargetHash)
			{
				UE_LOG(LogAssetForge, Log, TEXT("[示例] %s 被改过，不覆盖（要换成插件自带的版本就删掉它再重启编辑器）"), *InTarget);
				return false;
			}
		}

		if (FileManager.Copy(*InTarget, *InSource, /*Replace=*/true, /*EvenIfReadOnly=*/false) != COPY_OK)
		{
			UE_LOG(LogAssetForge, Warning, TEXT("[示例] 拷贝失败：%s → %s（目标只读或被占用，签出 / 解锁后重启编辑器再装）"), *InSource, *InTarget);
			return false;
		}
		InOutRecord.Add(InKey, SourceHash);
		return true;
	}
}

namespace ForgeSamples
{
	FString GetSourceDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AssetForge"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("docs/AssetForge/Test"));
	}

	int32 Install()
	{
		if (!GetDefault<UAssetForgeSettings>()->bInstallSamples)
		{
			return 0;
		}

		const FString SourceDir = GetSourceDir();
		if (SourceDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*SourceDir))
		{
			return 0;
		}

		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *SourceDir, TEXT("*"), /*Files=*/true, /*Directories=*/false);

		const FString ConfigTestDir = ForgePipeline::GetConfigRootDir() / TEXT("Test");
		const bool bGameTest = IsGameTestEnabled();
		const FString GameTestDir = bGameTest ? ResolveGameTestRoot() / TEXT("AssetForgeSample") : FString();
		const FString GameTestPrefix = FString(GameTestSubDir) + TEXT("/");

		TMap<FString, FString> Record = LoadRecord();
		int32 Written = 0;
		for (const FString& File : Files)
		{
			FString Relative = File;
			FPaths::MakePathRelativeTo(Relative, *(SourceDir + TEXT("/")));
			Relative.ReplaceInline(TEXT("\\"), TEXT("/"));

			Written += InstallFile(File, ConfigTestDir / Relative, TEXT("AssetForge:Test/") + Relative, Record) ? 1 : 0;

			if (bGameTest && Relative.StartsWith(GameTestPrefix))
			{
				const FString ScenarioRelative = Relative.RightChop(GameTestPrefix.Len());
				Written += InstallFile(File, GameTestDir / ScenarioRelative, TEXT("GameTest:AssetForgeSample/") + ScenarioRelative, Record) ? 1 : 0;
			}
		}

		SaveRecord(Record);
		if (Written > 0)
		{
			UE_LOG(LogAssetForge, Log, TEXT("[示例] 装了 %d 个示例文件：配置在 %s，在 Agent 面板 AssetForge 页导入后生成 /Game/AgentTest/AssetForge 下的测试界面"), Written, *ConfigTestDir);
		}
		return Written;
	}
}
