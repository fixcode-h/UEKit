// Copyright Epic Games, Inc. All Rights Reserved.

// 脚本调试相关的操作（协议第 10 节）：gt.run 的调试参数、gt.debug（暂停 / 继续 / 单步 / 断点）、
// gt.resolve 的 conds / exprs（这一刻的条件和表达式值）。只是把请求转给执行器（FGameTestFlow）和统一条件，不另写判断。

#include "Integration/GameTestRpcScript.h"
#include "Core/GameTestClock.h"

#include "Core/Expr/GameTestExpr.h"
#include "Core/Flow/Blocks/GameTestBlock.h"
#include "Core/Flow/GameTestFlow.h"
#include "Core/GameTestRunConfig.h"
#include "Core/GameTestScenario.h"
#include "Core/GameTestWorld.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"
#include "GameTestSettings.h"
#include "GameTestSubsystem.h"
#include "Observe/Checks/GameTestCond.h"

namespace GameTestRpcDebugLocal
{
	/** 字符串或字符串数组 → 集合（断点 id 列表）。 */
	bool ReadIds(const TSharedPtr<FJsonValue>& InValue, TSet<FString>& OutIds, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!InValue.IsValid() || !InValue->TryGetArray(Items) || !Items)
		{
			OutError = TEXT("breakpoints / ids 要写成步骤 id 的数组（没写 id 的步骤用路径，如 steps.3.then.0）");
			return false;
		}
		OutIds.Reset();
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			FString Id;
			if (Item.IsValid() && Item->TryGetString(Id) && !Id.TrimStartAndEnd().IsEmpty())
			{
				OutIds.Add(Id.TrimStartAndEnd());
			}
		}
		return true;
	}

	/** 执行器当前状态：`state`、`at`、`path`、暂停时带 `vars`。 */
	TSharedPtr<FJsonObject> StateJson(UGameTestSubsystem& InSubsystem)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		FGameTestFlow* Flow = InSubsystem.GetFlow();
		if (Flow == nullptr || Flow->IsFinished())
		{
			Out->SetStringField(TEXT("state"), TEXT("idle"));
			return Out;
		}
		Out->SetStringField(TEXT("state"), Flow->IsPaused() ? TEXT("paused") : TEXT("running"));
		if (!Flow->GetAt().IsEmpty())
		{
			Out->SetStringField(TEXT("at"), Flow->GetAt());
			Out->SetStringField(TEXT("path"), Flow->GetAtPath());
		}
		if (Flow->IsPaused())
		{
			Out->SetObjectField(TEXT("vars"), Flow->GetContext().SnapshotVars(4000));
		}
		return Out;
	}

	/** gt.resolve 用的一次性上下文：没有 run 在跑时临时造一个（世界、句柄、运行配置的开关、传进来的 vars）。 */
	struct FTempContext
	{
		FGameTestFlowContext Context;
		bool bPushed = false;
		~FTempContext()
		{
			if (bPushed)
			{
				Context.PopScript();
			}
		}
	};
}

namespace GameTestRpcScript
{
	bool ReadRunOptions(const TSharedPtr<FJsonObject>& InArgs, FGameTestScenarioRun& InOutRun, FString& OutError)
	{
		using namespace GameTestRpcDebugLocal;
		if (!InArgs.IsValid())
		{
			return true;
		}
		if (InArgs->HasField(TEXT("params")))
		{
			const TSharedPtr<FJsonObject>* Params = nullptr;
			if (!InArgs->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
			{
				OutError = TEXT("params 要写成对象：{\"参数名\": 值}");
				return false;
			}
			InOutRun.Params = *Params;
		}
		InArgs->TryGetStringField(TEXT("from"), InOutRun.Debug.From);
		InArgs->TryGetStringField(TEXT("to"), InOutRun.Debug.To);
		InArgs->TryGetBoolField(TEXT("pauseOnFail"), InOutRun.Debug.bPauseOnFail);
		if (InArgs->HasField(TEXT("breakpoints")) && !ReadIds(InArgs->TryGetField(TEXT("breakpoints")), InOutRun.Debug.Breakpoints, OutError))
		{
			return false;
		}
		return true;
	}

	bool Debug(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		using namespace GameTestRpcDebugLocal;
		UGameTestSubsystem* Subsystem = UGameTestSubsystem::Get();
		if (Subsystem == nullptr)
		{
			OutError = TEXT("GameTest 子系统还没就绪（引擎在启动或退出中），稍后再试");
			return false;
		}
		FString Action;
		if (InArgs.IsValid())
		{
			InArgs->TryGetStringField(TEXT("action"), Action);
		}
		FGameTestFlow* Flow = Subsystem->GetFlow();
		if (Action.IsEmpty() || Action == TEXT("state"))
		{
			OutResult = StateJson(*Subsystem);
			return true;
		}
		if (Flow == nullptr || Flow->IsFinished())
		{
			OutError = TEXT("现在没有 run 在跑：先 gt.run（可带 breakpoints / from / to / pauseOnFail）");
			return false;
		}
		if (Action == TEXT("pause"))
		{
			Flow->Pause();
		}
		else if (Action == TEXT("resume"))
		{
			Flow->Resume();
		}
		else if (Action == TEXT("step"))
		{
			bool bInto = false;
			InArgs->TryGetBoolField(TEXT("into"), bInto);
			Flow->Step(bInto);
		}
		else if (Action == TEXT("breakpoints"))
		{
			TSet<FString> Ids;
			if (!ReadIds(InArgs->TryGetField(TEXT("ids")), Ids, OutError))
			{
				return false;
			}
			Flow->SetBreakpoints(Ids);
		}
		else
		{
			OutError = FString::Printf(TEXT("gt.debug 不认识 action=%s（可用：state / pause / resume / step / breakpoints）"), *Action);
			return false;
		}
		OutResult = StateJson(*Subsystem);
		return true;
	}

	bool HasScriptResolve(const TSharedPtr<FJsonObject>& InArgs)
	{
		return InArgs.IsValid() && (InArgs->HasField(TEXT("conds")) || InArgs->HasField(TEXT("exprs")));
	}

	bool ResolveScript(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		using namespace GameTestRpcDebugLocal;
		UGameTestSubsystem* Subsystem = UGameTestSubsystem::Get();
		if (Subsystem == nullptr)
		{
			OutError = TEXT("GameTest 子系统还没就绪（引擎在启动或退出中），稍后再试");
			return false;
		}

		// run 在跑（多半是调试暂停中）：用它的上下文，看得到此刻的变量；否则临时造一个
		FTempContext Temp;
		FGameTestFlowContext* Context = nullptr;
		FGameTestFlow* Flow = Subsystem->GetFlow();
		if (Flow != nullptr && !Flow->IsFinished())
		{
			Context = &Flow->GetContext();
		}
		else
		{
			FGameTestRunConfig Config;
			FString ConfigError;
			if (GameTestRunConfig::Load(Config, ConfigError))
			{
				Temp.Context.Options = Config.ScriptOptions;
			}
			Temp.Context.World = GameTestWorld::FindGameWorld();
			Temp.Context.Handles = Subsystem->GetHandles();
			Temp.Context.ScenarioRoot = UGameTestSettings::ResolveScenarioRootDir();
			Temp.Context.NowSeconds = GameTestClock::Now();
			Temp.bPushed = Temp.Context.PushScript(FString());
			Context = &Temp.Context;
		}
		const TSharedPtr<FJsonObject>* Vars = nullptr;
		if (InArgs->TryGetObjectField(TEXT("vars"), Vars) && Vars && Vars->IsValid())
		{
			// 只为这一次核对设的变量：在跑的 run 里也只放进块局部，看完就弹掉，不改 run 的变量
			Context->PushLocals();
			for (const auto& Pair : (*Vars)->Values)
			{
				Context->SetLocal(GAMETEST_JSON_KEY(Pair.Key), FGameTestValue::FromJson(Pair.Value));
			}
		}

		FGameTestFlowContext::FReadOnlyScope ReadOnly(*Context);
		OutResult = MakeShared<FJsonObject>();
		auto ReadItems = [&InArgs](const TCHAR* InField, const TCHAR* InKey, TFunctionRef<void(const FString&, const TSharedPtr<FJsonValue>&)> InEach)
		{
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!InArgs->TryGetArrayField(InField, Items) || !Items)
			{
				return;
			}
			for (int32 Index = 0; Index < Items->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>* Item = nullptr;
				if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Item) || !Item || !Item->IsValid())
				{
					continue;
				}
				FString Id;
				if (!(*Item)->TryGetStringField(TEXT("id"), Id))
				{
					Id = FString::FromInt(Index);
				}
				InEach(Id, (*Item)->TryGetField(InKey));
			}
		};

		TArray<TSharedPtr<FJsonValue>> Conds;
		ReadItems(TEXT("conds"), TEXT("cond"), [&Conds, Context](const FString& InId, const TSharedPtr<FJsonValue>& InCond)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("id"), InId);
			const TSharedPtr<FJsonObject> Cond = GameTestCond::AsObject(InCond);
			if (!Cond.IsValid())
			{
				Item->SetBoolField(TEXT("holds"), false);
				Item->SetStringField(TEXT("error"), TEXT("cond 要写成条件对象"));
			}
			else
			{
				// 只看这一刻：不稳住
				const FGameTestCondResult Result = GameTestCond::Evaluate(*Cond, *Context, nullptr);
				const TSharedPtr<FJsonObject> Json = Result.ToJson();
				for (const auto& Pair : Json->Values)
				{
					Item->SetField(GAMETEST_JSON_KEY(Pair.Key), Pair.Value);
				}
				Item->SetBoolField(TEXT("holds"), Result.Holds());
			}
			Conds.Add(MakeShared<FJsonValueObject>(Item));
		});
		TArray<TSharedPtr<FJsonValue>> Exprs;
		ReadItems(TEXT("exprs"), TEXT("expr"), [&Exprs, Context](const FString& InId, const TSharedPtr<FJsonValue>& InExpr)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("id"), InId);
			FString Text;
			FString Error;
			const TSharedPtr<const FGameTestExpr> Expr = InExpr.IsValid() && InExpr->TryGetString(Text) ? GameTestExprCache::Get(Text, Error) : nullptr;
			if (!Expr.IsValid())
			{
				Item->SetStringField(TEXT("error"), Error.IsEmpty() ? TEXT("expr 要写成表达式字符串") : Error);
			}
			else
			{
				const FGameTestValue Value = Expr->Evaluate(*Context);
				if (Value.IsUnresolved())
				{
					Item->SetStringField(TEXT("unresolved"), Value.GetString());
					Item->SetStringField(TEXT("code"), Value.GetCode().ToString());
				}
				else
				{
					Item->SetField(TEXT("value"), Value.ToJson());
					Item->SetStringField(TEXT("type"), Value.TypeName());
				}
			}
			Exprs.Add(MakeShared<FJsonValueObject>(Item));
		});
		if (Vars && Vars->IsValid())
		{
			Context->PopLocals();
		}
		OutResult->SetArrayField(TEXT("conds"), Conds);
		OutResult->SetArrayField(TEXT("exprs"), Exprs);
		return true;
	}

	bool IsScriptStep(const FJsonObject& InStep)
	{
		FString Action;
		InStep.TryGetStringField(TEXT("action"), Action);
		return GameTestBlocks::FindFor(InStep, FName(*Action)) != nullptr;
	}
}
