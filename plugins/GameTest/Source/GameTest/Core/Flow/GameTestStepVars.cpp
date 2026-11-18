// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/GameTestStepVars.h"

#include "Core/Flow/GameTestNode.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Core/Script/GameTestValue.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"

namespace GameTestStepVars
{
	namespace
	{
		/** 块里装子步骤的字段：子步骤各自有记录，块的变量里不重复列它们的。 */
		bool IsChildListKey(const FString& InKey)
		{
			return InKey == TEXT("then") || InKey == TEXT("else") || InKey == TEXT("do") || InKey == TEXT("steps") || InKey == TEXT("setup") || InKey == TEXT("guards");
		}

		/** 写变量的字段：值是变量名本身，不带 `$`，CollectVars 认不出来。 */
		void AddWrittenName(const FJsonObject& InJson, const TCHAR* InField, TSet<FString>& OutNames)
		{
			FString Name;
			if (InJson.TryGetStringField(InField, Name) && !Name.IsEmpty())
			{
				OutNames.Add(Name);
			}
		}

		/** elif 每一项只收条件，不收它的 then。 */
		void CollectElif(const TSharedPtr<FJsonValue>& InValue, TSet<FString>& OutNames)
		{
			if (!InValue.IsValid() || InValue->Type != EJson::Array)
			{
				GameTestRefs::CollectVars(InValue, OutNames);
				return;
			}
			for (const TSharedPtr<FJsonValue>& Item : InValue->AsArray())
			{
				if (!Item.IsValid() || Item->Type != EJson::Object)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> Branch = Item->AsObject();
				if (!Branch.IsValid())
				{
					continue;
				}
				for (const auto& Pair : Branch->Values)
				{
					if (GAMETEST_JSON_KEY(Pair.Key) != TEXT("then"))
					{
						GameTestRefs::CollectVars(Pair.Value, OutNames);
					}
				}
			}
		}
	}

	void CollectNames(const FGameTestNode& InNode, TSet<FString>& OutNames)
	{
		if (!InNode.Json.IsValid())
		{
			return;
		}
		const bool bBlock = InNode.IsBlock();
		for (const auto& Pair : InNode.Json->Values)
		{
			const FString Key = GAMETEST_JSON_KEY(Pair.Key);
			if (bBlock && IsChildListKey(Key))
			{
				continue;
			}
			if (bBlock && Key == TEXT("elif"))
			{
				CollectElif(Pair.Value, OutNames);
				continue;
			}
			GameTestRefs::CollectVars(Pair.Value, OutNames);
		}
		AddWrittenName(*InNode.Json, TEXT("saveAs"), OutNames);
		if (bBlock)
		{
			AddWrittenName(*InNode.Json, TEXT("var"), OutNames);
			AddWrittenName(*InNode.Json, TEXT("as"), OutNames);
		}
	}

	TSharedPtr<FJsonObject> Pick(const FGameTestFlowContext& InContext, const TSet<FString>& InNames, int32 InMaxChars)
	{
		if (InNames.Num() == 0)
		{
			return nullptr;
		}
		// 按名字排序：报告里同一步每次跑出来的顺序一致，面板对比两次运行时不会因为顺序乱跳。
		TArray<FString> Names = InNames.Array();
		Names.Sort();
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		int32 Used = 0;
		for (const FString& Name : Names)
		{
			FGameTestValue Value;
			if (!InContext.FindVar(Name, Value))
			{
				continue;
			}
			const FString Shown = Value.ToDisplay(300);
			if (InMaxChars > 0 && Used + Shown.Len() + Name.Len() > InMaxChars)
			{
				Out->SetBoolField(TEXT("_truncated"), true);
				break;
			}
			Used += Shown.Len() + Name.Len();
			// 长的值（大数组、对象）只给截过的文字：报告每步一份，原样写会把报告撑得很大。
			if (Shown.Len() >= 300)
			{
				Out->SetStringField(Name, Shown);
			}
			else
			{
				Out->SetField(Name, Value.ToJson());
			}
		}
		return Out->Values.Num() > 0 ? Out : nullptr;
	}
}
