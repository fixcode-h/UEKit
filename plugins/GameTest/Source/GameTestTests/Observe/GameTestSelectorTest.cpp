// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Observe/GameTestSelector.h"
#include "Observe/GameTestWidgetNode.h"

#if WITH_AUTOMATION_WORKER

namespace
{
	TSharedPtr<FGameTestWidgetNode> MakeNode(
		const FString& InName,
		const FString& InClass,
		const FString& InPath,
		const FString& InText = FString())
	{
		const TSharedPtr<FGameTestWidgetNode> Node = MakeShared<FGameTestWidgetNode>();
		Node->Name = InName;
		Node->ClassName = InClass;
		Node->Path = InPath;
		Node->Text = InText;
		return Node;
	}

	/** 两棵屏：主菜单（有重名的两个按钮）与 HUD（也有一个同名按钮，用来验"全局搜索歧义"）。 */
	void BuildTestScreens(TArray<TSharedPtr<FGameTestWidgetNode>>& OutScreens)
	{
		const TSharedPtr<FGameTestWidgetNode> Menu = MakeNode(TEXT("WBP_MainMenu_C"), TEXT("WBP_MainMenu"), TEXT("WBP_MainMenu_C"));
		const TSharedPtr<FGameTestWidgetNode> MenuRoot = MakeNode(TEXT("Root"), TEXT("SafeZone"), TEXT("WBP_MainMenu_C/Root"));
		const TSharedPtr<FGameTestWidgetNode> MenuStack = MakeNode(TEXT("MenuStack"), TEXT("Overlay"), TEXT("WBP_MainMenu_C/Root/MenuStack"));
		const TSharedPtr<FGameTestWidgetNode> BtnStart = MakeNode(TEXT("BtnStart"), TEXT("Button"), TEXT("WBP_MainMenu_C/Root/MenuStack/BtnStart"), TEXT("启动"));
		const TSharedPtr<FGameTestWidgetNode> BtnStart2 = MakeNode(TEXT("BtnStartCopy"), TEXT("Button"), TEXT("WBP_MainMenu_C/Root/MenuStack/BtnStartCopy"), TEXT("启动副本"));
		const TSharedPtr<FGameTestWidgetNode> Hint = MakeNode(TEXT("Hint"), TEXT("TextBlock"), TEXT("WBP_MainMenu_C/Root/MenuStack/Hint"), TEXT("按任意键"));

		MenuStack->Children.Add(BtnStart);
		MenuStack->Children.Add(BtnStart2);
		MenuStack->Children.Add(Hint);
		MenuRoot->Children.Add(MenuStack);
		Menu->Children.Add(MenuRoot);

		const TSharedPtr<FGameTestWidgetNode> Hud = MakeNode(TEXT("WBP_HUD_C"), TEXT("WBP_HUD"), TEXT("WBP_HUD_C"));
		const TSharedPtr<FGameTestWidgetNode> HudRoot = MakeNode(TEXT("Root"), TEXT("CanvasPanel"), TEXT("WBP_HUD_C/Root"));
		// 故意做成与主菜单同形状的路径：用来验"不写屏名时多屏命中要报歧义"。
		const TSharedPtr<FGameTestWidgetNode> HudStack = MakeNode(TEXT("MenuStack"), TEXT("Overlay"), TEXT("WBP_HUD_C/Root/MenuStack"));
		const TSharedPtr<FGameTestWidgetNode> HudButton = MakeNode(TEXT("BtnStart"), TEXT("Button"), TEXT("WBP_HUD_C/Root/MenuStack/BtnStart"), TEXT("开始"));
		HudStack->Children.Add(HudButton);
		HudRoot->Children.Add(HudStack);
		Hud->Children.Add(HudRoot);

		OutScreens.Add(Menu);
		OutScreens.Add(Hud);
	}
}

TEST_CLASS_WITH_FLAGS(GameTestSelectorTest, "GameTest.Selector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	TEST_METHOD(Parse_RecognizesHandleReference)
	{
		GameTestSelector::FParsed Parsed;
		FString Error;
		ASSERT_THAT(IsTrue(GameTestSelector::Parse(TEXT("@btnStart"), Parsed, Error), *Error));
		ASSERT_THAT(IsTrue(Parsed.bHandle, TEXT("@ 开头应识别为句柄")));
		ASSERT_THAT(IsTrue(Parsed.Handle == TEXT("btnStart"), TEXT("句柄名应剥掉 @")));
	}

	TEST_METHOD(Parse_RejectsScreenWithoutPath)
	{
		GameTestSelector::FParsed Parsed;
		FString Error;
		ASSERT_THAT(IsFalse(GameTestSelector::Parse(TEXT("WBP_MainMenu:"), Parsed, Error)));
		ASSERT_THAT(IsTrue(Error.Contains(TEXT("路径")), *Error));
	}

	TEST_METHOD(Parse_SplitsScreenAndPath)
	{
		GameTestSelector::FParsed Parsed;
		FString Error;
		ASSERT_THAT(IsTrue(GameTestSelector::Parse(TEXT("WBP_MainMenu:Root/MenuStack/BtnStart"), Parsed, Error), *Error));
		ASSERT_THAT(IsFalse(Parsed.bAnyScreen, TEXT("写了屏名就不该是 anyScreen")));
		ASSERT_THAT(IsTrue(Parsed.Screen == TEXT("WBP_MainMenu"), *Parsed.Screen));
		ASSERT_THAT(IsTrue(FString::Join(Parsed.Segments, TEXT("/")) == TEXT("Root/MenuStack/BtnStart"),
			*FString::Join(Parsed.Segments, TEXT("/"))));
	}

	TEST_METHOD(Parse_WithoutScreenIsAnyScreenSearch)
	{
		GameTestSelector::FParsed Parsed;
		FString Error;
		ASSERT_THAT(IsTrue(GameTestSelector::Parse(TEXT("Root/MenuStack/BtnStart"), Parsed, Error), *Error));
		ASSERT_THAT(IsTrue(Parsed.bAnyScreen, TEXT("没有冒号时应是全局搜索")));
	}

	TEST_METHOD(Resolve_FindsUniqueWidgetByPath)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		BuildTestScreens(Screens);

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		ASSERT_THAT(IsTrue(GameTestSelector::ResolveText(Screens, TEXT("WBP_MainMenu:Root/MenuStack/BtnStart"), Node, Error), *Error));
		ASSERT_THAT(IsTrue(Node->Name == TEXT("BtnStart"), *Node->Name));
		ASSERT_THAT(IsTrue(Node->Text == TEXT("启动"), *Node->Text));
	}

	TEST_METHOD(Resolve_ScreenNameIgnoresCase)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		BuildTestScreens(Screens);

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		ASSERT_THAT(IsTrue(GameTestSelector::ResolveText(Screens, TEXT("wbp_tk_mainmenu:Root"), Node, Error), *Error));
		ASSERT_THAT(IsTrue(Node->Name == TEXT("Root"), *Node->Name));
	}

	TEST_METHOD(Resolve_ReportsCandidatesForWrongSegment)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		BuildTestScreens(Screens);

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		ASSERT_THAT(IsFalse(GameTestSelector::ResolveText(Screens, TEXT("WBP_MainMenu:Root/MenuStack/NoSuchButton"), Node, Error)));
		ASSERT_THAT(IsTrue(Error.Contains(TEXT("BtnStart")), *Error));
		ASSERT_THAT(IsTrue(Error.Contains(TEXT("候选")), *Error));
	}

	TEST_METHOD(Resolve_ClassNameWithMultipleMatchesIsAmbiguous)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		BuildTestScreens(Screens);

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		ASSERT_THAT(IsFalse(GameTestSelector::ResolveText(Screens, TEXT("WBP_MainMenu:Root/MenuStack/#Button"), Node, Error)));
		ASSERT_THAT(IsTrue(Error.Contains(TEXT("命中")), *Error));
	}

	TEST_METHOD(Resolve_ClassNameWithIndexPicksNth)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		BuildTestScreens(Screens);

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		ASSERT_THAT(IsTrue(GameTestSelector::ResolveText(Screens, TEXT("WBP_MainMenu:Root/MenuStack/#Button#1"), Node, Error), *Error));
		ASSERT_THAT(IsTrue(Node->Name == TEXT("BtnStartCopy"), *Node->Name));
	}

	TEST_METHOD(Resolve_MatchesByTextSubstring)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		BuildTestScreens(Screens);

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		ASSERT_THAT(IsTrue(GameTestSelector::ResolveText(Screens, TEXT("WBP_MainMenu:Root/MenuStack/text=按任意键"), Node, Error), *Error));
		ASSERT_THAT(IsTrue(Node->Name == TEXT("Hint"), *Node->Name));
	}

	TEST_METHOD(Resolve_DoubleDotSegmentSearchesDescendants)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		BuildTestScreens(Screens);

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		ASSERT_THAT(IsTrue(GameTestSelector::ResolveText(Screens, TEXT("WBP_MainMenu:Root/../Hint"), Node, Error), *Error));
		ASSERT_THAT(IsTrue(Node->Name == TEXT("Hint"), *Node->Name));
	}

	TEST_METHOD(Resolve_SamePathInMultipleScreensSuggestsScreenName)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		BuildTestScreens(Screens);

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		ASSERT_THAT(IsFalse(GameTestSelector::ResolveText(Screens, TEXT("*:Root/MenuStack/BtnStart"), Node, Error)));
		ASSERT_THAT(IsTrue(Error.Contains(TEXT("屏名")), *Error));
	}

	TEST_METHOD(Resolve_UnknownScreenListsLiveScreens)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		BuildTestScreens(Screens);

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		ASSERT_THAT(IsFalse(GameTestSelector::ResolveText(Screens, TEXT("WBP_NoSuchScreen:Root"), Node, Error)));
		ASSERT_THAT(IsTrue(Error.Contains(TEXT("WBP_MainMenu")), *Error));
	}

	TEST_METHOD(Parse_SplitsNestedHopsAndSlateSegments)
	{
		GameTestSelector::FParsed Parsed;
		FString Error;
		ASSERT_THAT(IsTrue(GameTestSelector::Parse(TEXT("WBP_Shop:Root/ItemList[3]>Root/BtnBuy>>SButton[text=确定]"), Parsed, Error), *Error));
		ASSERT_THAT(IsTrue(Parsed.Screen == TEXT("WBP_Shop"), *Parsed.Screen));
		ASSERT_THAT(IsTrue(FString::Join(Parsed.Segments, TEXT("/")) == TEXT("Root/ItemList[3]"), *FString::Join(Parsed.Segments, TEXT("/"))));
		ASSERT_THAT(AreEqual(1, Parsed.NestedHops.Num(), TEXT("> 之后应有一层嵌套")));
		ASSERT_THAT(IsTrue(FString::Join(Parsed.NestedHops[0], TEXT("/")) == TEXT("Root/BtnBuy"), TEXT("嵌套层的路径")));
		ASSERT_THAT(AreEqual(1, Parsed.SlateSegments.Num(), TEXT(">> 之后应有一段 Slate")));
		ASSERT_THAT(IsFalse(Parsed.bSlateOnly, TEXT("有 UMG 锚点时不是纯 Slate")));
	}

	TEST_METHOD(Parse_LeadingDoubleArrowIsSlateOnly)
	{
		GameTestSelector::FParsed Parsed;
		FString Error;
		ASSERT_THAT(IsTrue(GameTestSelector::Parse(TEXT(">>STableRow[text=简单]"), Parsed, Error), *Error));
		ASSERT_THAT(IsTrue(Parsed.bSlateOnly, TEXT(">> 开头应是纯 Slate")));
	}

	TEST_METHOD(Parse_TextSegmentWithColonKeepsWholeText)
	{
		GameTestSelector::FParsed Parsed;
		FString Error;
		// 冒号出现在 text= 之后时不是屏名分隔符。
		ASSERT_THAT(IsTrue(GameTestSelector::Parse(TEXT("text=时间:10"), Parsed, Error), *Error));
		ASSERT_THAT(IsTrue(Parsed.bAnyScreen, TEXT("没有屏名")));
	}

	TEST_METHOD(Resolve_DotSegmentIsScreenItself)
	{
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		BuildTestScreens(Screens);

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		ASSERT_THAT(IsTrue(GameTestSelector::ResolveText(Screens, TEXT("WBP_HUD:."), Node, Error), *Error));
		ASSERT_THAT(IsTrue(Node->Name == TEXT("WBP_HUD_C"), *Node->Name));
	}
};

#endif	// WITH_AUTOMATION_WORKER
