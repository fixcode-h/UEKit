// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Flow/Blocks/GameTestBlock.h"
#include "Core/Flow/GameTestNode.h"
#include "Core/Flow/GameTestScriptParser.h"
#include "Core/Script/GameTestDiag.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"
#include "Observe/Checks/GameTestCond.h"

/** 各个块实现共用的小工具（只给 Core/Flow/Blocks 里的实现用）。 */
namespace GameTestBlockUtil
{
	/** 读条件字段：检查写法并转成条件对象（字符串当表达式）。缺了且 `bRequired` 时报错，返回空。 */
	inline TSharedPtr<FJsonObject> ParseCond(FGameTestScriptParser& InParser, const FGameTestNode& InNode, const TCHAR* InField, bool bRequired)
	{
		const TSharedPtr<FJsonValue> Value = InNode.Json.IsValid() ? InNode.Json->TryGetField(InField) : nullptr;
		if (!Value.IsValid())
		{
			if (bRequired)
			{
				InParser.Sink().Error(TEXT("bad_type"), FString::Printf(TEXT("%s 缺少 %s（统一条件）"), *InNode.Action.ToString(), InField));
			}
			return nullptr;
		}
		InParser.CheckCond(Value, InField);
		TSharedPtr<FJsonObject> Cond = GameTestCond::AsObject(Value);
		if (!Cond.IsValid())
		{
			InParser.Sink().Error(TEXT("bad_type"), FString::Printf(TEXT("%s 要写成条件对象或表达式字符串"), InField));
		}
		return Cond;
	}

	/** 变量名：字母、数字、下划线，不以数字开头（`$名字` 里能认出来的）。中文名也允许。 */
	inline bool IsValidVarName(const FString& InName)
	{
		if (InName.IsEmpty() || InName.Len() > 64 || FChar::IsDigit(InName[0]) || InName.StartsWith(TEXT("__")))
		{
			return false;
		}
		for (const TCHAR C : InName)
		{
			if (!(FChar::IsAlnum(C) || C == TEXT('_') || C >= 0x80))
			{
				return false;
			}
		}
		return true;
	}

	/** 「只走一遍」的块状态：第一次 Tick 发出子节点，第二次 Tick 结束。 */
	struct FOnceState : FGameTestBlockState
	{
		bool bStarted = false;
	};
}
