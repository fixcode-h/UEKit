// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/Blocks/GameTestBlockUtil.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"

/**
 * 控制块：`break` / `continue`（出最内层循环 / 进下一轮）、`return`（结束当前脚本，带 `value` 回 call）、`fail`（主动失败）。
 * 都可以带 `cond`：此刻成立才生效（不稳住——要稳住就套一个 if）。
 * 信号往外传由执行器做：一层层出块，直到有块接住（loop 接 break / continue，call 接 return，到顶的 return 结束这次 run）。
 */
namespace
{
	struct FSignalData : FGameTestNodeData
	{
		TSharedPtr<FJsonObject> Cond;
		TSharedPtr<FJsonValue> Value;
		FString Message;
	};

	/** cond 没写算成立；写了按此刻的判断。 */
	bool CondHolds(const FSignalData* InData, FGameTestFlowContext& InContext, FString& OutSummary)
	{
		if (!InData || !InData->Cond.IsValid())
		{
			return true;
		}
		FGameTestFlowContext::FReadOnlyScope ReadOnly(InContext);
		const FGameTestCondResult Result = GameTestCond::Evaluate(*InData->Cond, InContext);
		OutSummary = Result.Summary();
		return Result.Holds();
	}

	class FBlockSignal : public IGameTestBlock
	{
	public:
		FBlockSignal(const TCHAR* InKey, EGameTestFlowSignal InSignal, const TCHAR* InSummary)
			: Key(InKey)
			, Signal(InSignal)
			, Summary(InSummary)
		{
		}

		virtual FName GetKey() const override { return Key; }
		virtual FString GetSummary() const override { return Summary; }
		virtual bool IsContainer() const override { return false; }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> LoopFields = { TEXT("cond") };
			static const TArray<FString> ReturnFields = { TEXT("cond"), TEXT("value") };
			return Signal == EGameTestFlowSignal::Return ? ReturnFields : LoopFields;
		}

		virtual void Parse(FGameTestNode& InOutNode, FGameTestScriptParser& InParser) const override
		{
			TSharedRef<FSignalData> Data = MakeShared<FSignalData>();
			Data->Cond = GameTestBlockUtil::ParseCond(InParser, InOutNode, TEXT("cond"), false);
			if (Signal == EGameTestFlowSignal::Return && InOutNode.Json->HasField(TEXT("value")))
			{
				Data->Value = InOutNode.Json->TryGetField(TEXT("value"));
				InParser.CheckValue(Data->Value, TEXT("value"));
			}
			InOutNode.Data = Data;
		}

		virtual EGameTestBlockStep Tick(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext, double InNowSeconds, FGameTestBlockTick& Out) const override
		{
			const FSignalData* Data = InNode.GetData<FSignalData>();
			FString CondSay;
			if (!CondHolds(Data, InContext, CondSay))
			{
				Out.Message = FString::Printf(TEXT("条件不成立，不%s（%s）"), *Key.ToString(), *CondSay);
				return EGameTestBlockStep::Done;
			}
			if (Signal == EGameTestFlowSignal::Return)
			{
				FGameTestValue Value = FGameTestValue::Null();
				if (Data && Data->Value.IsValid())
				{
					Value = GameTestRefs::EvalJson(Data->Value, InContext);
					if (Value.IsUnresolved())
					{
						Out.Message = FString::Printf(TEXT("return 的 value 取不到：%s"), *Value.GetString());
						return EGameTestBlockStep::Failed;
					}
				}
				InContext.GetScope().ReturnValue = Value;
				Out.Message = Value.IsNull() ? TEXT("return") : FString::Printf(TEXT("return %s"), *Value.ToDisplay());
			}
			else
			{
				Out.Message = Key.ToString();
			}
			Out.Signal = Signal;
			return EGameTestBlockStep::Signal;
		}

	private:
		FName Key;
		EGameTestFlowSignal Signal;
		FString Summary;
	};

	class FBlockFail : public IGameTestBlock
	{
	public:
		virtual FName GetKey() const override { return TEXT("fail"); }
		virtual FString GetSummary() const override { return TEXT("主动失败：message 写原因（可以用表达式）"); }
		virtual bool IsContainer() const override { return false; }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("message"), TEXT("cond") };
			return Fields;
		}

		virtual void Parse(FGameTestNode& InOutNode, FGameTestScriptParser& InParser) const override
		{
			TSharedRef<FSignalData> Data = MakeShared<FSignalData>();
			Data->Cond = GameTestBlockUtil::ParseCond(InParser, InOutNode, TEXT("cond"), false);
			if (!InOutNode.Json->TryGetStringField(TEXT("message"), Data->Message) || Data->Message.IsEmpty())
			{
				InParser.Sink().Error(TEXT("bad_type"), TEXT("fail 缺少 message（失败原因，给人看）"));
			}
			InParser.CheckValue(InOutNode.Json->TryGetField(TEXT("message")), TEXT("message"));
			InOutNode.Data = Data;
		}

		virtual EGameTestBlockStep Tick(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext, double InNowSeconds, FGameTestBlockTick& Out) const override
		{
			const FSignalData* Data = InNode.GetData<FSignalData>();
			FString Summary;
			if (!CondHolds(Data, InContext, Summary))
			{
				Out.Message = FString::Printf(TEXT("条件不成立，不失败（%s）"), *Summary);
				return EGameTestBlockStep::Done;
			}
			FString Message = Data ? Data->Message : FString();
			FString Error;
			if (Data && !GameTestRefs::EvalText(Data->Message, InContext, Message, Error))
			{
				// 消息里的引用取不到也照样失败，原样带上没求值的文字和原因。
				Message = FString::Printf(TEXT("%s（消息里的引用取不到：%s）"), *Data->Message, *Error);
			}
			Out.Message = Message.IsEmpty() ? TEXT("fail") : Message;
			return EGameTestBlockStep::Failed;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterBlockSignals()
	{
		GameTestBlocks::Get().Register(MakeShared<FBlockSignal>(TEXT("break"), EGameTestFlowSignal::Break, TEXT("出最内层循环")));
		GameTestBlocks::Get().Register(MakeShared<FBlockSignal>(TEXT("continue"), EGameTestFlowSignal::Continue, TEXT("进循环的下一轮")));
		GameTestBlocks::Get().Register(MakeShared<FBlockSignal>(TEXT("return"), EGameTestFlowSignal::Return, TEXT("结束当前脚本，value 带回 call 的 saveAs")));
		GameTestBlocks::Get().Register(MakeShared<FBlockFail>());
	}
}
