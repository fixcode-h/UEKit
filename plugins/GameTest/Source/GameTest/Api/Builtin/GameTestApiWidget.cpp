// Copyright Epic Games, Inc. All Rights Reserved.

// 自带接口：界面（协议第 8.1 节 Widget.*）。读控件一律走 GameTestWidgetProbe（判断、表达式函数同一份），
// 「界面在不在」按继承 UUserWidget 认，子类也算，不按名字猜。

#include "Api/Builtin/GameTestApiArgs.h"
#include "Api/GameTestApi.h"
#include "Blueprint/UserWidget.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Observe/GameTestCondition.h"
#include "Observe/GameTestSlate.h"
#include "Observe/GameTestWidgetNode.h"
#include "Observe/Sources/GameTestObjectLocator.h"
#include "Observe/Sources/GameTestObjectRoots.h"
#include "Observe/Sources/GameTestValueSource.h"
#include "Operate/GameTestOps.h"

namespace GA = GameTestApiArgs;
using GameTestApiArgs::Opt;
using GameTestApiArgs::Req;

namespace
{
	using FCall = FGameTestApiCall;
	using FCtx = FGameTestFlowContext;
	using EState = EGameTestCallState;

	/** 选择器带屏名和路径（`WBP_X:Root/Btn`、`@句柄`、`$item>Btn`）；只写类名时按界面类认。 */
	bool LooksLikeSelector(const FString& InText)
	{
		return InText.Contains(TEXT(":")) || InText.Contains(TEXT(">")) || InText.StartsWith(TEXT("@")) || InText.StartsWith(TEXT("$"));
	}

	/** 解析控件参数；解析不到时写错误（selector_missing）并返回空。 */
	TSharedPtr<FGameTestWidgetNode> NeedNode(FCall& C, int32 InIndex, FCtx& X)
	{
		const FGameTestWidgetLook Look = GA::Widget(C.Arg(InIndex), X);
		if (!Look.bResolved || !Look.Node.IsValid())
		{
			C.Fail(TEXT("selector_missing"), Look.Error.IsEmpty() ? FString::Printf(TEXT("控件 %s 不在"), *Look.Target) : Look.Error);
			return nullptr;
		}
		return Look.Node;
	}

	APlayerController* LocalPc(FCtx& X)
	{
		FString Ignore;
		return X.World ? Cast<APlayerController>(GameTestObjectRoots::Player(*X.World, TEXT("pc"), Ignore)) : nullptr;
	}

	/** 界面能不能接住点击：可见且不是「自己和子控件都不接点击」。 */
	bool IsInteractive(const UUserWidget& InWidget)
	{
		const ESlateVisibility Visibility = InWidget.GetVisibility();
		return InWidget.IsVisible() && Visibility != ESlateVisibility::HitTestInvisible;
	}

	EState IsShown(FCall& C, FCtx& X)
	{
		const FString Target = GA::Text(C.Arg(0));
		if (LooksLikeSelector(Target) || C.Arg(0).Type == EGameTestValueType::Map)
		{
			const FGameTestWidgetLook Look = GA::Widget(C.Arg(0), X);
			return C.Done(FGameTestValue::FromBool(Look.bResolved && Look.Node.IsValid() && GameTestWidgetProbe::IsOnScreen(*Look.Node)));
		}
		if (GA::World(X, C) == nullptr)
		{
			return EState::Failed;
		}
		return C.Done(FGameTestValue::FromBool(GameTestWidgetProbe::IsScreenShown(Target, X.World)));
	}

	EState FindScreen(FCall& C, FCtx& X)
	{
		if (GA::World(X, C) == nullptr)
		{
			return EState::Failed;
		}
		const FString ClassName = GA::Text(C.Arg(0));
		const FGameTestValue& Key = C.Arg(1);
		TArray<UUserWidget*> Screens;
		GameTestWidgetProbe::ListScreens(X.World, Screens);
		int32 Matched = 0;
		for (UUserWidget* Screen : Screens)
		{
			if (Screen == nullptr || !GameTestWidgetProbe::ClassMatches(Screen->GetClass(), ClassName))
			{
				continue;
			}
			++Matched;
			bool bKeyOk = true;
			for (const TPair<FString, FGameTestValue>& Pair : Key.GetMap())
			{
				if (!GameTestObjectLocate::ReadKey(Screen, Pair.Key, X).LooseEquals(Pair.Value))
				{
					bKeyOk = false;
					break;
				}
			}
			if (bKeyOk)
			{
				// 屏上同类界面开了几个时取最上面的（ListScreens 已按 ZOrder 从上到下排）
				return C.Done(FGameTestValue::FromObject(Screen));
			}
		}
		return C.Fail(TEXT("object_not_found"), Matched == 0
			? FString::Printf(TEXT("屏上没有 %s 界面（按继承认，子类也算）"), *ClassName)
			: FString::Printf(TEXT("屏上有 %d 个 %s，但 key 都对不上"), Matched, *ClassName));
	}

	EState List(FCall& C, FCtx& X)
	{
		if (GA::World(X, C) == nullptr)
		{
			return EState::Failed;
		}
		TArray<UUserWidget*> Screens;
		GameTestWidgetProbe::ListScreens(X.World, Screens);
		TArray<FGameTestValue> Items;
		for (int32 Index = 0; Index < Screens.Num(); ++Index)
		{
			if (UUserWidget* Screen = Screens[Index])
			{
				TMap<FString, FGameTestValue> Item;
				Item.Add(TEXT("class"), FGameTestValue::FromString(Screen->GetClass()->GetName()));
				Item.Add(TEXT("name"), FGameTestValue::FromString(Screen->GetName()));
				// z：从上往下第几层（0 = 最上面），比原始 ZOrder 好用：脚本只关心谁盖住谁
				Item.Add(TEXT("z"), FGameTestValue::FromNumber(Index));
				Item.Add(TEXT("visible"), FGameTestValue::FromBool(Screen->IsVisible()));
				Item.Add(TEXT("obj"), FGameTestValue::FromObject(Screen));
				Items.Add(FGameTestValue::FromMap(MoveTemp(Item)));
			}
		}
		return C.Done(FGameTestValue::FromArray(MoveTemp(Items)));
	}

	EState Top(FCall& C, FCtx& X)
	{
		if (GA::World(X, C) == nullptr)
		{
			return EState::Failed;
		}
		TArray<UUserWidget*> Screens;
		GameTestWidgetProbe::ListScreens(X.World, Screens);
		for (UUserWidget* Screen : Screens)
		{
			if (Screen != nullptr && IsInteractive(*Screen))
			{
				return C.Done(FGameTestValue::FromObject(Screen));
			}
		}
		return C.Done(FGameTestValue::Null());
	}

	EState Focused(FCall& C, FCtx&)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return C.Done(FGameTestValue::Null());
		}
		const TSharedPtr<SWidget> Focus = FSlateApplication::Get().GetKeyboardFocusedWidget();
		return C.Done(FGameTestValue::FromObject(Focus.IsValid() ? GameTestSlate::FindOwningWidget(Focus) : nullptr));
	}

	EState State(FCall& C, FCtx& X)
	{
		const FGameTestWidgetLook Look = GA::Widget(C.Arg(0), X);
		TMap<FString, FGameTestValue> Out;
		const bool bExists = Look.bResolved && Look.Node.IsValid();
		Out.Add(TEXT("exists"), FGameTestValue::FromBool(bExists));
		if (bExists)
		{
			const FGameTestWidgetNode& Node = *Look.Node;
			Out.Add(TEXT("visible"), FGameTestValue::FromBool(GameTestWidgetProbe::IsOnScreen(Node)));
			Out.Add(TEXT("enabled"), FGameTestValue::FromBool(Node.bEnabled));
			Out.Add(TEXT("text"), FGameTestValue::FromString(GameTestCondition::ReadText(Node)));
			bool bChecked = false;
			if (GameTestCondition::ReadChecked(Node, bChecked))
			{
				Out.Add(TEXT("checked"), FGameTestValue::FromBool(bChecked));
			}
			bool bSelected = false;
			if (Node.State.IsValid() && Node.State->TryGetBoolField(TEXT("selected"), bSelected))
			{
				Out.Add(TEXT("selected"), FGameTestValue::FromBool(bSelected));
			}
		}
		else
		{
			Out.Add(TEXT("visible"), FGameTestValue::FromBool(false));
		}
		return C.Done(FGameTestValue::FromMap(MoveTemp(Out)));
	}

	EState ReadItems(FCall& C, FCtx& X, bool bCountOnly)
	{
		const TSharedPtr<FGameTestWidgetNode> Node = NeedNode(C, 0, X);
		if (!Node.IsValid())
		{
			return EState::Failed;
		}
		if (bCountOnly)
		{
			return C.Done(FGameTestValue::FromNumber(GameTestWidgetProbe::CountOf(*Node)));
		}
		TArray<FGameTestValue> Out;
		if (Node->Items.Num() > 0)
		{
			// 列表族：数据对象（含没滚到的条目），和 loop each 的 $item 一致
			for (const TSharedPtr<FGameTestListItem>& Item : Node->Items)
			{
				Out.Add(FGameTestValue::FromObject(Item.IsValid() ? Item->Item.Get() : nullptr));
			}
		}
		else
		{
			for (const TSharedPtr<FGameTestWidgetNode>& Child : Node->Children)
			{
				Out.Add(FGameTestValue::FromObject(Child.IsValid() ? Child->Widget.Get() : nullptr));
			}
		}
		return C.Done(FGameTestValue::FromArray(MoveTemp(Out)));
	}

	/** 控件所在的界面：选到的是界面本身就是它，否则往外找。 */
	UUserWidget* OwningScreen(UWidget* InWidget)
	{
		if (UUserWidget* AsUser = Cast<UUserWidget>(InWidget))
		{
			return AsUser;
		}
		return InWidget ? InWidget->GetTypedOuter<UUserWidget>() : nullptr;
	}

	EState Close(FCall& C, FCtx& X)
	{
		const TSharedPtr<FGameTestWidgetNode> Node = NeedNode(C, 0, X);
		if (!Node.IsValid())
		{
			return EState::Failed;
		}
		FString Message;
		if (!GameTestOps::RemoveWidget(OwningScreen(Node->Widget.Get()), Message))
		{
			return C.Fail(TEXT("selector_missing"), Message);
		}
		return C.Done(FGameTestValue::Null());
	}

	EState CallScreenFn(FCall& C, FCtx& X)
	{
		const TSharedPtr<FGameTestWidgetNode> Node = NeedNode(C, 0, X);
		if (!Node.IsValid())
		{
			return EState::Failed;
		}
		UUserWidget* Screen = OwningScreen(Node->Widget.Get());
		if (Screen == nullptr)
		{
			return C.Fail(TEXT("selector_missing"), TEXT("控件已经不在了"));
		}
		TArray<FGameTestValue> Args;
		if (C.Arg(2).Type == EGameTestValueType::Array)
		{
			Args = C.Arg(2).GetArray();
		}
		else if (!C.Arg(2).IsNull())
		{
			Args.Add(C.Arg(2));
		}
		// 反射函数（蓝图函数、自定义事件）和绑的 Lua 实例表上的函数都认，顺序同表达式 obj.Fn()
		const FGameTestValue Result = GameTestPath::CallMethod(FGameTestValue::FromObject(Screen), GA::Text(C.Arg(1)), Args, X);
		return Result.IsUnresolved() ? C.Fail(Result.GetCode(), Result.GetString()) : C.Done(Result);
	}

	EState InputMode(FCall& C, FCtx& X)
	{
		APlayerController* Controller = LocalPc(X);
		if (Controller == nullptr)
		{
			return C.Fail(TEXT("engine_unavailable"), TEXT("还没有本地 PlayerController（游戏没开、或还在加载）"));
		}
		// 5.4 没有公开「当前输入模式」的读取：按 SetInputMode 实际改的两处推断——
		// UIOnly 让视口忽略输入；GameAndUI / UIOnly 一般都开光标。
		const ULocalPlayer* Player = Controller->GetLocalPlayer();
		// IgnoreInput() 不是 const 成员（5.4 / 5.8 都是），这里只能拿非 const 指针
		UGameViewportClient* Viewport = Player ? Player->ViewportClient.Get() : nullptr;
		const bool bIgnoreInput = Viewport != nullptr && Viewport->IgnoreInput();
		const FString Mode = bIgnoreInput ? TEXT("ui") : (Controller->ShouldShowMouseCursor() ? TEXT("gameAndUi") : TEXT("game"));
		return C.Done(FGameTestValue::FromString(Mode));
	}
}

namespace GameTestBuiltin
{
	void RegisterApiWidget()
	{
		const TCHAR* Cat = TEXT("界面");
		GameTestApis::Add(TEXT("Widget.IsShown"), Cat, TEXT("界面或控件在不在屏上：写类名按继承 UUserWidget 认（子类也算），写选择器看那个控件"),
			{ Req(TEXT("target"), TEXT("sel"), TEXT("WBP_Shop、UMyBaseWidget 或 WBP_Shop:Root/BtnBuy")) }, TEXT("bool"), true, &IsShown);
		GameTestApis::Add(TEXT("Widget.Find"), Cat, TEXT("屏上的某个界面（同类开了几个取最上面的；key 按属性过滤）"),
			{ Req(TEXT("cls"), TEXT("cls")), Opt(TEXT("key"), TEXT("any"), TEXT("{属性路径: 值}")) }, TEXT("obj"), true, &FindScreen);
		GameTestApis::Add(TEXT("Widget.List"), Cat, TEXT("屏上所有界面，从上到下（z = 0 是最上面）"),
			{}, TEXT("[{class, name, z, visible, obj}]"), true, &List);
		GameTestApis::Add(TEXT("Widget.Top"), Cat, TEXT("最上层可见、能接点击的界面；没有时为空"), {}, TEXT("obj"), true, &Top);
		GameTestApis::Add(TEXT("Widget.Focused"), Cat, TEXT("当前键盘焦点所在的控件；没有时为空"), {}, TEXT("obj"), true, &Focused);
		GameTestApis::Add(TEXT("Widget.State"), Cat, TEXT("控件此刻的状态；控件不在时 exists 为 false，不算失败"),
			{ Req(TEXT("target"), TEXT("sel")) }, TEXT("{exists, visible, enabled, text, checked, selected}"), true, &State);
		GameTestApis::Add(TEXT("Widget.Text"), Cat, TEXT("控件的文字（容器取里面的文字）"),
			{ Req(TEXT("target"), TEXT("sel")) }, TEXT("string"), true,
			[](FCall& C, FCtx& X)
			{
				const TSharedPtr<FGameTestWidgetNode> Node = NeedNode(C, 0, X);
				return Node ? C.Done(FGameTestValue::FromString(GameTestCondition::ReadText(*Node))) : EState::Failed;
			});
		GameTestApis::Add(TEXT("Widget.Number"), Cat, TEXT("控件文字里的第一个数（认 1,234、1.2万、15K）"),
			{ Req(TEXT("target"), TEXT("sel")) }, TEXT("number"), true,
			[](FCall& C, FCtx& X)
			{
				const TSharedPtr<FGameTestWidgetNode> Node = NeedNode(C, 0, X);
				if (!Node)
				{
					return EState::Failed;
				}
				const FString Text = GameTestCondition::ReadText(*Node);
				double Number = 0.0;
				return GameTestWidgetProbe::NumberInText(Text, Number) ? C.Done(FGameTestValue::FromNumber(Number))
					: C.Fail(TEXT("expr_type"), FString::Printf(TEXT("文字「%s」里没有数字"), *Text.Left(40)));
			});
		GameTestApis::Add(TEXT("Widget.Items"), Cat, TEXT("列表的条目（ListView 按数据对象，含没滚到的；别的容器按子控件）"),
			{ Req(TEXT("target"), TEXT("sel")) }, TEXT("[obj]"), true, [](FCall& C, FCtx& X) { return ReadItems(C, X, false); });
		GameTestApis::Add(TEXT("Widget.ItemCount"), Cat, TEXT("列表的条目数（含没滚到的）"),
			{ Req(TEXT("target"), TEXT("sel")) }, TEXT("int"), true, [](FCall& C, FCtx& X) { return ReadItems(C, X, true); });
		GameTestApis::Add(TEXT("Widget.Close"), Cat, TEXT("把控件所在的界面从屏上移除（同 widget.close）"),
			{ Req(TEXT("target"), TEXT("sel")) }, TEXT(""), false, &Close);
		GameTestApis::Add(TEXT("Widget.CallFn"), Cat, TEXT("调控件所在界面上的函数（蓝图函数或绑的 Lua 函数，同 widget.call）"),
			{ Req(TEXT("target"), TEXT("sel")), Req(TEXT("fn"), TEXT("string")), Opt(TEXT("args"), TEXT("any"), TEXT("按顺序的参数数组")) },
			TEXT("any"), false, &CallScreenFn);
		GameTestApis::Add(TEXT("Widget.InputMode"), Cat, TEXT("键盘鼠标现在给谁：game / ui / gameAndUi（按视口是否忽略输入、光标是否显示推断）"),
			{}, TEXT("string"), true, &InputMode);
		GameTestApis::Add(TEXT("Widget.CursorVisible"), Cat, TEXT("鼠标光标显示没有"), {}, TEXT("bool"), true,
			[](FCall& C, FCtx& X)
			{
				const APlayerController* Controller = LocalPc(X);
				return Controller ? C.Done(FGameTestValue::FromBool(Controller->ShouldShowMouseCursor()))
					: C.Fail(TEXT("engine_unavailable"), TEXT("还没有本地 PlayerController"));
			});
	}
}
