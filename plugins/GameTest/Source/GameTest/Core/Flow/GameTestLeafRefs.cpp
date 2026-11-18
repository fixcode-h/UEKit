// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/GameTestLeafRefs.h"

#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"

namespace
{
	/** 不求值的字段（见头文件）。 */
	const TSet<FString>& KeptFields()
	{
		static const TSet<FString> Fields = {
			TEXT("action"), TEXT("id"), TEXT("note"), TEXT("hint"), TEXT("onFail"), TEXT("retries"), TEXT("timeoutMs"), TEXT("disabled"),
			TEXT("expectNoErrors"), TEXT("guards"), TEXT("skipIf"), TEXT("cond"), TEXT("expr"), TEXT("expect"), TEXT("until"),
			TEXT("args"), TEXT("object"), TEXT("with"), TEXT("saveAs"),
		};
		return Fields;
	}

	/** 写选择器的字段：控件变量要换成句柄，不能按文字替换。 */
	bool IsSelectorField(const FString& InKey)
	{
		return InKey == TEXT("target") || InKey == TEXT("toTarget") || InKey == TEXT("container") || InKey == TEXT("each");
	}
}

namespace GameTestLeafRefs
{
	bool Resolve(FGameTestStep& InOutStep, FGameTestFlowContext& InContext, FString& OutError)
	{
		if (!InOutStep.Params.IsValid())
		{
			return true;
		}
		TSharedPtr<FJsonObject> Copy;
		for (const auto& Pair : InOutStep.Params->Values)
		{
			const FString Key = GAMETEST_JSON_KEY(Pair.Key);
			if (KeptFields().Contains(Key) || !GameTestRefs::HasRefs(Pair.Value))
			{
				continue;
			}
			if (!Copy.IsValid())
			{
				// 解析好的节点是只读、跨 run 共用的：在副本上改。
				Copy = MakeShared<FJsonObject>(*InOutStep.Params);
			}
			FString Text;
			if (IsSelectorField(Key) && Pair.Value.IsValid() && Pair.Value->TryGetString(Text))
			{
				FString Selector;
				FString Error;
				if (!GameTestRefs::EvalSelector(Text, InContext, Selector, Error))
				{
					OutError = FString::Printf(TEXT("%s 求值失败：%s"), *Key, *Error);
					return false;
				}
				Copy->SetStringField(Key, Selector);
				continue;
			}
			const FGameTestValue Value = GameTestRefs::EvalJson(Pair.Value, InContext);
			if (Value.IsUnresolved())
			{
				OutError = FString::Printf(TEXT("%s 取不到值：%s"), *Key, *Value.GetString());
				return false;
			}
			Copy->SetField(Key, Value.ToJson());
		}
		if (Copy.IsValid())
		{
			InOutStep.Params = Copy;
			Copy->TryGetStringField(TEXT("target"), InOutStep.Target);
		}
		return true;
	}
}
