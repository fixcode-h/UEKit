// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestKeyRules.h"

#include "Core/GameTestRunConfig.h"
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
				GCache.Rules = MoveTemp(Config.ItemKeys);
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
	FGameTestRunConfig Config;
	if (!GameTestRunConfig::Load(Config, OutError))
	{
		return false;
	}
	Config.ItemKeys.RemoveAll([&InRule](const FGameTestKeyRule& Rule)
	{
		return SameClass(Rule.Entry, InRule.Entry) && Rule.Container.Equals(InRule.Container, ESearchCase::IgnoreCase);
	});
	Config.ItemKeys.Add(InRule);
	if (!GameTestRunConfig::Save(Config, OutError))
	{
		return false;
	}
	GameTestKeyRulesDetail::GCache.bLoaded = false;
	return true;
}
