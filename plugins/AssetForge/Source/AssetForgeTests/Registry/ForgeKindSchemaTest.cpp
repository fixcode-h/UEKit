// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/ForgePropertySchema.h"
#include "Registry/ForgeKindRegistry.h"
#include "Registry/ForgeKindSchema.h"

#include "Dom/JsonObject.h"

#if WITH_AUTOMATION_WORKER

/**
 * forge.kinds 的 schema（Agent 写配置前取）：每个登记的 kind 都要写了形状——
 * 加 kind 时忘了这里，Agent 拿到的就是不知道 children 能不能写的通用 schema。
 */
TEST_CLASS_WITH_FLAGS(ForgeKindSchemaTest, "AssetForge.Schema",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	TEST_METHOD(每个kind都有形状)
	{
		for (const ForgeKindRegistry::FForgeKindInfo& Info : ForgeKindRegistry::GetAll())
		{
			ASSERT_THAT(IsTrue(ForgeKindSchema::HasShape(Info.Kind), FString::Printf(TEXT("%s 没写配置形状（ForgeKindSchema.cpp）"), Info.Kind)));
			const TSharedPtr<FJsonObject> Schema = ForgeKindSchema::Build(Info.Kind);
			ASSERT_THAT(IsTrue(Schema.IsValid() && Schema->HasField(TEXT("$defs")), FString::Printf(TEXT("%s 的 schema 不完整"), Info.Kind)));
		}
		ASSERT_THAT(IsFalse(ForgeKindSchema::Build(TEXT("no-such-kind")).IsValid()));
	}

	TEST_METHOD(没有子节点的kind禁止children)
	{
		const TSharedPtr<FJsonObject> Schema = ForgeKindSchema::Build(TEXT("map"));
		ASSERT_THAT(IsTrue(Schema.IsValid()));
		const TSharedPtr<FJsonObject> Root = Schema->GetObjectField(TEXT("$defs"))->GetObjectField(TEXT("root"));
		const TSharedPtr<FJsonObject> Children = Root->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("children"));
		ASSERT_THAT(AreEqual(0, static_cast<int32>(Children->GetNumberField(TEXT("maxItems")))));
	}

	TEST_METHOD(属性表按反射列出且可分页)
	{
		ForgePropertySchema::FQuery Query;
		Query.Type = TEXT("/Script/Engine.WorldSettings");
		Query.Filter = TEXT("KillZ");
		const TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		FString Error;
		ASSERT_THAT(IsTrue(ForgePropertySchema::Describe(Query, Out, Error), Error));
		const TSharedPtr<FJsonObject> Props = Out->GetObjectField(TEXT("properties"));
		ASSERT_THAT(IsTrue(Props->HasField(TEXT("KillZ")), TEXT("WorldSettings 应该有 KillZ")));
		ASSERT_THAT(AreEqual(FString(TEXT("number")), Props->GetObjectField(TEXT("KillZ"))->GetStringField(TEXT("type"))));

		Query.Type = TEXT("no_such_class_xyz");
		ASSERT_THAT(IsFalse(ForgePropertySchema::Describe(Query, MakeShared<FJsonObject>(), Error)));
		ASSERT_THAT(IsFalse(Error.IsEmpty()));
	}
};

#endif
