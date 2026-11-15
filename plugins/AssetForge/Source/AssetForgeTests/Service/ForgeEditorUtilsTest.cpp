// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Service/ForgeEditorUtils.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgeTypes.h"

#include "Dom/JsonObject.h"

// 守卫必须与 CQTest 宏的实体条件一致（CQTest.h 的注册体本身包在 WITH_AUTOMATION_WORKER 内）。
// 用 WITH_DEV_AUTOMATION_TESTS 之类的宏会让整块展开成"没注册的空壳"——测试静默消失。
#if WITH_AUTOMATION_WORKER

/**
 * 编辑器侧共享判定的守门用例。
 *
 * 配置目录约定（生成物、配置分类、导出落点）的纯函数判定。
 *
 * 重点钉住两类历史缺陷：
 * - 「生成项」三套口径中 `Contains("_schema")` 会**误杀**路径里恰好含该子串的正常配置；
 * - 命令行导出曾绕过清单按默认路径落盘，会把手工挪过位置的配置再建一份。
 */
TEST_CLASS_WITH_FLAGS(ForgeEditorUtilsTest, "AssetForge.Editor.Utils",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	static TSharedPtr<FJsonObject> MakeJsonWithKind(const FString& InKind)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("kind"), InKind);
		return Json;
	}

	/** 分类入参：文档加载结果、文档、报告、原始 JSON、支持的 kind 名单。 */
	static ForgeEditorUtils::FForgeConfigClassification Classify(
		const bool bInParsed,
		const TSharedPtr<FJsonObject>& InRawJson,
		const TArray<FString>& InSupportedKinds)
	{
		FForgeDocument Document;
		Document.Kind = TEXT("umg-widget");
		Document.Asset = TEXT("/Game/UI/WBP_Probe");
		Document.Parent = TEXT("/Script/UMG.UserWidget");
		Document.Revision = 7;

		FForgeReport Report;
		if (!bInParsed && !InRawJson.IsValid())
		{
			Report.AddError(TEXT("root"), TEXT("JSON 语法错误"));
		}

		return ForgeEditorUtils::ClassifyConfig(
			bInParsed, Document, Report, InRawJson, InSupportedKinds);
	}

	TEST_METHOD(生成项判定只看前缀不看子串)
	{
		// 插件的生成物：`_` 开头的目录与文件。
		ASSERT_THAT(IsTrue(ForgeEditorUtils::IsGeneratedEntry(TEXT("_schema")), TEXT("_schema 目录是生成物")));
		ASSERT_THAT(IsTrue(ForgeEditorUtils::IsGeneratedEntry(TEXT("_forge-state.json")), TEXT("清单文件是生成物")));

		// 正常配置不能被误杀——`Contains("_schema")` 的老写法会在这里翻车。
		ASSERT_THAT(IsFalse(ForgeEditorUtils::IsGeneratedEntry(TEXT("WBP_UIRoot.json")), TEXT("正常配置不是生成物")));
		ASSERT_THAT(IsFalse(ForgeEditorUtils::IsGeneratedEntry(TEXT("schema.json")), TEXT("不含下划线前缀不是生成物")));
		ASSERT_THAT(IsFalse(ForgeEditorUtils::IsGeneratedEntry(TEXT("my_schema.json")), TEXT("名字里含 _schema 但不是下划线开头，不该被误杀")));
	}

	TEST_METHOD(生成路径按整段判定)
	{
		ASSERT_THAT(IsTrue(
			ForgeEditorUtils::IsGeneratedPath(TEXT("E:/P/Config/AssetForge/_schema/umg-widget.json")),
			TEXT("路径里出现 _schema 段即生成物")));
		ASSERT_THAT(IsTrue(
			ForgeEditorUtils::IsGeneratedPath(TEXT("E:/P/Config/AssetForge/_forge-state.json")),
			TEXT("路径里出现 _forge-state.json 即生成物")));

		// 整段匹配：`_schema` 作为更长名字的一部分不该命中——这是与 `Contains` 的关键差别。
		ASSERT_THAT(IsFalse(
			ForgeEditorUtils::IsGeneratedPath(TEXT("E:/P/Config/AssetForge/umg/Not_schema_here.json")),
			TEXT("非独立段中的 _schema 不该命中")));
		ASSERT_THAT(IsFalse(
			ForgeEditorUtils::IsGeneratedPath(TEXT("E:/P/Config/AssetForge/umg/UI/WBP_UIRoot.json")),
			TEXT("普通配置路径不是生成物")));
	}

	TEST_METHOD(噪声资产路径判定)
	{
		ASSERT_THAT(IsTrue(
			ForgeEditorUtils::IsNoiseAssetPath(TEXT("/Game/__ExternalActors__/Map/0/ABC")),
			TEXT("外部 Actor 是中间产物")));
		ASSERT_THAT(IsTrue(
			ForgeEditorUtils::IsNoiseAssetPath(TEXT("/Developers/Someone/Test")),
			TEXT("开发者目录不是可纳管资产")));
		ASSERT_THAT(IsFalse(
			ForgeEditorUtils::IsNoiseAssetPath(TEXT("/Game/UI/Framework/WBP_UIRoot")),
			TEXT("正常资产不该被过滤")));
	}

	TEST_METHOD(可解析配置的分类)
	{
		const TArray<FString> Kinds = { TEXT("umg-widget") };
		const ForgeEditorUtils::FForgeConfigClassification Result =
			Classify(/*bInParsed=*/true, /*InRawJson=*/nullptr, Kinds);

		ASSERT_THAT(IsTrue(Result.Class == ForgeEditorUtils::EForgeConfigClass::Parsed, TEXT("应判为可解析")));
		ASSERT_THAT(AreEqual(FString(TEXT("umg-widget")), Result.Kind, TEXT("kind 应带出来")));
		ASSERT_THAT(AreEqual(FString(TEXT("/Game/UI/WBP_Probe")), Result.AssetPath, TEXT("资产路径应带出来")));
		ASSERT_THAT(AreEqual(7, Result.Revision, TEXT("修订号应带出来")));
	}

	TEST_METHOD(外来kind与解析失败是两件事)
	{
		const TArray<FString> Kinds = { TEXT("umg-widget") };

		// 有 `kind` 但不认识 → 外来配置：它没坏，只是不归本管道管。
		const ForgeEditorUtils::FForgeConfigClassification Foreign =
			Classify(/*bInParsed=*/false, MakeJsonWithKind(TEXT("some-other-tool")), Kinds);
		ASSERT_THAT(IsTrue(
			Foreign.Class == ForgeEditorUtils::EForgeConfigClass::ForeignKind,
			TEXT("不认识的 kind 应判为外来配置，而不是解析失败")));
		ASSERT_THAT(AreEqual(FString(TEXT("some-other-tool")), Foreign.ForeignKind, TEXT("应带出那个 kind")));

		// JSON 语法错误（原始 JSON 都读不出来）→ 解析失败，且要带出原因。
		const ForgeEditorUtils::FForgeConfigClassification Broken =
			Classify(/*bInParsed=*/false, /*InRawJson=*/nullptr, Kinds);
		ASSERT_THAT(IsTrue(
			Broken.Class == ForgeEditorUtils::EForgeConfigClass::ParseFailed,
			TEXT("读不出 JSON 应判为解析失败")));
		ASSERT_THAT(IsFalse(Broken.ErrorMessage.IsEmpty(), TEXT("解析失败必须带原因，不能静默")));

		// 有 `kind` 但正好是本插件支持的、却没解析成功 → 仍是解析失败（不是外来）。
		const ForgeEditorUtils::FForgeConfigClassification SupportedButBroken =
			Classify(/*bInParsed=*/false, MakeJsonWithKind(TEXT("umg-widget")), Kinds);
		ASSERT_THAT(IsTrue(
			SupportedButBroken.Class == ForgeEditorUtils::EForgeConfigClass::ParseFailed,
			TEXT("支持的 kind 没解析成功仍算解析失败")));
	}

	TEST_METHOD(导出落点优先用既有配置)
	{
		// 清单里有记录 → 用它，绝不按默认路径另建（这条曾只在窗口与右键上成立，命令行漏了）。
		ASSERT_THAT(AreEqual(
			FString(TEXT("E:/P/Config/AssetForge/custom/WhereIWantIt.json")),
			ForgeEditorUtils::ChooseExportConfigPath(
				TEXT("E:/P/Config/AssetForge/custom/WhereIWantIt.json"),
				TEXT("E:/P/Config/AssetForge/umg/default.json")),
			TEXT("有既有路径时必须用它")));

		ASSERT_THAT(AreEqual(
			FString(TEXT("E:/P/Config/AssetForge/umg/default.json")),
			ForgeEditorUtils::ChooseExportConfigPath(
				TEXT(""),
				TEXT("E:/P/Config/AssetForge/umg/default.json")),
			TEXT("没有既有路径才退默认")));
	}

	/**
	 * 所有导出入口都经 `ForgeService::ExportAsset`，它用这条落点规则。这条用例把规则本身钉在纯函数上——入口侧只需确认调了它。
	 */
	TEST_METHOD(导出落点规则对所有入口唯一)
	{
		const FString Existing = TEXT("E:/P/Config/AssetForge/mat/Custom/M_Moved.json");
		const FString Default = TEXT("E:/P/Config/AssetForge/material/M_Moved.json");

		// 反复调用结果稳定（纯函数、无状态），且绝不返回默认路径。
		for (int32 Index = 0; Index < 3; ++Index)
		{
			ASSERT_THAT(AreEqual(
				Existing,
				ForgeEditorUtils::ChooseExportConfigPath(Existing, Default),
				TEXT("有既有配置时，任何一次调用都必须给出它")));
		}
	}
};

#endif	// WITH_AUTOMATION_WORKER
