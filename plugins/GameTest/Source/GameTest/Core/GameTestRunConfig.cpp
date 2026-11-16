// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestRunConfig.h"

#include "Core/GameTestJson.h"
#include "GameTestSettings.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	bool ParseAction(const FString& InName, EGameTestBlockerAction& OutAction)
	{
		if (InName.IsEmpty() || InName == TEXT("close"))
		{
			OutAction = EGameTestBlockerAction::Close;
			return true;
		}
		if (InName == TEXT("call"))
		{
			OutAction = EGameTestBlockerAction::Call;
			return true;
		}
		if (InName == TEXT("command"))
		{
			OutAction = EGameTestBlockerAction::Command;
			return true;
		}
		return false;
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

const TCHAR* GameTestRunConfig::ActionName(EGameTestBlockerAction InAction)
{
	switch (InAction)
	{
	case EGameTestBlockerAction::Call:
		return TEXT("call");
	case EGameTestBlockerAction::Command:
		return TEXT("command");
	default:
		return TEXT("close");
	}
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

	const TArray<TSharedPtr<FJsonValue>>* Items = GameTestJson::TryGetArray(InJson, TEXT("blockers"));
	if (Items == nullptr)
	{
		return true;
	}
	TArray<FString> Errors;
	for (int32 Index = 0; Index < Items->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Object) || Object == nullptr)
		{
			Errors.Add(FString::Printf(TEXT("第 %d 条阻挡界面规则不是对象"), Index + 1));
			continue;
		}
		const FJsonObject& Item = **Object;
		FGameTestBlockerRule Rule;
		FString ActionText;
		GameTestJson::TryGetString(Item, TEXT("widget"), Rule.Widget);
		GameTestJson::TryGetString(Item, TEXT("action"), ActionText);
		GameTestJson::TryGetString(Item, TEXT("function"), Rule.Function);
		GameTestJson::TryGetString(Item, TEXT("command"), Rule.Command);
		GameTestJson::TryGetString(Item, TEXT("note"), Rule.Note);
		GameTestJson::TryGetInt(Item, TEXT("delayMs"), Rule.DelayMs);
		GameTestJson::TryGetBool(Item, TEXT("enabled"), Rule.bEnabled);
		Rule.Widget.TrimStartAndEndInline();
		Rule.Function.TrimStartAndEndInline();
		Rule.Command.TrimStartAndEndInline();
		Rule.DelayMs = FMath::Clamp(Rule.DelayMs, 0, 60000);

		if (Rule.Widget.IsEmpty())
		{
			Errors.Add(FString::Printf(TEXT("第 %d 条阻挡界面规则没写 widget（WBP 类名或资产路径）"), Index + 1));
			continue;
		}
		if (!ParseAction(ActionText, Rule.Action))
		{
			Errors.Add(FString::Printf(TEXT("第 %d 条（%s）的 action 只能是 close / call / command"), Index + 1, *Rule.Widget));
			continue;
		}
		if (Rule.Action == EGameTestBlockerAction::Call && Rule.Function.IsEmpty())
		{
			Errors.Add(FString::Printf(TEXT("第 %d 条（%s）action=call 要写 function（界面上的无参函数名）"), Index + 1, *Rule.Widget));
			continue;
		}
		if (Rule.Action == EGameTestBlockerAction::Command && Rule.Command.IsEmpty())
		{
			Errors.Add(FString::Printf(TEXT("第 %d 条（%s）action=command 要写 command（控制台指令）"), Index + 1, *Rule.Widget));
			continue;
		}
		OutConfig.Blockers.Add(MoveTemp(Rule));
	}
	if (Errors.Num() > 0)
	{
		OutError = FString::Join(Errors, TEXT("；"));
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> GameTestRunConfig::ToJson(const FGameTestRunConfig& InConfig)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	if (InConfig.UiWaitTimeoutMs >= 0)
	{
		Json->SetNumberField(TEXT("uiWaitTimeoutMs"), InConfig.UiWaitTimeoutMs);
	}
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FGameTestBlockerRule& Rule : InConfig.Blockers)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("widget"), Rule.Widget);
		Item->SetStringField(TEXT("action"), ActionName(Rule.Action));
		if (!Rule.Function.IsEmpty())
		{
			Item->SetStringField(TEXT("function"), Rule.Function);
		}
		if (!Rule.Command.IsEmpty())
		{
			Item->SetStringField(TEXT("command"), Rule.Command);
		}
		if (!Rule.Note.IsEmpty())
		{
			Item->SetStringField(TEXT("note"), Rule.Note);
		}
		if (Rule.DelayMs > 0)
		{
			Item->SetNumberField(TEXT("delayMs"), Rule.DelayMs);
		}
		if (!Rule.bEnabled)
		{
			Item->SetBoolField(TEXT("enabled"), false);
		}
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	Json->SetArrayField(TEXT("blockers"), Items);
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
