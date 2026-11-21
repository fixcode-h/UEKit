// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/Blocks/GameTestBlockUtil.h"
#include "Core/Flow/GameTestGuards.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"

/**
 * `call`：跑一份子脚本（`script` 相对场景根，`.json` 可省），`with` 传参数，子脚本 `return` 的 `value` 存进 `saveAs`。
 *
 * - 子脚本加载时就解析好（解析器按路径缓存、查环、查深度），执行时不再读文件。
 * - 子脚本有自己的一层作用域：自己的 vars、params、objects，看不到调用方的变量（要用就 with 传）。
 * - 子脚本的 guards 在调用期间生效、和外层叠加（CollectGuards）。
 */
namespace
{
	struct FCallData : FGameTestNodeData
	{
		TSharedPtr<FGameTestScript> Script;
		TSharedPtr<FJsonObject> With;
		FString SaveAs;
	};

	enum class ECallPhase : uint8
	{
		Start,
		Setup,
		Steps,
		Finish,
	};

	struct FCallState : FGameTestBlockState
	{
		ECallPhase Phase = ECallPhase::Start;
		bool bPushed = false;
		bool bReturned = false;
		/** 子脚本里冒出来的 break / continue（没有被子脚本里的循环接住）。 */
		FString StrayError;
	};

	class FBlockCall : public IGameTestBlock
	{
	public:
		virtual FName GetKey() const override { return TEXT("call"); }
		virtual FString GetSummary() const override { return TEXT("调子脚本：with 传参数，return 的值存进 saveAs"); }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("script"), TEXT("with"), TEXT("saveAs") };
			return Fields;
		}

		virtual void Parse(FGameTestNode& InOutNode, FGameTestScriptParser& InParser) const override
		{
			TSharedRef<FCallData> Data = MakeShared<FCallData>();
			FGameTestDiagSink& Sink = InParser.Sink();
			const FJsonObject& Json = *InOutNode.Json;
			FString Script;
			if (!Json.TryGetStringField(TEXT("script"), Script) || Script.IsEmpty())
			{
				Sink.Error(TEXT("bad_type"), TEXT("call 缺少 script（子脚本相对场景根的路径）"));
			}
			else
			{
				FGameTestDiagSink::FScope Scope(Sink, TEXT("script"));
				Data->Script = InParser.LoadCall(Script);
			}

			const TSharedPtr<FJsonObject>* With = nullptr;
			if (Json.HasField(TEXT("with")))
			{
				if (!Json.TryGetObjectField(TEXT("with"), With) || !With || !With->IsValid())
				{
					Sink.Error(TEXT("bad_type"), TEXT("with 要写成对象：{参数名: 值}"));
				}
				else
				{
					Data->With = *With;
					InParser.CheckValue(Json.TryGetField(TEXT("with")), TEXT("with"));
				}
			}
			if (Data->Script.IsValid())
			{
				CheckParams(*Data->Script, Data->With, Sink);
			}

			if (Json.TryGetStringField(TEXT("saveAs"), Data->SaveAs) && !Data->SaveAs.IsEmpty())
			{
				if (!GameTestBlockUtil::IsValidVarName(Data->SaveAs))
				{
					Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("saveAs 写的「%s」不能当变量名"), *Data->SaveAs));
				}
				InParser.DeclareVar(Data->SaveAs);
			}
			InOutNode.Data = Data;
		}

		virtual TSharedRef<FGameTestBlockState> Enter(const FGameTestNode& InNode, FGameTestFlowContext& InContext) const override
		{
			return MakeShared<FCallState>();
		}

		virtual EGameTestBlockStep Tick(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext, double InNowSeconds, FGameTestBlockTick& Out) const override
		{
			FCallState& State = static_cast<FCallState&>(InState);
			const FCallData* Data = InNode.GetData<FCallData>();
			if (!Data || !Data->Script.IsValid())
			{
				Out.Message = TEXT("call 的子脚本没加载成（场景有错，先 gt.validate）");
				return EGameTestBlockStep::Failed;
			}
			const FGameTestScript& Script = *Data->Script;
			if (!State.StrayError.IsEmpty())
			{
				Out.Message = State.StrayError;
				return EGameTestBlockStep::Failed;
			}
			switch (State.Phase)
			{
			case ECallPhase::Start:
			{
				FString Error;
				if (!EnterScript(*Data, InContext, Error))
				{
					Out.Message = Error;
					return EGameTestBlockStep::Failed;
				}
				State.bPushed = true;
				State.Phase = ECallPhase::Setup;
				if (Script.Setup.Nodes.Num() > 0)
				{
					Out.ExternalList = &Script.Setup;
					return EGameTestBlockStep::RunList;
				}
				// 没有 setup：直接接着跑 steps。
				[[fallthrough]];
			}
			case ECallPhase::Setup:
				State.Phase = ECallPhase::Steps;
				Out.ExternalList = &Script.Steps;
				Out.Message = FString::Printf(TEXT("进入 %s"), *Script.Path);
				return EGameTestBlockStep::RunList;
			case ECallPhase::Steps:
			case ECallPhase::Finish:
			default:
			{
				// 先取返回值、弹出子脚本作用域，再在调用方作用域里存 saveAs。
				const FGameTestValue Returned = InContext.GetScope().ReturnValue;
				InContext.PopScript();
				State.bPushed = false;
				if (!Data->SaveAs.IsEmpty())
				{
					InContext.SetVar(Data->SaveAs, Returned);
				}
				Out.Payload = MakeShared<FJsonObject>();
				Out.Payload->SetStringField(TEXT("script"), Script.Path);
				if (State.bReturned)
				{
					Out.Payload->SetBoolField(TEXT("returned"), true);
				}
				if (!Returned.IsNull())
				{
					Out.Payload->SetField(TEXT("value"), Returned.ToJson());
				}
				Out.Message = Returned.IsNull() ? FString::Printf(TEXT("%s 跑完"), *Script.Path)
					: FString::Printf(TEXT("%s 跑完，返回 %s"), *Script.Path, *Returned.ToDisplay());
				return EGameTestBlockStep::Done;
			}
			}
		}

		virtual bool CatchSignal(EGameTestFlowSignal InSignal, const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext) const override
		{
			FCallState& State = static_cast<FCallState&>(InState);
			if (InSignal == EGameTestFlowSignal::Return)
			{
				State.bReturned = true;
				State.Phase = ECallPhase::Finish;
				return true;
			}
			// break / continue 不许穿出子脚本去影响调用方的循环：这里接住并让 call 失败，说清楚原因。
			State.StrayError = TEXT("子脚本里的 break / continue 不在循环里（不能跳出 call 去控制调用方的循环，用 return 带值回去再判断）");
			return true;
		}

		virtual void Leave(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext) const override
		{
			FCallState& State = static_cast<FCallState&>(InState);
			if (State.bPushed)
			{
				InContext.PopScript();
				State.bPushed = false;
			}
		}

		virtual void CollectGuards(const FGameTestNode& InNode, const FGameTestBlockState& InState, TArray<TSharedRef<FGameTestGuardDef>>& OutGuards) const override
		{
			const FCallState& State = static_cast<const FCallState&>(InState);
			const FCallData* Data = InNode.GetData<FCallData>();
			if (State.bPushed && Data && Data->Script.IsValid())
			{
				OutGuards.Append(Data->Script->Guards);
			}
		}

	private:
		static void CheckParams(const FGameTestScript& InScript, const TSharedPtr<FJsonObject>& InWith, FGameTestDiagSink& InSink)
		{
			TArray<FString> Names;
			for (const FGameTestParamDef& Param : InScript.Params)
			{
				Names.Add(Param.Name);
			}
			if (InWith.IsValid())
			{
				for (const auto& Pair : InWith->Values)
				{
					const FString Key = GAMETEST_JSON_KEY(Pair.Key);
					if (!Names.Contains(Key))
					{
						InSink.Error(TEXT("unknown_field"), FString::Printf(TEXT("%s 没有参数 %s（有：%s）"), *InScript.Path, *Key,
							Names.Num() > 0 ? *FString::Join(Names, TEXT(", ")) : TEXT("无")));
					}
				}
			}
			for (const FGameTestParamDef& Param : InScript.Params)
			{
				const bool bGiven = InWith.IsValid() && InWith->HasField(Param.Name);
				if (!bGiven && (!Param.Default.IsValid() || Param.Default->IsNull()))
				{
					InSink.Warning(TEXT("var_undefined"), FString::Printf(TEXT("没传参数 %s，它也没有默认值：子脚本里 $%s 是 null"), *Param.Name, *Param.Name));
				}
			}
		}

		/** with 在调用方作用域里求值，再进子脚本那一层，填参数、变量初值、对象定义。 */
		static bool EnterScript(const FCallData& InData, FGameTestFlowContext& InContext, FString& OutError)
		{
			const FGameTestScript& Script = *InData.Script;
			TMap<FString, FGameTestValue> Args;
			if (InData.With.IsValid())
			{
				for (const auto& Pair : InData.With->Values)
				{
					FGameTestValue Value = GameTestRefs::EvalJson(Pair.Value, InContext);
					if (Value.IsUnresolved())
					{
						OutError = FString::Printf(TEXT("参数 %s 取不到：%s"), *GAMETEST_JSON_KEY(Pair.Key), *Value.GetString());
						return false;
					}
					Args.Add(GAMETEST_JSON_KEY(Pair.Key), MoveTemp(Value));
				}
			}
			if (!InContext.PushScript(Script.Path))
			{
				OutError = FString::Printf(TEXT("call 嵌套超过 %d 层（运行设置 maxCallDepth）"), InContext.Options.MaxCallDepth);
				return false;
			}
			FGameTestScriptScope& Scope = InContext.GetScope();
			for (const FGameTestParamDef& Param : Script.Params)
			{
				const FGameTestValue* Given = Args.Find(Param.Name);
				Scope.Params.Add(Param.Name, Given ? *Given : FGameTestValue::FromJson(Param.Default));
			}
			if (Script.Objects.IsValid())
			{
				Scope.AddObjects(*Script.Objects);
			}
			if (Script.Vars.IsValid())
			{
				// 变量初值可以引用参数（`"gold0": "=$base * 2"`），所以在参数填好后、子脚本作用域里求。
				for (const auto& Pair : Script.Vars->Values)
				{
					FGameTestValue Value = GameTestRefs::EvalJson(Pair.Value, InContext);
					if (Value.IsUnresolved())
					{
						OutError = FString::Printf(TEXT("%s 的变量 %s 初值取不到：%s"), *Script.Path, *GAMETEST_JSON_KEY(Pair.Key), *Value.GetString());
						// 已经压进去的子脚本作用域要弹掉：失败时 bPushed 还没置位，Leave 不会替我们弹。
						InContext.PopScript();
						return false;
					}
					InContext.GetScope().Vars.Add(GAMETEST_JSON_KEY(Pair.Key), MoveTemp(Value));
				}
			}
			return true;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterBlockCall()
	{
		GameTestBlocks::Get().Register(MakeShared<FBlockCall>());
	}
}
