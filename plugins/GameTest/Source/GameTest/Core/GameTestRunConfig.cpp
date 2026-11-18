// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestRunConfig.h"

#include "Core/GameTestJson.h"
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

	if (const TArray<TSharedPtr<FJsonValue>>* Keys = GameTestJson::TryGetArray(InJson, TEXT("itemKeys")))
	{
		for (int32 Index = 0; Index < Keys->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!(*Keys)[Index].IsValid() || !(*Keys)[Index]->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
			{
				OutError = FString::Printf(TEXT("条目规则（itemKeys）第 %d 条不是对象"), Index + 1);
				return false;
			}
			FGameTestKeyRule Rule;
			GameTestJson::TryGetString(**Object, TEXT("entry"), Rule.Entry);
			GameTestJson::TryGetString(**Object, TEXT("container"), Rule.Container);
			Rule.Entry.TrimStartAndEndInline();
			Rule.Container.TrimStartAndEndInline();
			if (const TArray<TSharedPtr<FJsonValue>>* By = GameTestJson::TryGetArray(**Object, TEXT("by")))
			{
				for (const TSharedPtr<FJsonValue>& Value : *By)
				{
					FString Field;
					if (Value.IsValid() && Value->TryGetString(Field) && !Field.TrimStartAndEnd().IsEmpty())
					{
						Rule.By.Add(Field.TrimStartAndEnd());
					}
				}
			}
			if (Rule.Entry.IsEmpty() || Rule.By.Num() == 0)
			{
				OutError = FString::Printf(TEXT("条目规则（itemKeys）第 %d 条要写 entry（条目类名）和 by（字段数组）"), Index + 1);
				return false;
			}
			OutConfig.ItemKeys.Add(MoveTemp(Rule));
		}
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
	if (InConfig.UiStepGapMs >= 0)
	{
		Json->SetNumberField(TEXT("uiStepGapMs"), InConfig.UiStepGapMs);
	}
	if (!InConfig.ClickVia.IsEmpty())
	{
		Json->SetStringField(TEXT("clickVia"), InConfig.ClickVia);
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
	if (InConfig.ItemKeys.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Keys;
		for (const FGameTestKeyRule& Rule : InConfig.ItemKeys)
		{
			const TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
			Key->SetStringField(TEXT("entry"), Rule.Entry);
			if (!Rule.Container.IsEmpty())
			{
				Key->SetStringField(TEXT("container"), Rule.Container);
			}
			TArray<TSharedPtr<FJsonValue>> By;
			for (const FString& Field : Rule.By)
			{
				By.Add(MakeShared<FJsonValueString>(Field));
			}
			Key->SetArrayField(TEXT("by"), By);
			Keys.Add(MakeShared<FJsonValueObject>(Key));
		}
		Json->SetArrayField(TEXT("itemKeys"), Keys);
	}
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
