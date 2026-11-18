// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/ForgeDiff.h"

#if WITH_AUTOMATION_WORKER

/**
 * 增量计划的判定用例。
 *
 * 这些断言保护的是「不清空重建」这件事：计划里不该出现多余的动作，否则每改一个属性
 * 都会连带重建整棵树，动画与绑定随之失效——而且不会报错，只会悄悄坏掉。
 */
TEST_CLASS_WITH_FLAGS(ForgeDiffTest, "AssetForge.Diff",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	static FForgeNode MakeNode(const FString& InClassName, const FString& InName)
	{
		FForgeNode Node;
		Node.ClassName = InClassName;
		Node.Name = InName;
		return Node;
	}

	static const FForgePlanEntry* FindEntry(const TArray<FForgePlanEntry>& InPlan, EForgeAction InAction, const FString& InNodePath)
	{
		for (const FForgePlanEntry& Entry : InPlan)
		{
			if (Entry.Action == InAction && Entry.NodePath == InNodePath)
			{
				return &Entry;
			}
		}
		return nullptr;
	}

	static int32 CountAction(const TArray<FForgePlanEntry>& InPlan, EForgeAction InAction)
	{
		int32 Count = 0;
		for (const FForgePlanEntry& Entry : InPlan)
		{
			if (Entry.Action == InAction)
			{
				++Count;
			}
		}
		return Count;
	}

	TEST_METHOD(相同的两棵树产生空计划)
	{
		FForgeNode Target = MakeNode(TEXT("CanvasPanel"), TEXT("Root"));
		Target.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("Title")));

		FForgeNode Current = MakeNode(TEXT("CanvasPanel"), TEXT("Root"));
		Current.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("Title")));

		TArray<FForgePlanEntry> Plan;
		ForgeDiff::ComputeRootPlan(Target, Current, Plan);

		ASSERT_THAT(IsTrue(Plan.Num() == 0,
			*FString::Printf(TEXT("内容相同就不该有任何动作，实际得到 %d 条"), Plan.Num())));
	}

	/**
	 * 生成类名带不带 `_C` 指同一个类：导出写 `GetPathName()`（带 `_C`），人工手写常省略。
	 * 逐字节比会让每次回环都报「已更新」，而导入看到的是同一个类、什么都不做——
	 * 于是计划永远不收敛，还谎报改动。
	 */
	TEST_METHOD(带_C与不带_C的同一个类不算变化)
	{
		FForgeNode Target = MakeNode(TEXT("/Script/UnrealSharp.ActivatableWidgetStack"), TEXT("Root"));
		FForgeNode Current = MakeNode(TEXT("/Script/UnrealSharp.ActivatableWidgetStack_C"), TEXT("Root"));

		TArray<FForgePlanEntry> Plan;
		ForgeDiff::ComputeRootPlan(Target, Current, Plan);

		ASSERT_THAT(IsTrue(Plan.Num() == 0,
			*FString::Printf(TEXT("同一个类的两种写法不该算改动，实际得到 %d 条"), Plan.Num())));
	}

	/** 反过来：真的是两个类时必须报出来，不能被归一吃掉。 */
	TEST_METHOD(不同的类仍然产生更新)
	{
		FForgeNode Target = MakeNode(TEXT("/Script/UnrealSharp.ActivatableWidgetStack"), TEXT("Root"));
		FForgeNode Current = MakeNode(TEXT("/Script/CommonUI.CommonActivatableWidgetStack_C"), TEXT("Root"));

		TArray<FForgePlanEntry> Plan;
		ForgeDiff::ComputeRootPlan(Target, Current, Plan);

		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Update) == 1,
			TEXT("不同类必须报成一次更新")));
	}

	TEST_METHOD(新增节点只产生创建动作且不碰既有节点)
	{
		FForgeNode Target = MakeNode(TEXT("VerticalBox"), TEXT("Root"));
		Target.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("A")));
		Target.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("B")));

		FForgeNode Current = MakeNode(TEXT("VerticalBox"), TEXT("Root"));
		Current.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("A")));

		TArray<FForgePlanEntry> Plan;
		ForgeDiff::ComputeRootPlan(Target, Current, Plan);

		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Create) == 1, TEXT("只应有一条创建动作")));
		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Update) == 0,
			TEXT("既有节点内容没变，不该出现更新动作——否则每次加控件都会重建整棵树")));
		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Delete) == 0, TEXT("不该有删除动作")));
		ASSERT_THAT(IsTrue(FindEntry(Plan, EForgeAction::Create, TEXT("root/B")) != nullptr,
			TEXT("创建动作应指向 root/B")));
	}

	TEST_METHOD(配置中移除的节点产生删除动作)
	{
		FForgeNode Target = MakeNode(TEXT("VerticalBox"), TEXT("Root"));
		Target.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("A")));

		FForgeNode Current = MakeNode(TEXT("VerticalBox"), TEXT("Root"));
		Current.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("A")));
		Current.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("B")));

		TArray<FForgePlanEntry> Plan;
		ForgeDiff::ComputeRootPlan(Target, Current, Plan);

		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Delete) == 1, TEXT("多出来的节点应被计划删除")));
		ASSERT_THAT(IsTrue(FindEntry(Plan, EForgeAction::Delete, TEXT("root/B")) != nullptr,
			TEXT("删除动作应指向 root/B")));
	}

	TEST_METHOD(属性变化产生更新动作并列出字段名)
	{
		FForgeNode Target = MakeNode(TEXT("TextBlock"), TEXT("Root"));
		Target.Props = MakeShared<FJsonObject>();
		Target.Props->SetNumberField(TEXT("RenderOpacity"), 0.25);

		FForgeNode Current = MakeNode(TEXT("TextBlock"), TEXT("Root"));
		Current.Props = MakeShared<FJsonObject>();
		Current.Props->SetNumberField(TEXT("RenderOpacity"), 1.0);

		TArray<FForgePlanEntry> Plan;
		ForgeDiff::ComputeRootPlan(Target, Current, Plan);

		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Update) == 1, TEXT("应有一条更新动作")));

		const FForgePlanEntry* Entry = FindEntry(Plan, EForgeAction::Update, TEXT("root"));
		ASSERT_THAT(IsTrue(Entry != nullptr, TEXT("更新动作应指向根节点")));
		ASSERT_THAT(IsTrue(Entry != nullptr && Entry->Detail.Contains(TEXT("RenderOpacity")),
			TEXT("更新动作的说明里应点名变化了的属性——AI 靠它决定要不要细看")));
	}

	TEST_METHOD(类型变化被识别为更新)
	{
		const FForgeNode Target = MakeNode(TEXT("Image"), TEXT("Root"));
		const FForgeNode Current = MakeNode(TEXT("TextBlock"), TEXT("Root"));

		TArray<FForgePlanEntry> Plan;
		ForgeDiff::ComputeRootPlan(Target, Current, Plan);

		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Update) == 1, TEXT("换类应产生更新动作")));

		const FForgePlanEntry* Entry = FindEntry(Plan, EForgeAction::Update, TEXT("root"));
		ASSERT_THAT(IsTrue(Entry != nullptr && Entry->Detail.Contains(TEXT("TextBlock")),
			TEXT("说明里应写出旧类名，便于人确认是不是手误")));
	}

	TEST_METHOD(同级顺序变化产生重排动作)
	{
		FForgeNode Target = MakeNode(TEXT("VerticalBox"), TEXT("Root"));
		Target.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("B")));
		Target.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("A")));

		FForgeNode Current = MakeNode(TEXT("VerticalBox"), TEXT("Root"));
		Current.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("A")));
		Current.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("B")));

		TArray<FForgePlanEntry> Plan;
		ForgeDiff::ComputeRootPlan(Target, Current, Plan);

		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Reorder) == 1, TEXT("应有一条重排动作")));
		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Create) == 0, TEXT("重排不该被当成新建")));
		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Delete) == 0, TEXT("重排不该被当成删除")));
	}

	TEST_METHOD(新增节点时按名匹配而不是按位置)
	{
		// 目标在开头插了一个新节点：若按位置匹配，A 会被认成 B、全盘错位。
		FForgeNode Target = MakeNode(TEXT("VerticalBox"), TEXT("Root"));
		Target.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("New")));
		Target.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("A")));

		FForgeNode Current = MakeNode(TEXT("VerticalBox"), TEXT("Root"));
		Current.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("A")));

		TArray<FForgePlanEntry> Plan;
		ForgeDiff::ComputeRootPlan(Target, Current, Plan);

		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Delete) == 0,
			TEXT("A 在两边都存在，不该被判成删除")));
		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Create) == 1, TEXT("只该新建 New 一个")));
		ASSERT_THAT(IsTrue(FindEntry(Plan, EForgeAction::Create, TEXT("root/New")) != nullptr,
			TEXT("创建动作应指向 root/New")));
	}

	TEST_METHOD(新增节点的整棵子树都被计划为创建)
	{
		FForgeNode Target = MakeNode(TEXT("VerticalBox"), TEXT("Root"));
		FForgeNode Panel = MakeNode(TEXT("VerticalBox"), TEXT("Panel"));
		Panel.Children.Add(MakeNode(TEXT("TextBlock"), TEXT("Inner")));
		Target.Children.Add(Panel);

		const FForgeNode Current = MakeNode(TEXT("VerticalBox"), TEXT("Root"));

		TArray<FForgePlanEntry> Plan;
		ForgeDiff::ComputeRootPlan(Target, Current, Plan);

		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Create) == 2,
			TEXT("新增的子树里每个节点都应各有一条创建动作")));
		ASSERT_THAT(IsTrue(FindEntry(Plan, EForgeAction::Create, TEXT("root/Panel/Inner")) != nullptr,
			TEXT("子节点的路径应逐级拼出来")));
	}

	/** 回归：导出侧对「没有属性」产出空对象，解析侧产出 nullptr，两者必须视为相同。 */
	TEST_METHOD(空属性对象与缺少属性视为相同)
	{
		FForgeNode Target = MakeNode(TEXT("TextBlock"), TEXT("Root"));
		Target.Props = MakeShared<FJsonObject>();
		Target.Slot = MakeShared<FJsonObject>();

		const FForgeNode Current = MakeNode(TEXT("TextBlock"), TEXT("Root"));

		TArray<FForgePlanEntry> Plan;
		ForgeDiff::ComputeRootPlan(Target, Current, Plan);

		ASSERT_THAT(IsTrue(Plan.Num() == 0,
			*FString::Printf(
				TEXT("空属性集合与缺少该集合表达同一件事；判为不同会让回环把每个节点都误报为已更新（实际得到 %d 条动作）"),
				Plan.Num())));
	}

	/**
	 * 回归：属性名的大小写在导入侧是**不敏感**的（UE 的属性查找本来就如此），比较也必须跟上。
	 *
	 * 行为树的 `defaultValue` / `key` 就是这么踩到的：手写配置写小写、导出写大写，
	 * 于是一份能导入成功的配置每次都被判成「已更新」——资产反复重写却怎么都不收敛。
	 */
	TEST_METHOD(属性名大小写不同不产生动作)
	{
		FForgeNode Target = MakeNode(TEXT("BTTask_Wait"), TEXT("Wait"));
		Target.Props = MakeShared<FJsonObject>();
		Target.Props->SetNumberField(TEXT("defaultValue"), 2);
		Target.Props->SetStringField(TEXT("key"), TEXT("None"));

		FForgeNode Current = MakeNode(TEXT("BTTask_Wait"), TEXT("Wait"));
		Current.Props = MakeShared<FJsonObject>();
		Current.Props->SetNumberField(TEXT("DefaultValue"), 2);
		Current.Props->SetStringField(TEXT("Key"), TEXT("None"));

		TArray<FForgePlanEntry> Plan;
		ForgeDiff::ComputeRootPlan(Target, Current, Plan);

		ASSERT_THAT(IsTrue(Plan.Num() == 0,
			*FString::Printf(
				TEXT("大小写不同但指向同一个属性，不该产生动作（实际得到 %d 条）"),
				Plan.Num())));
	}

	/** 同样地，导入侧接受省掉 `b` 前缀的写法，比较也不能把它当成差异。 */
	TEST_METHOD(省略b前缀的属性名不产生动作)
	{
		FForgeNode Target = MakeNode(TEXT("Image"), TEXT("Icon"));
		Target.Props = MakeShared<FJsonObject>();
		Target.Props->SetBoolField(TEXT("isEnabled"), false);

		FForgeNode Current = MakeNode(TEXT("Image"), TEXT("Icon"));
		Current.Props = MakeShared<FJsonObject>();
		Current.Props->SetBoolField(TEXT("bIsEnabled"), false);

		TArray<FForgePlanEntry> Plan;
		ForgeDiff::ComputeRootPlan(Target, Current, Plan);

		ASSERT_THAT(IsTrue(Plan.Num() == 0,
			*FString::Printf(
				TEXT("`isEnabled` 与 `bIsEnabled` 指向同一个属性（实际得到 %d 条）"),
				Plan.Num())));
	}
};

#endif // WITH_AUTOMATION_WORKER
