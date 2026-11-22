// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/Blocks/GameTestBlockUtil.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"

/**
 * `gm.run`（2.3.0）：执行一条 Agent 面板的 GM（预设、自定义 GM、宏）。
 *
 * GameTest 不读面板的 gm.json：面板存盘、运行前按 GM 库把这条 GM 展开成步骤写进 `run`，这里只跑 `run`。
 * `gm` 记着它是哪条（`{kind, name}`，报告里显示、面板按它重新展开），`args` 是这次填的参数，
 * 跑 `run` 期间当块局部名字（`$参数名`），出了这一步就看不到——和循环的 `$index` 一样，不污染脚本变量。
 * 名字 `gm` 已经是「控制台 GM 命令」那个叶子动作，所以叫 `gm.run`。
 */
namespace
{
	struct FGmData : FGameTestNodeData
	{
		FString Kind;
		FString Name;
		TSharedPtr<FJsonObject> Args;
	};

	struct FGmState : FGameTestBlockState
	{
		bool bStarted = false;
		bool bLocals = false;
	};

	class FBlockGm : public IGameTestBlock
	{
	public:
		virtual FName GetKey() const override { return TEXT("gm.run"); }
		virtual FString GetSummary() const override { return TEXT("执行一条面板的 GM：run 是面板按 GM 库展开的步骤，args 跑的时候当 $参数名"); }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("gm"), TEXT("args"), TEXT("run") };
			return Fields;
		}

		virtual void Parse(FGameTestNode& InOutNode, FGameTestScriptParser& InParser) const override
		{
			TSharedRef<FGmData> Data = MakeShared<FGmData>();
			FGameTestDiagSink& Sink = InParser.Sink();
			const FJsonObject& Json = *InOutNode.Json;
			const TSharedPtr<FJsonObject>* Gm = nullptr;
			if (!Json.TryGetObjectField(TEXT("gm"), Gm) || !Gm || !Gm->IsValid())
			{
				Sink.Error(TEXT("bad_type"), TEXT("gm.run 缺少 gm（{kind, name}：哪条 GM，面板按它重新展开 run）"));
			}
			else
			{
				(*Gm)->TryGetStringField(TEXT("kind"), Data->Kind);
				(*Gm)->TryGetStringField(TEXT("name"), Data->Name);
				if (Data->Name.IsEmpty())
				{
					Sink.Error(TEXT("bad_type"), TEXT("gm.run 的 gm 缺少 name"));
				}
			}

			const TSharedPtr<FJsonObject>* Args = nullptr;
			if (Json.HasField(TEXT("args")))
			{
				if (!Json.TryGetObjectField(TEXT("args"), Args) || !Args || !Args->IsValid())
				{
					Sink.Error(TEXT("bad_type"), TEXT("args 要写成对象：{参数名: 值}"));
				}
				else
				{
					Data->Args = *Args;
					// 参数值在外层求（可以写 $变量、= 表达式、附带 GM 里的 $ret），所以在登记参数名之前检查
					InParser.CheckValue(Json.TryGetField(TEXT("args")), TEXT("args"));
				}
			}

			// run 里能用 $参数名：只在这一步里登记，出去就没有
			InParser.PushVarScope();
			if (Data->Args.IsValid())
			{
				for (const auto& Pair : Data->Args->Values)
				{
					const FString Key = GAMETEST_JSON_KEY(Pair.Key);
					if (!GameTestBlockUtil::IsValidVarName(Key))
					{
						Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("参数名「%s」不能当变量名（run 里按 $参数名 用）"), *Key));
					}
					InParser.DeclareVar(Key);
				}
			}
			if (Json.HasField(TEXT("run")))
			{
				InParser.ParseList(InOutNode, TEXT("run"), Json.TryGetField(TEXT("run")), true);
			}
			else
			{
				Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("gm.run 缺少 run：面板没把「%s」展开成步骤（在面板里打开这个脚本存一次，或者 GM 库里已经没有这条）"), *Data->Name));
			}
			InParser.PopVarScope();
			InOutNode.Data = Data;
		}

		virtual TSharedRef<FGameTestBlockState> Enter(const FGameTestNode& InNode, FGameTestFlowContext& InContext) const override
		{
			return MakeShared<FGmState>();
		}

		virtual EGameTestBlockStep Tick(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext, double InNowSeconds, FGameTestBlockTick& Out) const override
		{
			FGmState& State = static_cast<FGmState&>(InState);
			const FGmData* Data = InNode.GetData<FGmData>();
			const FString Name = Data ? Data->Name : FString();
			if (State.bStarted)
			{
				Out.Payload = MakeShared<FJsonObject>();
				TSharedPtr<FJsonObject> Gm = MakeShared<FJsonObject>();
				if (Data && !Data->Kind.IsEmpty())
				{
					Gm->SetStringField(TEXT("kind"), Data->Kind);
				}
				Gm->SetStringField(TEXT("name"), Name);
				Out.Payload->SetObjectField(TEXT("gm"), Gm);
				Out.Message = FString::Printf(TEXT("GM「%s」跑完"), *Name);
				return EGameTestBlockStep::Done;
			}
			State.bStarted = true;
			if (InNode.Lists.Num() == 0)
			{
				Out.Message = FString::Printf(TEXT("GM「%s」没有展开的步骤（面板里打开脚本存一次再跑）"), *Name);
				return EGameTestBlockStep::Failed;
			}
			// 参数在外层作用域里求值，再压一层局部放进去：run 里的 $参数名 只在这一步里有效
			TArray<TPair<FString, FGameTestValue>> Values;
			if (Data && Data->Args.IsValid())
			{
				for (const auto& Pair : Data->Args->Values)
				{
					FGameTestValue Value = GameTestRefs::EvalJson(Pair.Value, InContext);
					if (Value.IsUnresolved())
					{
						Out.Message = FString::Printf(TEXT("GM「%s」的参数 %s 取不到：%s"), *Name, *GAMETEST_JSON_KEY(Pair.Key), *Value.GetString());
						return EGameTestBlockStep::Failed;
					}
					Values.Emplace(GAMETEST_JSON_KEY(Pair.Key), MoveTemp(Value));
				}
			}
			InContext.PushLocals();
			State.bLocals = true;
			for (const TPair<FString, FGameTestValue>& Pair : Values)
			{
				InContext.SetLocal(Pair.Key, Pair.Value);
			}
			Out.List = 0;
			Out.Message = FString::Printf(TEXT("执行 GM「%s」"), *Name);
			return EGameTestBlockStep::RunList;
		}

		virtual void Leave(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext) const override
		{
			FGmState& State = static_cast<FGmState&>(InState);
			if (State.bLocals)
			{
				InContext.PopLocals();
				State.bLocals = false;
			}
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterBlockGm()
	{
		GameTestBlocks::Get().Register(MakeShared<FBlockGm>());
	}
}
