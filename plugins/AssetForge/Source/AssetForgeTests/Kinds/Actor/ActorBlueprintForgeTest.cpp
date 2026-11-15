// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Kinds/Actor/ActorBlueprintForge.h"
#include "Registry/ForgeKindRegistry.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgeTypes.h"

#if WITH_AUTOMATION_WORKER

/**
 * Actor 蓝图处理器（actor-blueprint）的守门用例。
 *
 * 测的是「这个 kind 才知道、通用层给不了」的判定：
 *
 * - 父类必须能解析、且是 AActor 的派生——非 Actor 蓝图各有自己的 kind；
 * - 组件类必须能解析成真正的组件（写错不能静默通过）；
 * - 组件属性名写错要被指出。
 *
 * 走 dry-run 与纯函数：不建包、不写盘，用例不依赖任何已有资产——
 * 「导出的组件树能不能重新导入」由端到端回环负责（见 skill `tk-actor-blueprint-config`）。
 */
TEST_CLASS_WITH_FLAGS(ActorBlueprintForgeTest, "AssetForge.Actor",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 造一份最小文档，只换 parent 与 children 两段；asset 指向必定不存在的路径——dry-run 不碰盘。 */
	static bool ParseDocument(const FString& InParent, const FString& InChildren, FForgeDocument& OutDocument)
	{
		const FString Text = FString::Printf(TEXT(R"({
			"kind": "actor-blueprint",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "/Game/Test/Actor/ForgeProbe",
			"parent": "%s",
			"root": {
				"class": "ActorComponents",
				"name": "Components",
				"children": %s
			}
		})"), *InParent, *InChildren);

		TSharedPtr<FJsonObject> Json;
		FString Error;
		if (!ForgeJsonText::ParseObject(Text, Json, Error))
		{
			return false;
		}

		TArray<FForgeDiagnostic> Diagnostics;
		return ForgeDoc::Parse(Json, TEXT("test.json"), OutDocument, Diagnostics);
	}

	static FString MakeComponent(const FString& InClass, const FString& InName, const FString& InProps = TEXT("{}"))
	{
		return FString::Printf(TEXT(R"({ "class": "%s", "name": "%s", "props": %s })"), *InClass, *InName, *InProps);
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
		ASSERT_THAT(IsTrue(ForgeKindRegistry::GetSupportedKinds().Contains(ActorBlueprintForge::Kind),
			TEXT("actor-blueprint 必须出现在支持的 kind 名单里（窗口与校验都读它）")));
	}

	TEST_METHOD(找不到的父类被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(TEXT("/Script/Engine.NoSuchClass"), TEXT("[]"), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(ActorBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("找不到的父类必须报错，不能猜")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("parent"), EForgeSeverity::Error),
			TEXT("诊断应指向 parent")));
	}

	TEST_METHOD(非_AActor_的父类被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(TEXT("/Script/Engine.ActorComponent"), TEXT("[]"), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(ActorBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("非 Actor 蓝图各有自己的 kind，必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("parent"), EForgeSeverity::Error),
			TEXT("诊断应指向 parent")));
	}

	TEST_METHOD(组件类找不到被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			TEXT("/Script/Engine.Actor"),
			FString::Printf(TEXT("[ %s ]"), *MakeComponent(TEXT("/Script/Engine.NoSuchComponent"), TEXT("Body"))),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(ActorBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("找不到的组件类必须报错")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root/Body.class"), EForgeSeverity::Error),
			TEXT("诊断应指向该组件（路径按 root/<变量名> 拼）")));
	}

	TEST_METHOD(组件属性名写错被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			TEXT("/Script/Engine.Actor"),
			FString::Printf(TEXT("[ %s ]"),
				*MakeComponent(TEXT("/Script/Engine.SceneComponent"), TEXT("Root"), TEXT(R"({ "NoSuchProperty": 1 })"))),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(ActorBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("写错的属性名不能静默通过")));
		ASSERT_THAT(IsTrue(Report.Diagnostics.Num() > 0, TEXT("至少给出一条诊断")));
	}

	TEST_METHOD(合法组件树在_dry_run_下给出计划)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			TEXT("/Script/Engine.Actor"),
			FString::Printf(TEXT(R"([ { "class": "/Script/Engine.SceneComponent", "name": "Root",
				"children": [ %s ] } ])"),
				*MakeComponent(TEXT("/Script/Engine.StaticMeshComponent"), TEXT("Body"),
					TEXT(R"({ "RelativeScale3D": { "X": 1, "Y": 1, "Z": 2 } })"))),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsTrue(ActorBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("合法组件树应给出计划，而不是报错")));
		ASSERT_THAT(IsTrue(!Report.Actions.IsEmpty(),
			TEXT("目标蓝图不存在 → 计划里应有创建动作")));
	}
};

#endif // WITH_AUTOMATION_WORKER
