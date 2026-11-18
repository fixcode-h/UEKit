// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Expr/GameTestExprFunction.h"
#include "Core/Flow/Blocks/GameTestBlockUtil.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"

/**
 * `set` 变量块：`{var, value}` 或 `{var, from: {target, read}}`。
 * 没有 `var` 的 `set` 是 1.x 的控件赋值动作（叶子），由 Claims 区分，两种写法并存。
 */
namespace
{
	/** from.read 能写的值：都是表达式里的控件函数，读法和条件里完全一样。 */
	const TArray<FString>& ReadKinds()
	{
		static const TArray<FString> Kinds = { TEXT("text"), TEXT("number"), TEXT("visible"), TEXT("enabled"), TEXT("exists"), TEXT("checked"),
			TEXT("count"), TEXT("onScreen") };
		return Kinds;
	}

	struct FSetData : FGameTestNodeData
	{
		FString Var;
		TSharedPtr<FJsonValue> Value;
		FString FromTarget;
		FString FromRead = TEXT("text");
		bool bFrom = false;
	};

	class FBlockSetVar : public IGameTestBlock
	{
	public:
		virtual FName GetKey() const override { return TEXT("set.var"); }
		virtual FString GetSummary() const override { return TEXT("设变量：value（值、$引用、表达式）或 from（读控件）"); }

		virtual bool Claims(const FJsonObject& InJson, FName InAction) const override
		{
			return InAction == TEXT("set") && InJson.HasField(TEXT("var"));
		}

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("var"), TEXT("value"), TEXT("from") };
			return Fields;
		}

		virtual bool IsContainer() const override { return false; }

		virtual void Parse(FGameTestNode& InOutNode, FGameTestScriptParser& InParser) const override
		{
			TSharedRef<FSetData> Data = MakeShared<FSetData>();
			FGameTestDiagSink& Sink = InParser.Sink();
			const FJsonObject& Json = *InOutNode.Json;
			if (!Json.TryGetStringField(TEXT("var"), Data->Var) || !GameTestBlockUtil::IsValidVarName(Data->Var))
			{
				Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("var 写的「%s」不能当变量名（字母数字下划线，不以数字开头）"), *Data->Var));
			}
			const bool bValue = Json.HasField(TEXT("value"));
			const bool bFrom = Json.HasField(TEXT("from"));
			if (bValue == bFrom)
			{
				Sink.Error(TEXT("bad_type"), TEXT("set 要写 value 或 from 其中一个"));
			}
			if (bValue)
			{
				Data->Value = Json.TryGetField(TEXT("value"));
				InParser.CheckValue(Data->Value, TEXT("value"));
			}
			else if (bFrom)
			{
				Data->bFrom = true;
				FGameTestDiagSink::FScope Scope(Sink, TEXT("from"));
				const TSharedPtr<FJsonObject>* From = nullptr;
				if (!Json.TryGetObjectField(TEXT("from"), From) || !From || !From->IsValid())
				{
					Sink.Error(TEXT("bad_type"), TEXT("from 要写成 {target, read}"));
				}
				else
				{
					if (!(*From)->TryGetStringField(TEXT("target"), Data->FromTarget) || Data->FromTarget.IsEmpty())
					{
						Sink.Error(TEXT("bad_type"), TEXT("from 缺少 target（控件选择器）"));
					}
					(*From)->TryGetStringField(TEXT("read"), Data->FromRead);
					if (!ReadKinds().Contains(Data->FromRead))
					{
						Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("read 只能是 %s，写的是 %s"), *FString::Join(ReadKinds(), TEXT("、")), *Data->FromRead));
					}
					InParser.CheckValue((*From)->TryGetField(TEXT("target")), TEXT("target"));
				}
			}
			// 先登记再说：同一个 set 里 value 引用自己（`"=$n + 1"`）要求之前已经有它，上面已经按旧的变量表检查过了。
			InParser.DeclareVar(Data->Var);
			InOutNode.Data = Data;
		}

		virtual EGameTestBlockStep Tick(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext, double InNowSeconds, FGameTestBlockTick& Out) const override
		{
			const FSetData* Data = InNode.GetData<FSetData>();
			if (!Data || Data->Var.IsEmpty())
			{
				Out.Message = TEXT("set 没解析好（场景有错，先 gt.validate）");
				return EGameTestBlockStep::Failed;
			}
			FGameTestValue Value;
			if (Data->bFrom)
			{
				const FGameTestExprFunction* Fn = GameTestExprFunctions::Get().Find(FName(*Data->FromRead));
				if (!Fn || !Fn->Call)
				{
					Out.Message = FString::Printf(TEXT("读法 %s 没有登记"), *Data->FromRead);
					return EGameTestBlockStep::Failed;
				}
				FGameTestFlowContext::FReadOnlyScope ReadOnly(InContext);
				Value = Fn->Call({ FGameTestValue::FromString(Data->FromTarget) }, InContext);
			}
			else
			{
				Value = GameTestRefs::EvalJson(Data->Value, InContext);
			}
			if (Value.IsUnresolved())
			{
				// 协议第 3 节：set 里取不到让这一步失败，不悄悄写成 null。
				Out.Message = FString::Printf(TEXT("$%s 取不到值：%s"), *Data->Var, *Value.GetString());
				return EGameTestBlockStep::Failed;
			}
			InContext.SetVar(Data->Var, Value);
			Out.Message = FString::Printf(TEXT("$%s = %s"), *Data->Var, *Value.ToDisplay());
			Out.Payload = MakeShared<FJsonObject>();
			Out.Payload->SetStringField(TEXT("var"), Data->Var);
			Out.Payload->SetField(TEXT("value"), Value.ToJson());
			return EGameTestBlockStep::Done;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterBlockSetVar()
	{
		GameTestBlocks::Get().Register(MakeShared<FBlockSetVar>());
	}
}
