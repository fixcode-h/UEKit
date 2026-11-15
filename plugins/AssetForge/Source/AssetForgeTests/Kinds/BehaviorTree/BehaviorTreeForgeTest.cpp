// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Kinds/BehaviorTree/BehaviorTreeForge.h"
#include "Registry/ForgeKindRegistry.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgeTypes.h"

#if WITH_AUTOMATION_WORKER

/**
 * 行为树处理器（behavior-tree）的守门用例。
 *
 * 测的是「这个 kind 才知道、通用层给不了」的判定：
 *
 * - 根节点必须是 composite（不是的话树根本挂不上子节点）；
 * - 每个节点的类必须能解析、且得是行为树节点（`UBTNode` 的派生）；
 * - 非 composite 的节点写了 `children` 要拦下，而不是悄悄忽略；
 * - 装饰器 / 服务必须是 `UBTNode` 的派生（它们也走同一套类解析）。
 *
 * 走 dry-run 与纯函数：不建包、不写盘，用例不依赖任何已有资产——
 * 「导出的树能不能重新导入」由端到端回环负责（见 skill `tk-behavior-tree-config`）。
 */
TEST_CLASS_WITH_FLAGS(BehaviorTreeForgeTest, "AssetForge.BehaviorTree",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 造一份最小文档，只换 root 一段；asset 指向必定不存在的路径——dry-run 不碰盘。 */
	static bool ParseDocument(const FString& InRootBody, FForgeDocument& OutDocument)
	{
		const FString Text = FString::Printf(TEXT(R"({
			"kind": "behavior-tree",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "/Game/Test/AI/BT_ForgeProbe",
			"parent": "/Script/AIModule.BehaviorTree",
			"root": %s
		})"), *InRootBody);

		TSharedPtr<FJsonObject> Json;
		FString Error;
		if (!ForgeJsonText::ParseObject(Text, Json, Error))
		{
			return false;
		}

		TArray<FForgeDiagnostic> Diagnostics;
		return ForgeDoc::Parse(Json, TEXT("test.json"), OutDocument, Diagnostics);
	}

	static FString MakeNode(const FString& InClass, const FString& InName, const FString& InExtra = FString())
	{
		return FString::Printf(TEXT(R"({ "class": "%s", "name": "%s"%s })"), *InClass, *InName, *InExtra);
	}

	static bool HasDiagnosticAt(const FForgeReport& InReport, const FString& InFieldPath, EForgeSeverity InSeverity)
	{
		for (const FForgeDiagnostic& Diagnostic : InReport.Diagnostics)
		{
			if (Diagnostic.FieldPath == InFieldPath && Diagnostic.Severity == InSeverity)
			{
				return true;
			}
		}
		return false;
	}

	TEST_METHOD(类型已登记在管道里)
	{
		ASSERT_THAT(IsTrue(ForgeKindRegistry::GetSupportedKinds().Contains(BehaviorTreeForge::Kind),
			TEXT("behavior-tree 必须出现在支持的 kind 名单里（窗口与校验都读它）")));
	}

	TEST_METHOD(根节点不是_composite_被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			MakeNode(TEXT("/Script/AIModule.BTTask_Wait"), TEXT("Wait")),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(BehaviorTreeForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("根节点必须是 composite（task 挂不上子节点）")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.class"), EForgeSeverity::Error),
			TEXT("诊断应指向 root.class")));
	}

	TEST_METHOD(找不到的节点类被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			FString::Printf(TEXT(R"({ "class": "/Script/AIModule.BTComposite_Selector", "name": "Selector",
				"children": [ %s ] })"),
				*MakeNode(TEXT("/Script/AIModule.NoSuchNode"), TEXT("Child"))),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(BehaviorTreeForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("找不到的节点类必须报错，不能猜")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root/Child.class"), EForgeSeverity::Error),
			TEXT("诊断应指向该节点（路径按 root/<节点名> 拼）")));
	}

	TEST_METHOD(非_composite_节点写子节点被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			FString::Printf(TEXT(R"({ "class": "/Script/AIModule.BTComposite_Selector", "name": "Selector",
				"children": [ %s ] })"),
				*MakeNode(TEXT("/Script/AIModule.BTTask_Wait"), TEXT("Wait"),
					TEXT(R"(, "children": [ { "class": "/Script/AIModule.BTTask_Wait", "name": "Nested" } ])"))),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(BehaviorTreeForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("task 不能有子节点，写了必须被拦下")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root/Wait.children"), EForgeSeverity::Error),
			TEXT("诊断应指向该节点的 children")));
	}

	TEST_METHOD(装饰器不是行为树节点被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			FString::Printf(TEXT(R"({ "class": "/Script/AIModule.BTComposite_Selector", "name": "Selector",
				"children": [ %s ] })"),
				*MakeNode(TEXT("/Script/AIModule.BTTask_Wait"), TEXT("Wait"),
					TEXT(R"(, "decorators": [ { "class": "/Script/Engine.Actor", "name": "Nope" } ])"))),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(BehaviorTreeForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("装饰器必须是 UBTNode 的派生")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root/Wait.attached.class"), EForgeSeverity::Error),
			TEXT("诊断应指向该装饰器（attached 是装饰器/服务的共用路径段）")));
	}

	TEST_METHOD(合法树在_dry_run_下给出计划)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			FString::Printf(TEXT(R"({ "class": "/Script/AIModule.BTComposite_Selector", "name": "Selector",
				"children": [ { "class": "/Script/AIModule.BTComposite_Sequence", "name": "Sequence",
					"decorators": [ { "class": "/Script/AIModule.BTDecorator_TimeLimit", "name": "Limit" } ],
					"children": [ %s ] } ] })"),
				*MakeNode(TEXT("/Script/AIModule.BTTask_Wait"), TEXT("Wait"))),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsTrue(BehaviorTreeForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("合法树应给出计划，而不是报错")));
		ASSERT_THAT(IsTrue(!Report.Actions.IsEmpty(),
			TEXT("目标资产不存在 → 计划里应有创建动作")));
	}
};

#endif // WITH_AUTOMATION_WORKER
