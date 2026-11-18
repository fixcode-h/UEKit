// Copyright Epic Games, Inc. All Rights Reserved.

// gt.validate：不跑场景，先查写得对不对（协议第 10 节）。
// 结构（v3 写法、块、条件、表达式、变量）由 FGameTestScriptParser 查；deep 时再交给深度检查器
// （函数、接口、选择器、取值来源、对象定位器在项目里有没有）。两边的错误都进同一个 FGameTestDiagSink。

#include "Core/Flow/Deep/GameTestDeepCheck.h"
#include "Core/Flow/GameTestNode.h"
#include "Core/Flow/GameTestScriptParser.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestRunConfig.h"
#include "Core/GameTestWorld.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestSettings.h"
#include "Integration/GameTestRpcScript.h"
#include "Misc/Paths.h"

namespace GameTestValidateLocal
{
	/** 运行设置里和脚本有关的几项（allowLuaInvoke / allowLuaInCond / maxCallDepth）：和执行器读同一份；没有设置文件就用默认值。 */
	FGameTestScriptOptions ReadOptions()
	{
		FGameTestRunConfig Config;
		FString Error;
		GameTestRunConfig::Load(Config, Error);
		return Config.ScriptOptions;
	}

	int32 CountCalls(const FGameTestNodeList& InList)
	{
		int32 Count = 0;
		for (const TSharedRef<FGameTestNode>& Node : InList.Nodes)
		{
			Count += Node->Action == FName(TEXT("call")) ? 1 : 0;
			for (const FGameTestNodeList& Child : Node->Lists)
			{
				Count += CountCalls(Child);
			}
		}
		return Count;
	}

	/** 表达式（`"=…"` 开头的字符串）有几处：给 checked.exprs。 */
	int32 CountExprs(const TSharedPtr<FJsonValue>& InValue)
	{
		if (!InValue.IsValid())
		{
			return 0;
		}
		FString Text;
		if (InValue->TryGetString(Text))
		{
			return Text.StartsWith(TEXT("=")) ? 1 : 0;
		}
		int32 Count = 0;
		if (InValue->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : InValue->AsArray())
			{
				Count += CountExprs(Item);
			}
		}
		else if (InValue->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Object = InValue->AsObject();
			if (Object.IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
				{
					Count += CountExprs(Pair.Value);
				}
			}
		}
		return Count;
	}
}

namespace GameTestRpcScript
{
	bool Validate(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		using namespace GameTestValidateLocal;
		const FString Root = UGameTestSettings::ResolveScenarioRootDir();
		TSharedPtr<FJsonObject> Script;
		FString Path;
		if (InArgs->TryGetStringField(TEXT("scenario"), Path) && !Path.IsEmpty())
		{
			Path = Path.Replace(TEXT("\\"), TEXT("/"));
			Script = GameTestScriptFiles::LoadJson(FPaths::Combine(Root, Path), OutError);
			if (!Script.IsValid())
			{
				return false;
			}
		}
		else
		{
			const TSharedPtr<FJsonObject>* Given = nullptr;
			if (!InArgs->TryGetObjectField(TEXT("script"), Given) || Given == nullptr || !Given->IsValid())
			{
				OutError = TEXT("gt.validate 要给 scenario（相对场景根的路径）或 script（整份场景 JSON）");
				return false;
			}
			Script = *Given;
			Path = TEXT("(script)");
		}
		bool bDeep = true;
		InArgs->TryGetBoolField(TEXT("deep"), bDeep);

		FGameTestDiagSink Sink;
		const FGameTestScriptOptions Options = ReadOptions();
		FGameTestScriptParser Parser(Root, Sink, Options);
		const TSharedRef<FGameTestScript> Parsed = Parser.ParseScript(Script, Path);

		FGameTestDeepStats Stats;
		if (bDeep)
		{
			FGameTestFlowContext Context;
			Context.World = GameTestWorld::FindGameWorld();
			Context.Options = Options;
			Context.ScenarioRoot = Root;
			GameTestDeepChecks::Run(*Parsed, Context, Sink, Stats);
		}

		TArray<TSharedPtr<FJsonValue>> Errors, Warnings;
		for (const FGameTestDiag& Diag : Sink.GetAll())
		{
			(Diag.bWarning ? Warnings : Errors).Add(MakeShared<FJsonValueObject>(Diag.ToJson()));
		}
		OutResult->SetBoolField(TEXT("ok"), Errors.Num() == 0);
		OutResult->SetArrayField(TEXT("errors"), Errors);
		OutResult->SetArrayField(TEXT("warnings"), Warnings);
		const TSharedRef<FJsonObject> Checked = GameTestDeepChecks::StatsToJson(Stats);
		Checked->SetNumberField(TEXT("steps"), Parsed->CountLeaves());
		Checked->SetNumberField(TEXT("calls"), CountCalls(Parsed->Setup) + CountCalls(Parsed->Steps));
		Checked->SetNumberField(TEXT("exprs"), CountExprs(MakeShared<FJsonValueObject>(Script)));
		OutResult->SetObjectField(TEXT("checked"), Checked);
		OutResult->SetBoolField(TEXT("deep"), bDeep);
		if (bDeep && GameTestWorld::FindGameWorld() == nullptr)
		{
			// 没开游戏：选择器只查到资产，运行时才有的对象（玩家、子系统实例）不查
			OutResult->SetStringField(TEXT("note"), TEXT("游戏没开：选择器按 WBP 资产查，运行时对象没查"));
		}
		return true;
	}
}
