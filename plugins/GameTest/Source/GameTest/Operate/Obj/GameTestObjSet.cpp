// Copyright Epic Games, Inc. All Rights Reserved.

// obj.set（docs/game-test-ref.md 5.3）：设对象（或 part）的属性。
// 控件状态走控件自己的 setter，让 OnCheckStateChanged、OnValueChanged 这些委托照常触发（直接改属性值界面不会刷新，游戏逻辑也收不到）；
// 其余反射写入（UELink 的 AgentPanel::FromJson：JSON 转不了的字符串按 ImportText）或 Lua 字段赋值。

#include "AgentPanelBridgeUtil.h"
#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestRunner.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Lua/GameTestLuaBridge.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRefResolve.h"
#include "Operate/Obj/GameTestObjActions.h"

#include "Components/CheckBox.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/MultiLineEditableText.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/RichTextBlock.h"
#include "Components/Slider.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"

namespace GameTestObjSetImpl
{
	bool NameIs(const FString& InName, std::initializer_list<const TCHAR*> InNames)
	{
		for (const TCHAR* Name : InNames)
		{
			if (InName.Equals(Name, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool ToBool(const FGameTestValue& InValue, bool& OutBool)
	{
		if (InValue.Type == EGameTestValueType::Bool || InValue.Type == EGameTestValueType::Number)
		{
			OutBool = InValue.Truthy();
			return true;
		}
		const FString Text = InValue.GetString();
		if (NameIs(Text, { TEXT("true"), TEXT("Checked") }))
		{
			OutBool = true;
			return true;
		}
		if (NameIs(Text, { TEXT("false"), TEXT("Unchecked") }))
		{
			OutBool = false;
			return true;
		}
		return false;
	}

	/**
	 * 控件状态的 setter（委托照常触发）。`bOutHandled` = 这个名字是不是控件状态；是但值不对时返回 false 并写原因。
	 * 名字按引擎的属性名和常说的叫法都认（IsChecked / CheckedState、IsEnabled / bIsEnabled）。
	 */
	bool SetWidgetState(UWidget* InWidget, const FString& InName, const FGameTestValue& InValue, bool& bOutHandled, FString& OutError)
	{
		bOutHandled = true;
		if (NameIs(InName, { TEXT("Visibility") }))
		{
			const UEnum* Enum = StaticEnum<ESlateVisibility>();
			const int64 Raw = Enum != nullptr ? Enum->GetValueByNameString(InValue.ToDisplay(40)) : INDEX_NONE;
			if (Raw == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Visibility 要写 Visible / Collapsed / Hidden / HitTestInvisible / SelfHitTestInvisible，写的是 %s"), *InValue.ToDisplay(40));
				return false;
			}
			InWidget->SetVisibility(static_cast<ESlateVisibility>(Raw));
			return true;
		}
		if (NameIs(InName, { TEXT("IsEnabled"), TEXT("bIsEnabled"), TEXT("Enabled") }))
		{
			bool bEnabled = false;
			if (!ToBool(InValue, bEnabled))
			{
				OutError = TEXT("IsEnabled 要写 true / false");
				return false;
			}
			InWidget->SetIsEnabled(bEnabled);
			return true;
		}
		if (UCheckBox* CheckBox = Cast<UCheckBox>(InWidget))
		{
			if (NameIs(InName, { TEXT("IsChecked"), TEXT("CheckedState"), TEXT("Checked") }))
			{
				const FString Text = InValue.ToDisplay(40);
				if (NameIs(Text, { TEXT("Undetermined") }))
				{
					CheckBox->SetCheckedState(ECheckBoxState::Undetermined);
					return true;
				}
				bool bChecked = false;
				if (!ToBool(InValue, bChecked))
				{
					OutError = TEXT("勾选状态要写 true / false 或 Checked / Unchecked / Undetermined");
					return false;
				}
				// SetIsChecked 不广播 OnCheckStateChanged：和点一下复选框一样，改完自己广播
				CheckBox->SetIsChecked(bChecked);
				CheckBox->OnCheckStateChanged.Broadcast(bChecked);
				return true;
			}
		}
		if (NameIs(InName, { TEXT("Text") }))
		{
			const FText Text = FText::FromString(InValue.Type == EGameTestValueType::String ? InValue.GetString() : InValue.ToDisplay(4000));
			if (UTextBlock* Block = Cast<UTextBlock>(InWidget)) { Block->SetText(Text); return true; }
			if (URichTextBlock* Rich = Cast<URichTextBlock>(InWidget)) { Rich->SetText(Text); return true; }
			if (UEditableTextBox* Box = Cast<UEditableTextBox>(InWidget)) { Box->SetText(Text); return true; }
			if (UEditableText* Edit = Cast<UEditableText>(InWidget)) { Edit->SetText(Text); return true; }
			if (UMultiLineEditableTextBox* MultiBox = Cast<UMultiLineEditableTextBox>(InWidget)) { MultiBox->SetText(Text); return true; }
			if (UMultiLineEditableText* Multi = Cast<UMultiLineEditableText>(InWidget)) { Multi->SetText(Text); return true; }
		}
		if (NameIs(InName, { TEXT("Value") }) && InValue.Type == EGameTestValueType::Number)
		{
			const float Number = static_cast<float>(InValue.GetNumber());
			if (USlider* Slider = Cast<USlider>(InWidget))
			{
				// 和 set 动作的滑条同一种做法（GameTestOps）
				Slider->SetValue(Number);
				return true;
			}
			if (USpinBox* Spin = Cast<USpinBox>(InWidget))
			{
				// SetValue 不广播 OnValueChanged（GameTestOps 里实测过），补一次
				Spin->SetValue(Number);
				Spin->OnValueChanged.Broadcast(Number);
				return true;
			}
		}
		bOutHandled = false;
		return false;
	}

	/** 路径是 `@/子控件.属性`（或 `@.属性`）且落在控件上：返回控件和属性名，走 setter。 */
	UWidget* WidgetForSetter(UObject* InObject, const FGameTestPropPath& InPath, FString& OutName)
	{
		const int32 Children = InPath.NumChildren();
		if (InPath.bItem || InPath.HasState() || InPath.Segs.Num() != Children + 1 || InPath.Segs.Last().Kind != EGameTestPathSeg::Prop)
		{
			return nullptr;
		}
		OutName = InPath.Segs.Last().Name;
		FString Ignored;
		return Cast<UWidget>(GameTestPropRead::FindPart(InObject, InPath.ChildPart(), Ignored));
	}

	EGameTestStepStatus Fail(FGameTestStepContext& InContext, FName InCode, const FString& InMessage)
	{
		InContext.Message = FString::Printf(TEXT("%s：%s"), *InCode.ToString(), *InMessage);
		InContext.Payload->SetStringField(TEXT("code"), InCode.ToString());
		return GameTestHandlers::StepFailed;
	}

	EGameTestStepStatus Set(FGameTestStepContext& InContext, UObject* InObject, const FString& InProp, const TSharedPtr<FJsonValue>& InValueJson)
	{
		FGameTestFlowContext& Flow = *InContext.Flow;
		const FGameTestValue Value = GameTestRefs::EvalJson(InValueJson, Flow);
		if (Value.IsUnresolved())
		{
			return Fail(InContext, Value.GetCode(), FString::Printf(TEXT("value 取不到：%s"), *Value.GetString()));
		}
		FGameTestPropPath Path;
		FString Error;
		if (!FGameTestPropPath::Parse(InProp, Path, Error))
		{
			return Fail(InContext, Path.FailCode.IsNone() ? FName(TEXT("path_syntax")) : Path.FailCode, Error);
		}
		if (Path.HasState())
		{
			return Fail(InContext, TEXT("prop_readonly"), FString::Printf(TEXT("%s 是运行状态，只能读；要改就写它背后的属性（如 @/子控件.Visibility）"), *InProp));
		}
		const FString Shown = Value.ToDisplay(80);

		FString SetterName;
		if (UWidget* Widget = WidgetForSetter(InObject, Path, SetterName))
		{
			bool bHandled = false;
			if (SetWidgetState(Widget, SetterName, Value, bHandled, Error))
			{
				InContext.Message = FString::Printf(TEXT("已把 %s 的 %s 设成 %s"), *GameTestRefResolve::DisplayName(InObject), *InProp, *Shown);
				return GameTestHandlers::StepSucceeded;
			}
			if (bHandled)
			{
				return Fail(InContext, TEXT("op_type"), Error);
			}
		}

		const FGameTestPropRead Read = GameTestPropRead::Read(InObject, Path, Flow);
		if (!Read.IsResolved())
		{
			return Fail(InContext, Read.Value.GetCode(), Read.Value.GetString());
		}
		if (Read.Src == TEXT("lua"))
		{
			UObject* Holder = Read.Holder.Get();
			const FString LuaPath = Read.PropName.StartsWith(TEXT("lua.")) ? Read.PropName.Mid(4) : Read.PropName;
			if (Holder == nullptr || !GameTestLuaBridge::Write(Flow.World, Holder, GameTestLuaBridge::SplitPath(LuaPath), Value.ToJson(), Error))
			{
				return Fail(InContext, TEXT("prop_readonly"), FString::Printf(TEXT("Lua 字段 %s 写不进去：%s"), *LuaPath, *Error));
			}
			InContext.Message = FString::Printf(TEXT("已把 %s 的 %s 设成 %s"), *GameTestRefResolve::DisplayName(InObject), *InProp, *Shown);
			return GameTestHandlers::StepSucceeded;
		}
		if (Read.bReadOnly || Read.RawProp == nullptr || Read.RawValue == nullptr)
		{
			return Fail(InContext, TEXT("prop_readonly"), FString::Printf(TEXT("%s 是只读的（%s）"), *InProp, *Read.Type));
		}
		if (!AgentPanel::FromJson(const_cast<FProperty*>(Read.RawProp), Read.RawValue, Value.ToJson(), Error))
		{
			return Fail(InContext, TEXT("op_type"), FString::Printf(TEXT("%s（%s）写不进 %s：%s"), *InProp, *Read.Type, *Shown, *Error));
		}
		InContext.Message = FString::Printf(TEXT("已把 %s 的 %s 设成 %s"), *GameTestRefResolve::DisplayName(InObject), *InProp, *Shown);
		return GameTestHandlers::StepSucceeded;
	}
}

namespace GameTestHandlers
{
	EGameTestStepStatus HandleObjSet(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		FString Prop;
		Params.TryGetStringField(TEXT("prop"), Prop);
		const TSharedPtr<FJsonValue> Value = Params.TryGetField(TEXT("value"));
		if (Prop.IsEmpty() || !Value.IsValid())
		{
			InContext.Message = TEXT("obj.set 要写 prop（属性路径）和 value");
			return StepFailed;
		}
		return GameTestObjActions::RunWhenFound(InContext, FString::Printf(TEXT("设 %s"), *Prop), [Prop, Value](FGameTestStepContext& InStep, UObject* InObject)
		{
			return GameTestObjSetImpl::Set(InStep, InObject, Prop, Value);
		});
	}
}
