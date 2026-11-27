// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestRunConfig.h"

#include "Core/Flow/GameTestGuards.h"
#include "Core/GameTestRunConfigRef.h"
#include "Core/Flow/GameTestScriptParser.h"
#include "Core/GameTestJson.h"
#include "Core/Script/GameTestDiag.h"
#include "GameTest.h"
#include "GameTestCompat.h"
#include "GameTestSettings.h"
#include "Operate/GameTestMove.h"
#include "Operate/GameTestSkipIf.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/** 守护的 do 里 call 子脚本相对场景根：校验用的解析器要知道场景根。 */
	FString RunConfigScenarioRoot()
	{
		return FPaths::GetPath(GameTestRunConfig::GetPath());
	}

	/** 守护、脚本开关（1.9.0）。 */
	bool ParseScriptSection(const FJsonObject& InJson, FGameTestRunConfig& OutConfig, FString& OutError)
	{
		if (!GameTestRunConfigRef::Parse(InJson, OutConfig.ScriptOptions, OutError))
		{
			return false;
		}
		bool bFlag = false;
		if (InJson.HasField(TEXT("allowLuaInvoke")))
		{
			if (!InJson.TryGetBoolField(TEXT("allowLuaInvoke"), bFlag))
			{
				OutError = TEXT("allowLuaInvoke 要写 true / false");
				return false;
			}
			OutConfig.ScriptOptions.bAllowLuaInvoke = bFlag;
		}
		if (InJson.HasField(TEXT("allowLuaInCond")))
		{
			if (!InJson.TryGetBoolField(TEXT("allowLuaInCond"), bFlag))
			{
				OutError = TEXT("allowLuaInCond 要写 true / false");
				return false;
			}
			OutConfig.ScriptOptions.bAllowLuaInCond = bFlag;
		}
		if (InJson.HasField(TEXT("maxCallDepth")))
		{
			int32 Depth = 0;
			if (!GameTestJson::TryGetInt(InJson, TEXT("maxCallDepth"), Depth) || Depth < 1 || Depth > 32)
			{
				OutError = TEXT("maxCallDepth 要写 1–32 的整数（call 最多嵌套几层）");
				return false;
			}
			OutConfig.ScriptOptions.MaxCallDepth = Depth;
		}

		// 只有 blockers：1.9.0 之前的写法，改写成 guards；两个都有说明是改写后又被旧工具写回了 blockers，报错让人处理。
		TSharedPtr<FJsonObject> Working = MakeShared<FJsonObject>(InJson);
		if (InJson.HasField(TEXT("blockers")))
		{
			if (InJson.HasField(TEXT("guards")))
			{
				OutError = TEXT("blockers 已改成 guards，见 docs/game-test-script.md 第 5.1 节：把 blockers 里的规则挪进 guards 后删掉 blockers");
				return false;
			}
			OutConfig.bMigratedBlockers = GameTestGuards::MigrateBlockers(*Working, OutConfig.MigrationNotes);
		}
		if (Working->HasField(TEXT("guards")))
		{
			OutConfig.Guards = Working->TryGetField(TEXT("guards"));
			FGameTestDiagSink Sink;
			FGameTestScriptParser Parser(RunConfigScenarioRoot(), Sink, OutConfig.ScriptOptions);
			GameTestGuards::ParseList(OutConfig.Guards, TEXT("global"), TEXT("guards"), Parser, OutConfig.ParsedGuards);
			if (Sink.HasErrors())
			{
				TArray<FString> Lines;
				Sink.AppendErrorLines(Lines);
				OutError = FString::Printf(TEXT("守护写错了：%s"), *FString::Join(Lines, TEXT("；")));
				return false;
			}
		}
		OutConfig.Raw = Working;
		return true;
	}

	/** 以 .bak、.bak.1… 备份原文件（不覆盖已有的备份）。 */
	FString BackupPath(const FString& InPath)
	{
		FString Candidate = InPath + TEXT(".bak");
		for (int32 Index = 1; IFileManager::Get().FileExists(*Candidate) && Index < 1000; ++Index)
		{
			Candidate = FString::Printf(TEXT("%s.bak.%d"), *InPath, Index);
		}
		return Candidate;
	}
}

const TCHAR* GameTestRunConfig::FileName()
{
	return TEXT("_settings.json");
}

FString GameTestRunConfig::GetPath()
{
	return UGameTestSettings::ResolveScenarioRootDir() / FileName();
}

const TArray<FString>& GameTestRunConfig::KnownFields()
{
	static const TArray<FString> Fields = {
		TEXT("uiWaitTimeoutMs"), TEXT("uiStepGapMs"), TEXT("clickVia"), TEXT("pieWindow"), TEXT("randomSeed"), TEXT("move"), TEXT("skipIf"), TEXT("setup"),
		TEXT("identRules"), TEXT("closeRules"), TEXT("refDefaults"), TEXT("allowDestroy"), TEXT("guards"), TEXT("allowLuaInvoke"), TEXT("allowLuaInCond"), TEXT("maxCallDepth"), TEXT("blockers"),
	};
	return Fields;
}

void GameTestRunConfig::PreserveUnknown(FJsonObject& InOutIncoming, const FJsonObject& InExisting)
{
	for (const auto& Pair : InExisting.Values)
	{
		const FString Key = GAMETEST_JSON_KEY(Pair.Key);
		if (!KnownFields().Contains(Key) && !InOutIncoming.HasField(Key))
		{
			InOutIncoming.SetField(Key, Pair.Value);
		}
	}
}

TArray<FString> GameTestRunConfig::GuardScreens(const FGameTestRunConfig& InConfig)
{
	TArray<FString> Screens;
	for (const TSharedRef<FGameTestGuardDef>& Guard : InConfig.ParsedGuards)
	{
		if (!Guard->bDisabled)
		{
			for (const FString& Screen : Guard->Screens)
			{
				Screens.AddUnique(Screen);
			}
		}
	}
	return Screens;
}

bool GameTestRunConfig::Load(FGameTestRunConfig& OutConfig, FString& OutError)
{
	OutConfig = FGameTestRunConfig();
	const FString Path = GetPath();
	if (!IFileManager::Get().FileExists(*Path))
	{
		return true;
	}
	const TSharedPtr<FJsonObject> Json = GameTestJson::ParseObjectFromFile(Path, OutError);
	if (!Json.IsValid())
	{
		OutError = FString::Printf(TEXT("运行配置 %s 读不了：%s"), *Path, *OutError);
		return false;
	}
	if (!FromJson(*Json, OutConfig, OutError))
	{
		OutError = FString::Printf(TEXT("运行配置 %s：%s"), *Path, *OutError);
		return false;
	}
	if (OutConfig.bMigratedBlockers)
	{
		// 一次性改写文件：原文件留备份，改不了（只读、P4 没签出）也照样按改写后的跑，下次再试。
		const FString Backup = BackupPath(Path);
		FString SaveError;
		if (IFileManager::Get().Copy(*Backup, *Path) == COPY_OK && Save(OutConfig, SaveError))
		{
			UE_LOG(LogGameTest, Display, TEXT("运行配置 %s 的 blockers 已改写成 guards（原文件存为 %s）：%s"), *Path, *Backup,
				*FString::Join(OutConfig.MigrationNotes, TEXT("；")));
			OutConfig.bMigratedBlockers = false;
		}
		else
		{
			UE_LOG(LogGameTest, Warning, TEXT("运行配置 %s 的 blockers 要改写成 guards，但写不了文件（%s）：这次按改写后的跑"), *Path, *SaveError);
		}
	}
	return true;
}

bool GameTestRunConfig::FromJson(const FJsonObject& InJson, FGameTestRunConfig& OutConfig, FString& OutError)
{
	OutConfig = FGameTestRunConfig();
	int32 Wait = -1;
	if (GameTestJson::TryGetInt(InJson, TEXT("uiWaitTimeoutMs"), Wait))
	{
		if (Wait < 0)
		{
			OutError = TEXT("uiWaitTimeoutMs 要 >= 0（0 = 不限）");
			return false;
		}
		OutConfig.UiWaitTimeoutMs = Wait;
	}

	int32 Gap = -1;
	if (InJson.HasField(TEXT("uiStepGapMs")))
	{
		if (!GameTestJson::TryGetInt(InJson, TEXT("uiStepGapMs"), Gap) || Gap < 0 || Gap > MaxUiStepGapMs)
		{
			OutError = FString::Printf(TEXT("uiStepGapMs 要写 0–%d 的整数（毫秒，0 = UI 操作之间不额外等）"), MaxUiStepGapMs);
			return false;
		}
		OutConfig.UiStepGapMs = Gap;
	}

	if (GameTestJson::TryGetString(InJson, TEXT("clickVia"), OutConfig.ClickVia) && !OutConfig.ClickVia.IsEmpty()
		&& OutConfig.ClickVia != TEXT("code") && OutConfig.ClickVia != TEXT("pointer"))
	{
		OutError = TEXT("clickVia 只能是 code（调控件的点击委托，不动鼠标）或 pointer（真实鼠标）");
		return false;
	}

	if (GameTestJson::TryGetString(InJson, TEXT("pieWindow"), OutConfig.PieWindow) && !OutConfig.PieWindow.IsEmpty()
		&& OutConfig.PieWindow != TEXT("viewport") && OutConfig.PieWindow != TEXT("new"))
	{
		OutError = TEXT("pieWindow 只能是 viewport（选中的视口，默认）或 new（独立窗口）");
		return false;
	}

	int32 Seed = 0;
	if (InJson.HasField(TEXT("randomSeed")))
	{
		if (!GameTestJson::TryGetInt(InJson, TEXT("randomSeed"), Seed))
		{
			OutError = TEXT("randomSeed 要写整数（不固定就删掉这一项）");
			return false;
		}
		OutConfig.RandomSeed = Seed;
	}

	if (InJson.HasField(TEXT("move")))
	{
		OutConfig.Move = GameTestJson::TryGetObject(InJson, TEXT("move"));
		if (!OutConfig.Move.IsValid())
		{
			OutError = TEXT("move 要写成对象（{\"fallback\": \"none\", ...}）");
			return false;
		}
		if (!GameTestMove::ValidateDefaults(*OutConfig.Move, OutError))
		{
			return false;
		}
	}

	if (InJson.HasField(TEXT("skipIf")))
	{
		OutConfig.SkipIf = InJson.TryGetField(TEXT("skipIf"));
		FGameTestSkipOptions Options;
		if (!OutConfig.SkipIf.IsValid() || !GameTestSkipIf::ApplyOptions(*OutConfig.SkipIf, Options, OutError))
		{
			return false;
		}
	}

	// 前置步骤只检查是不是对象：动作和字段由开跑时的步骤解析检查（和场景步骤同一套，报错带「前置第 N 步」）。
	if (const TArray<TSharedPtr<FJsonValue>>* SetupItems = GameTestJson::TryGetArray(InJson, TEXT("setup")))
	{
		for (int32 Index = 0; Index < SetupItems->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!(*SetupItems)[Index].IsValid() || !(*SetupItems)[Index]->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
			{
				OutError = FString::Printf(TEXT("前置步骤第 %d 条不是对象（要写成 {\"action\": ...}）"), Index + 1);
				return false;
			}
			OutConfig.Setup.Add(*Object);
		}
	}

	return ParseScriptSection(InJson, OutConfig, OutError);
}

TSharedPtr<FJsonObject> GameTestRunConfig::ToJson(const FGameTestRunConfig& InConfig)
{
	// 从原文出发：不认识的字段（api…）原样留着；认得的字段先清掉，再按配置写回。
	TSharedPtr<FJsonObject> Json = InConfig.Raw.IsValid() ? MakeShared<FJsonObject>(*InConfig.Raw) : MakeShared<FJsonObject>();
	for (const FString& Key : KnownFields())
	{
		Json->RemoveField(Key);
	}
	if (InConfig.UiWaitTimeoutMs >= 0)
	{
		Json->SetNumberField(TEXT("uiWaitTimeoutMs"), InConfig.UiWaitTimeoutMs);
	}
	if (InConfig.UiStepGapMs >= 0)
	{
		Json->SetNumberField(TEXT("uiStepGapMs"), InConfig.UiStepGapMs);
	}
	if (!InConfig.ClickVia.IsEmpty())
	{
		Json->SetStringField(TEXT("clickVia"), InConfig.ClickVia);
	}
	if (!InConfig.PieWindow.IsEmpty())
	{
		Json->SetStringField(TEXT("pieWindow"), InConfig.PieWindow);
	}
	if (InConfig.RandomSeed.IsSet())
	{
		Json->SetNumberField(TEXT("randomSeed"), InConfig.RandomSeed.GetValue());
	}
	if (InConfig.Move.IsValid() && InConfig.Move->Values.Num() > 0)
	{
		Json->SetObjectField(TEXT("move"), InConfig.Move);
	}
	if (InConfig.SkipIf.IsValid())
	{
		Json->SetField(TEXT("skipIf"), InConfig.SkipIf);
	}
	if (InConfig.Setup.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> SetupItems;
		for (const TSharedPtr<FJsonObject>& Step : InConfig.Setup)
		{
			SetupItems.Add(MakeShared<FJsonValueObject>(Step));
		}
		Json->SetArrayField(TEXT("setup"), SetupItems);
	}
	if (InConfig.Guards.IsValid())
	{
		Json->SetField(TEXT("guards"), InConfig.Guards);
	}
	const FGameTestScriptOptions Defaults;
	if (InConfig.ScriptOptions.bAllowLuaInvoke != Defaults.bAllowLuaInvoke)
	{
		Json->SetBoolField(TEXT("allowLuaInvoke"), InConfig.ScriptOptions.bAllowLuaInvoke);
	}
	if (InConfig.ScriptOptions.bAllowLuaInCond != Defaults.bAllowLuaInCond)
	{
		Json->SetBoolField(TEXT("allowLuaInCond"), InConfig.ScriptOptions.bAllowLuaInCond);
	}
	if (InConfig.ScriptOptions.MaxCallDepth != Defaults.MaxCallDepth)
	{
		Json->SetNumberField(TEXT("maxCallDepth"), InConfig.ScriptOptions.MaxCallDepth);
	}
	GameTestRunConfigRef::Write(InConfig.ScriptOptions, *Json);
	return Json;
}

bool GameTestRunConfig::Save(const FGameTestRunConfig& InConfig, FString& OutError)
{
	const FString Path = GetPath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	const FString Text = GameTestJson::ToPrettyString(ToJson(InConfig));
	if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("写不进 %s：文件被占用或只读（P4 项目先签出）"), *Path);
		return false;
	}
	return true;
}
