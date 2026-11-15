// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"
#include "ForgeTestAssets.h"

#include "Registry/ForgeKindRegistry.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgeTypes.h"
#include "Kinds/Physics/PhysicsAssetForge.h"

#if WITH_AUTOMATION_WORKER

/**
 * 物理资产（纳管型 kind）的守门用例：与 anim-sequence 同款的三条——
 * 类型已登记、资产必须已存在、字段名写错在动资产之前被拦下。
 */
TEST_CLASS_WITH_FLAGS(PhysicsAssetForgeTest, "AssetForge.PhysicsAsset",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	static bool ParseDocument(const FString& InAssetPath, const FString& InProps, FForgeDocument& OutDocument)
	{
		const FString Text = FString::Printf(TEXT(R"({
			"kind": "physics-asset",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "%s",
			"root":
			{
				"class": "PhysicsAsset",
				"name": "Probe",
				"props": %s
			}
		})"), *InAssetPath, *InProps);

		TSharedPtr<FJsonObject> Json;
		FString Error;
		if (!ForgeJsonText::ParseObject(Text, Json, Error))
		{
			return false;
		}

		TArray<FForgeDiagnostic> Diagnostics;
		return ForgeDoc::Parse(Json, TEXT("test.json"), OutDocument, Diagnostics);
	}

	TEST_METHOD(类型已登记在管道里)
	{
		ASSERT_THAT(IsTrue(ForgeKindRegistry::GetSupportedKinds().Contains(PhysicsAssetForge::Kind),
			TEXT("physics-asset 必须出现在支持的 kind 名单里")));
	}

	TEST_METHOD(资产不存在时报错而不是生成)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(TEXT("/Game/No/Such/Phys_Probe"), TEXT("{}"), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(PhysicsAssetForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("纳管型 kind 找不到资产必须报错（不凭空生成）")));
	}

	TEST_METHOD(字段名写错被拒绝)
	{
		FORGE_SKIP_WITHOUT_ASSET(ForgeTestAssets::PhysicsAsset);
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			ForgeTestAssets::PhysicsAsset,
			TEXT(R"({ "NoSuchProperty": 1 })"),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(PhysicsAssetForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("写错的字段名不能静默通过")));
	}
};

#endif // WITH_AUTOMATION_WORKER
