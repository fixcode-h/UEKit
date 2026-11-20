// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Expr/GameTestExprFunction.h"
#include "Core/Flow/Blocks/GameTestBlockUtil.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Core/Script/GameTestDiag.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Ref/GameTestRef.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/**
 * `set` 变量块：`{var, value}` 或 `{var, from: {target, read}}`。2.0.0 起 `target` 是部件写法 `{obj, part}` 或引用，
 * 读法换成算出状态，经表达式的 `prop(引用, "@部件:状态")` 读（和条件同一套）。
 * 没有 `var` 的 `set` 是 1.x 的控件赋值动作（叶子），由 Claims 区分，两种写法并存。
 */
namespace
{
	/** from.read 能写的值（读法 → 算出状态，number 读 :text 再取数）。 */
	const TArray<FString>& ReadKinds()
	{
		static const TArray<FString> Kinds = { TEXT("text"), TEXT("number"), TEXT("visible"), TEXT("enabled"), TEXT("exists"), TEXT("checked"),
			TEXT("count"), TEXT("onScreen") };
		return Kinds;
	}

	FString StateOfRead(const FString& InRead)
	{
		if (InRead == TEXT("number"))
		{
			return TEXT("text");
		}
		return InRead == TEXT("visible") ? FString(TEXT("shown")) : InRead;
	}

	struct FSetData : FGameTestNodeData
	{
		FString Var;
		TSharedPtr<FJsonValue> Value;
		/** 部件写法里的引用（名字 / `$变量` / 内联对象）和部件。 */
		TSharedPtr<FJsonValue> FromObj;
		FString FromPart;
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
					FGameTestTarget Target;
					const TSharedPtr<FJsonValue> TargetValue = (*From)->TryGetField(TEXT("target"));
					FString Text;
					if (TargetValue.IsValid() && TargetValue->TryGetString(Text) && !Text.StartsWith(TEXT("$")) && !GameTestRefForm::IsName(Text))
					{
						Sink.Error(TEXT("selector_removed"), FString::Printf(TEXT("from.target「%s」是旧的选择器写法：写 {\"obj\": 引用, \"part\": \"部件名\"}"), *Text.Left(80)));
					}
					else if (!FGameTestTarget::FromJson(TargetValue, Target))
					{
						Sink.Error(TEXT("bad_type"), TEXT("from 缺少 target（部件写法 {obj, part} 或引用）"));
					}
					else
					{
						Data->FromObj = Target.Obj;
						Data->FromPart = Target.Part;
					}
					(*From)->TryGetStringField(TEXT("read"), Data->FromRead);
					if (!ReadKinds().Contains(Data->FromRead))
					{
						Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("read 只能是 %s，写的是 %s"), *FString::Join(ReadKinds(), TEXT("、")), *Data->FromRead));
					}
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
				const FGameTestExprFunction* Fn = GameTestExprFunctions::Get().Find(TEXT("prop"));
				if (!Fn || !Fn->Call || !Data->FromObj.IsValid())
				{
					Out.Message = TEXT("set from 没解析好（场景有错，先 gt.validate）");
					return EGameTestBlockStep::Failed;
				}
				// prop() 的引用参数：名字 / $变量 原样给，内联引用给 JSON 文字
				FString RefText;
				if (!Data->FromObj->TryGetString(RefText))
				{
					const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&RefText);
					TArray<TSharedPtr<FJsonValue>> Wrap;
					Wrap.Add(Data->FromObj);
					FJsonSerializer::Serialize(Wrap, Writer);
					RefText = RefText.Mid(1, RefText.Len() - 2);
				}
				FGameTestValue RefArg = FGameTestValue::FromString(RefText);
				if (RefText.StartsWith(TEXT("$")))
				{
					RefArg = GameTestRefs::EvalJson(Data->FromObj, InContext);
				}
				const FString State = TEXT(":") + StateOfRead(Data->FromRead);
				const FString Path = Data->FromPart.IsEmpty() ? State : TEXT("@") + Data->FromPart + State;
				FGameTestFlowContext::FReadOnlyScope ReadOnly(InContext);
				Value = Fn->Call({ RefArg, FGameTestValue::FromString(Path) }, InContext);
				if (Data->FromRead == TEXT("number") && !Value.IsUnresolved())
				{
					// 按 1.x 的 number 规则：从文字里取第一个数
					const FString Shown = Value.ToDisplay(200);
					FString Digits;
					bool bStarted = false;
					for (const TCHAR C : Shown)
					{
						if (FChar::IsDigit(C) || (C == TEXT('.') && bStarted) || (C == TEXT('-') && !bStarted))
						{
							Digits.AppendChar(C);
							bStarted = bStarted || FChar::IsDigit(C);
						}
						else if (bStarted)
						{
							break;
						}
						else
						{
							Digits.Reset();
						}
					}
					Value = bStarted ? FGameTestValue::FromNumber(FCString::Atod(*Digits)) : FGameTestValue::Unresolved(FString::Printf(TEXT("「%s」里没有数字"), *Shown));
				}
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
