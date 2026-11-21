// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestKeyRules.h"

#include "Core/GameTestRunConfig.h"
#include "Core/GameTestRunConfigRef.h"
#include "Observe/Ref/GameTestPropPathKey.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTest.h"

#include "HAL/FileManager.h"

namespace GameTestKeyRulesDetail
{
	struct FCache
	{
		FString Path;
		FDateTime Stamp;
		bool bLoaded = false;
		TArray<FGameTestKeyRule> Rules;
	};
	FCache GCache;

	/** identRules 的 prop（`@item.ItemId`，v5 路径）→ 录制的 by（默认来源 item 不写前缀）。录制认不了的（子控件、函数）原样留着，挑的时候读不到就不用。 */
	TArray<FGameTestKeyRule> FromIdentRules(const TSharedPtr<FJsonValue>& InRules)
	{
		TArray<FGameTestKeyRule> Out;
		const TArray<TSharedPtr<FJsonValue>>* Rules = nullptr;
		if (!InRules.IsValid() || !InRules->TryGetArray(Rules) || Rules == nullptr)
		{
			return Out;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Rules)
		{
			const TSharedPtr<FJsonObject>* Rule = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Ident = nullptr;
			FGameTestKeyRule Converted;
			if (!Value.IsValid() || !Value->TryGetObject(Rule) || Rule == nullptr || !Rule->IsValid() || !(*Rule)->TryGetStringField(TEXT("class"), Converted.Entry)
				|| !(*Rule)->TryGetArrayField(TEXT("ident"), Ident) || Ident == nullptr)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& Prop : *Ident)
			{
				FString Text;
				if (Prop.IsValid() && Prop->TryGetString(Text) && !Text.IsEmpty())
				{
					FString By;
					Converted.By.Add(GameTestPropPathKey::ToBy(Text, /*bInList=*/true, By) ? By : Text);
				}
			}
			if (Converted.By.Num() > 0)
			{
				Out.Add(MoveTemp(Converted));
			}
		}
		return Out;
	}

	const TArray<FGameTestKeyRule>& Rules()
	{
		const FString Path = GameTestRunConfig::GetPath();
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Path);
		if (!GCache.bLoaded || GCache.Path != Path || GCache.Stamp != Stamp)
		{
			GCache.bLoaded = true;
			GCache.Path = Path;
			GCache.Stamp = Stamp;
			GCache.Rules.Reset();
			FGameTestRunConfig Config;
			FString Error;
			if (GameTestRunConfig::Load(Config, Error))
			{
				GCache.Rules = FromIdentRules(Config.ScriptOptions.IdentRules);
			}
			else
			{
				// 配置写坏了不影响录制：只是规则用不上，运行设置页会显示同样的错
				UE_LOG(LogGameTest, Warning, TEXT("条目规则读不了：%s"), *Error);
			}
		}
		return GCache.Rules;
	}

	FString StripC(const FString& InClass)
	{
		return InClass.EndsWith(TEXT("_C")) ? InClass.LeftChop(2) : InClass;
	}
}

bool GameTestKeyRules::SameClass(const FString& InLeft, const FString& InRight)
{
	return GameTestKeyRulesDetail::StripC(InLeft).Equals(GameTestKeyRulesDetail::StripC(InRight), ESearchCase::IgnoreCase);
}

bool GameTestKeyRules::Find(const FString& InEntryClass, const FString& InBase, FGameTestKeyRule& OutRule)
{
	const FGameTestKeyRule* Loose = nullptr;
	for (const FGameTestKeyRule& Rule : GameTestKeyRulesDetail::Rules())
	{
		if (!SameClass(Rule.Entry, InEntryClass))
		{
			continue;
		}
		if (Rule.Container.IsEmpty())
		{
			Loose = Loose != nullptr ? Loose : &Rule;
			continue;
		}
		// 容器写的可能是完整选择器，也可能只写了后半段（WBP_Bag:Root/ItemList、Root/ItemList）
		if (InBase.Equals(Rule.Container, ESearchCase::IgnoreCase) || InBase.EndsWith(Rule.Container, ESearchCase::IgnoreCase))
		{
			OutRule = Rule;
			return true;
		}
	}
	if (Loose != nullptr)
	{
		OutRule = *Loose;
		return true;
	}
	return false;
}

bool GameTestKeyRules::Save(const FGameTestKeyRule& InRule, FString& OutError)
{
	// 录制的 by 不带 @ 时是条目数据对象上的字段：写进 identRules 时换成 v5 路径（@item.X、@.X、@#X…）
	TArray<FString> Props;
	for (const FString& By : InRule.By)
	{
		const FString Prop = GameTestPropPathKey::FromBy(By, /*bInList=*/true);
		Props.Add(Prop.IsEmpty() ? By : Prop);
	}
	if (!GameTestRunConfigRef::SaveIdentRule(InRule.Entry, Props, OutError))
	{
		return false;
	}
	GameTestKeyRulesDetail::GCache.bLoaded = false;
	return true;
}
