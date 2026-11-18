// Copyright Epic Games, Inc. All Rights Reserved.

#include "Components/Widget.h"
#include "Core/Expr/GameTestExprFunction.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Observe/GameTestCondition.h"
#include "Observe/GameTestWidgetNode.h"

/**
 * 表达式里看控件：`text(sel)`、`number(sel)`、`visible(sel)`、`enabled(sel)`、`exists(sel)`、`onScreen(sel)`、
 * `checked(sel)`、`count(sel)`。和判断（GameTestCheckWidget）走同一个读取（GameTestWidgetProbe），结论一致。
 *
 * 控件不在时：`exists` / `visible` / `onScreen` 回 false（不在本身就是答案），别的回 Unresolved（读不到）。
 */
namespace
{
	using FArgs = TArray<FGameTestValue>;

	using FRead = FGameTestValue (*)(const FGameTestWidgetNode&);

	FString SelectorOf(const FGameTestValue& InArg)
	{
		return InArg.IsString() ? InArg.GetString() : InArg.ToDisplay(1000);
	}

	/** 解析到控件就读，没解析到给 `InMissing`（Unresolved 表示读不到）。 */
	void AddReader(const TCHAR* InName, const TCHAR* InSummary, FRead InRead, bool bFalseWhenMissing)
	{
		GameTestExprFunctions::Add(InName, 1, 1, TEXT("控件"), *FString::Printf(TEXT("%s(sel)"), InName), InSummary,
			[InRead, bFalseWhenMissing](const FArgs& A, FGameTestFlowContext& Context)
			{
				const FGameTestWidgetLook Look = GameTestWidgetProbe::Look(SelectorOf(A[0]), Context);
				if (!Look.bResolved)
				{
					return bFalseWhenMissing ? FGameTestValue::FromBool(false) : FGameTestValue::Unresolved(Look.Error);
				}
				return InRead(*Look.Node);
			});
	}

	FGameTestValue ReadText(const FGameTestWidgetNode& InNode)
	{
		return FGameTestValue::FromString(GameTestCondition::ReadText(InNode));
	}

	FGameTestValue ReadNumber(const FGameTestWidgetNode& InNode)
	{
		const FString Text = GameTestCondition::ReadText(InNode);
		double Number = 0.0;
		return GameTestWidgetProbe::NumberInText(Text, Number) ? FGameTestValue::FromNumber(Number)
			: FGameTestValue::Unresolved(FString::Printf(TEXT("文字「%s」里没有数字"), *Text.Left(40)));
	}

	FGameTestValue ReadVisible(const FGameTestWidgetNode& InNode)
	{
		return FGameTestValue::FromBool(GameTestWidgetProbe::IsOnScreen(InNode));
	}

	FGameTestValue ReadEnabled(const FGameTestWidgetNode& InNode)
	{
		return FGameTestValue::FromBool(InNode.bEnabled);
	}

	FGameTestValue ReadExists(const FGameTestWidgetNode&)
	{
		return FGameTestValue::FromBool(true);
	}

	FGameTestValue ReadChecked(const FGameTestWidgetNode& InNode)
	{
		bool bChecked = false;
		return GameTestCondition::ReadChecked(InNode, bChecked) ? FGameTestValue::FromBool(bChecked)
			: FGameTestValue::Unresolved(TEXT("这个控件不是能勾选的控件"));
	}

	FGameTestValue ReadCount(const FGameTestWidgetNode& InNode)
	{
		return FGameTestValue::FromNumber(GameTestWidgetProbe::CountOf(InNode));
	}
}

namespace GameTestBuiltin
{
	void RegisterExprWidget()
	{
		AddReader(TEXT("text"), TEXT("控件显示的文字"), &ReadText, false);
		AddReader(TEXT("number"), TEXT("控件文字里的第一个数（认「1,234」「1.2万」）"), &ReadNumber, false);
		AddReader(TEXT("visible"), TEXT("控件在屏上看得见（整条父链可见、有尺寸）；不在算 false"), &ReadVisible, true);
		AddReader(TEXT("enabled"), TEXT("控件可交互"), &ReadEnabled, false);
		AddReader(TEXT("exists"), TEXT("控件找得到"), &ReadExists, true);
		AddReader(TEXT("checked"), TEXT("勾选 / 选中状态"), &ReadChecked, false);
		AddReader(TEXT("count"), TEXT("列表条目数（含没滚到的），其余按子控件数"), &ReadCount, false);
		// onScreen 看的是界面（屏）：按继承认，同一个界面几个实例有一个在屏上就算。
		GameTestExprFunctions::Add(TEXT("onScreen"), 1, 1, TEXT("控件"), TEXT("onScreen(界面)"), TEXT("界面在屏上（按继承 UUserWidget 认，子类也算）"),
			[](const FArgs& A, FGameTestFlowContext& Context)
			{
				FString Screen = SelectorOf(A[0]);
				const FString FromSelector = GameTestWidgetProbe::ScreenOf(Screen);
				Screen = FromSelector.IsEmpty() ? Screen : FromSelector;
				return FGameTestValue::FromBool(GameTestWidgetProbe::IsScreenShown(Screen, Context.World));
			});
	}
}
