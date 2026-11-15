// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Registry/ForgeKindRegistry.h"
#include "Core/ForgeClassResolver.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgeTypes.h"
#include "Kinds/Object/ObjectBlueprintForge.h"

#if WITH_AUTOMATION_WORKER

/**
 * 非 Actor 蓝图处理器（object-blueprint）的守门用例。
 *
 * 测的是「这个 kind 才知道、通用层给不了」的判定：
 *
 * - 父类必须"非 Actor、可蓝图化、非抽象"——每一类拒绝都要给可操作的报错（含"该用哪个 kind"）；
 * - 本类型没有组件树：写了 `children` 必须在计划阶段拦下，而不是悄悄忽略；
 * - 属性名写错要被指出（不是静默通过）。
 *
 * 走 dry-run 与纯函数：不建包、不写盘，用例不依赖任何已有资产——
 * 「创建出来的蓝图能不能用」由端到端回环负责（见 skill `tk-object-blueprint-config`）。
 */
TEST_CLASS_WITH_FLAGS(ObjectBlueprintForgeTest, "AssetForge.Object",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 造一份最小文档，只换 parent 与 root 两段；asset 指向必定不存在的路径——dry-run 不碰盘。 */
	static bool ParseDocument(const FString& InParent, const FString& InRootBody, FForgeDocument& OutDocument)
	{
		const FString Text = FString::Printf(TEXT(R"({
			"kind": "object-blueprint",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "/Game/Test/Object/ForgeProbe",
			"parent": "%s",
			"root": %s
		})"), *InParent, *InRootBody);

		TSharedPtr<FJsonObject> Json;
		FString Error;
		if (!ForgeJsonText::ParseObject(Text, Json, Error))
		{
			return false;
		}

		TArray<FForgeDiagnostic> Diagnostics;
		return ForgeDoc::Parse(Json, TEXT("test.json"), OutDocument, Diagnostics);
	}

	static FString MakeRoot(const FString& InProps, const FString& InChildren = FString())
	{
		const FString ChildrenField = InChildren.IsEmpty()
			? FString()
			: FString::Printf(TEXT(", \"children\": %s"), *InChildren);

		return FString::Printf(TEXT(R"({
			"class": "ClassDefaults",
			"name": "ClassDefaults",
			"props": %s%s
		})"), *InProps, *ChildrenField);
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

	static bool MentionsActorBlueprint(const FForgeReport& InReport)
	{
		for (const FForgeDiagnostic& Diagnostic : InReport.Diagnostics)
		{
			if (Diagnostic.Message.Contains(TEXT("actor-blueprint")))
			{
				return true;
			}
		}
		return false;
	}

	TEST_METHOD(类型已登记在管道里)
	{
		ASSERT_THAT(IsTrue(ForgeKindRegistry::GetSupportedKinds().Contains(ObjectBlueprintForge::Kind),
			TEXT("object-blueprint 必须出现在支持的 kind 名单里（窗口与校验都读它）")));
	}

	TEST_METHOD(测试锚点类应能被解析)
	{
		FString Error;
		UClass* Resolved = FForgeClassResolver::Resolve(
			TEXT("/Script/AssetForgeTests.ForgeObjectProbe"), UObject::StaticClass(), Error);
		ASSERT_THAT(IsTrue(Resolved != nullptr,
			FString::Printf(TEXT("锚点类应能被解析，实际错误：%s"), *Error)));
	}

	TEST_METHOD(Actor_父类被拒绝并提示另一侧)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(TEXT("/Script/Engine.Actor"), MakeRoot(TEXT("{}")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(ObjectBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("Actor 派生类不该由本类型接管")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("parent"), EForgeSeverity::Error),
			TEXT("诊断应指向 parent")));
		ASSERT_THAT(IsTrue(MentionsActorBlueprint(Report),
			TEXT("报错应点名该改用 actor-blueprint——这是给作者的动作指引")));
	}

	TEST_METHOD(抽象父类被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(TEXT("/Script/Engine.ActorComponent"), MakeRoot(TEXT("{}")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(ObjectBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("抽象类建不出实例，必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("parent"), EForgeSeverity::Error),
			TEXT("诊断应指向 parent")));
	}

	TEST_METHOD(找不到的父类被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(TEXT("/Script/Engine.NoSuchClass"), MakeRoot(TEXT("{}")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(ObjectBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("找不到的类必须报错，不能猜")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("parent"), EForgeSeverity::Error),
			TEXT("诊断应指向 parent")));
	}

	TEST_METHOD(缺失父类被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(FString(), MakeRoot(TEXT("{}")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(ObjectBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("没有父类就没有默认值的锚点，必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("parent"), EForgeSeverity::Error),
			TEXT("诊断应指向 parent")));
	}

	TEST_METHOD(写子节点被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			TEXT("/Script/AssetForgeTests.ForgeObjectProbe"),
			MakeRoot(TEXT("{}"), TEXT(R"([ { "class": "Actor", "name": "Nope" } ])")),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(ObjectBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("本类型没有组件树：children 必须被拦下")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children"), EForgeSeverity::Error),
			TEXT("诊断应指向 children")));
	}

	TEST_METHOD(属性名写错被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			TEXT("/Script/AssetForgeTests.ForgeObjectProbe"),
			MakeRoot(TEXT(R"({ "NoSuchProperty": 1 })")),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(ObjectBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("写错的属性名不能静默通过")));
		ASSERT_THAT(IsTrue(Report.Diagnostics.Num() > 0, TEXT("至少给出一条诊断")));
	}

	TEST_METHOD(合法父类在_dry_run_下给出计划)
	{
		// 用测试锚点类而不是 GameplayEffect：默认值由本模块定，"非默认 → 有动作"因此可控。
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			TEXT("/Script/AssetForgeTests.ForgeObjectProbe"),
			MakeRoot(TEXT(R"({ "ProbeValue": 7 })")),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsTrue(ObjectBlueprintForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("合法父类与属性应给出计划，而不是报错")));
		ASSERT_THAT(IsTrue(!Report.Actions.IsEmpty(),
			TEXT("非默认属性值 → 计划里应有动作")));
	}
};

#endif // WITH_AUTOMATION_WORKER
