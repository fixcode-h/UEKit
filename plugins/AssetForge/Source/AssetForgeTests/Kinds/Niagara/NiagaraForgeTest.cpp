#include "CQTest.h"

#include "Registry/ForgeKindRegistry.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgeTypes.h"
#include "Kinds/Niagara/NiagaraForge.h"

#if WITH_AUTOMATION_WORKER

/**
 * Niagara 处理器的守门用例。
 *
 * 测的是「只有这个 kind 才知道、通用层给不了」的判定：
 * - 发射器必须有名字（匹配键）、不重名，模板来源必须能解析成真正的 Emitter 资产；
 * - root.props 是系统层用户参数（`User.` 前缀）；发射器节点的 props 是可写参数（模块输入）
 *   覆盖——名字要写**复制品的**（引擎复制模板时换掉了参数名的发射器段），两边写错都要
 *   在动资产之前被拒绝。
 *
 * 走 dry-run 与纯校验：不建包、不写盘、不碰 Niagara 资产本身——
 * 「建出来的系统能不能用」由真实导入（把 Cue 资产建出来）负责。
 */
TEST_CLASS_WITH_FLAGS(NiagaraForgeTest, "AssetForge.Niagara",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 引擎内置模板：项目的自检与 Cue 资产都以它为来源。 */
	static constexpr TCHAR const* SpriteBurstTemplate =
		TEXT("/Niagara/DefaultAssets/Templates/Emitters/SimpleSpriteBurst");

	/** 造一份最小文档：只换 children 与 root.props 两段；asset 指向必定不存在的路径——dry-run 不碰盘。 */
	static bool ParseDocument(const FString& InChildren, const FString& InRootProps, FForgeDocument& OutDocument, FString* OutError = nullptr)
	{
		const FString PropsField = InRootProps.IsEmpty()
			? FString()
			: FString::Printf(TEXT(",\n\t\t\t\"props\": %s"), *InRootProps);

		const FString Text = FString::Printf(TEXT(R"({
			"kind": "niagara",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "/Game/Test/Niagara/NS_ForgeProbe",
			"root":
			{
				"class": "NiagaraSystem",
				"name": "NS_ForgeProbe"%s,
				"children": %s
			}
		})"), *PropsField, *InChildren);

		TSharedPtr<FJsonObject> Json;
		FString SyntaxError;
		if (!ForgeJsonText::ParseObject(Text, Json, SyntaxError))
		{
			if (OutError != nullptr)
			{
				*OutError = FString::Printf(TEXT("JSON 语法错误：%s"), *SyntaxError);
			}
			return false;
		}

		TArray<FForgeDiagnostic> Diagnostics;
		const bool bParsed = ForgeDoc::Parse(Json, TEXT("test.json"), OutDocument, Diagnostics);
		if (!bParsed && OutError != nullptr)
		{
			*OutError = TEXT("文档校验失败：");
			for (const FForgeDiagnostic& Diagnostic : Diagnostics)
			{
				*OutError += FString::Printf(TEXT("\n  [%s] %s"), *Diagnostic.FieldPath, *Diagnostic.Message);
			}
			if (Diagnostics.Num() == 0)
			{
				*OutError += TEXT("（无诊断）");
			}
		}
		return bParsed;
	}

	static FString MakeEmitter(const FString& InName, const FString& InTemplate, const FString& InProps = FString())
	{
		const FString PropsField = InProps.IsEmpty()
			? FString()
			: FString::Printf(TEXT(",\n\t\t\t\t\"props\": %s"), *InProps);

		return FString::Printf(TEXT(
			"[\n\t\t\t\t{\n\t\t\t\t\t\"class\": \"%s\",\n\t\t\t\t\t\"name\": \"%s\"%s\n\t\t\t\t}\n\t\t\t]"),
			*InTemplate, *InName, *PropsField);
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
		ASSERT_THAT(IsTrue(ForgeKindRegistry::GetSupportedKinds().Contains(NiagaraForge::Kind),
			TEXT("niagara 必须出现在支持的 kind 名单里（窗口与校验都读它）")));
	}

	TEST_METHOD(空_class_在文档解析阶段就被拒绝)
	{
		// `class` 是通用层要求的必填字段：空壳节点连文档都进不来（处理器的同类检查只是兜底）。
		FForgeDocument Document;
		FString ParseError;
		ASSERT_THAT(IsFalse(ParseDocument(MakeEmitter(TEXT("MuzzleFlash"), FString()), FString(), Document, &ParseError),
			FString::Printf(TEXT("空 class 应在文档解析阶段就被拒绝：%s"), *ParseError)));
	}

	TEST_METHOD(找不到的模板被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			MakeEmitter(TEXT("MuzzleFlash"), TEXT("/Niagara/DefaultAssets/Templates/Emitters/NoSuchTemplate")),
			FString(),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(NiagaraForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("模板路径不存在必须报错，不能猜")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children[0].class"), EForgeSeverity::Error),
			TEXT("诊断应指向该发射器的 class")));
	}

	TEST_METHOD(发射器重名被处理器拒绝)
	{
		const FString Emitters = FString::Printf(TEXT(
			"[\n\t\t\t\t{ \"class\": \"%s\", \"name\": \"Duplicated\" },\n\t\t\t\t{ \"class\": \"%s\", \"name\": \"Duplicated\" }\n\t\t\t]"),
			SpriteBurstTemplate, SpriteBurstTemplate);

		// 通用层不管重名（`name` 的语义由各类型自己定）：文档应当解析通过，
		// 由 niagara 处理器在导入计划阶段拒绝——名字是它与系统里既有发射器对应的匹配键。
		FForgeDocument Document;
		FString ParseError;
		ASSERT_THAT(IsTrue(ParseDocument(Emitters, FString(), Document, &ParseError),
			FString::Printf(TEXT("测试文档本身应能解析：%s"), *ParseError)));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(NiagaraForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("重名会让导入无法对应，必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children[1]"), EForgeSeverity::Error),
			TEXT("诊断应指向第二个发射器")));
	}

	TEST_METHOD(发射器_props_写模板里没有的参数名被拒绝)
	{
		// child.props 是**可写参数**（模块输入）覆盖的落点；名字必须能在模板里对上
		//（按去发射器段对照）——写错（比如把系统层的 `User.*` 写到发射器上）必须被拒绝，
		// 静默忽略会让人以为它生效了。
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			MakeEmitter(TEXT("MuzzleFlash"), SpriteBurstTemplate, TEXT(R"({ "User.SpriteSize": 12.0 })")),
			FString(),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(NiagaraForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("模板里没有的参数名必须在计划阶段报错")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.children[0].props.User.SpriteSize"), EForgeSeverity::Error),
			TEXT("诊断应指向该参数")));
	}

	TEST_METHOD(发射器_props_写复制品参数在_dry_run_下通过)
	{
		// 正例守卫：复制品上存在的可写参数必须放行——炸弹爆炸的四个发射器全靠这条路径调参，
		// 计划阶段的参数名校验不能把它误伤。名字的第二段是**发射器名**（复制品那一份）。
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			MakeEmitter(TEXT("MuzzleFlash"), SpriteBurstTemplate,
				TEXT(R"({ "Constants.MuzzleFlash.SpawnBurst_Instantaneous.Spawn Count": 10 })")),
			FString(),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsTrue(NiagaraForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("复制品上的可写参数应通过计划")));
	}

	TEST_METHOD(发射器_props_写模板自己的名字被拒绝)
	{
		// 模板里的参数叫 `Constants.Emitter.X`（模板的发射器名叫 Emitter），复制品上引擎把它
		// 换成了 `Constants.<发射器名>.X`——写模板的名字会落在不驱动任何模块的孤参数上
		//（配置看起来生效、画面上没有），必须拒绝。这条守卫钉住"按去发射器段对照且全等不算命中"。
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			MakeEmitter(TEXT("MuzzleFlash"), SpriteBurstTemplate,
				TEXT(R"({ "Constants.Emitter.SpawnBurst_Instantaneous.Spawn Count": 10 })")),
			FString(),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(NiagaraForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("模板自己的名字必须被拒绝——写它不会生效")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report,
			TEXT("root.children[0].props.Constants.Emitter.SpawnBurst_Instantaneous.Spawn Count"),
			EForgeSeverity::Error),
			TEXT("诊断应指向该参数")));
	}

	TEST_METHOD(参数名缺少前缀被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			MakeEmitter(TEXT("MuzzleFlash"), SpriteBurstTemplate),
			TEXT(R"({ "SpriteSize": 12.0 })"),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(NiagaraForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("用户参数名必须带 `User.` 前缀（Niagara 的约定）")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.props.SpriteSize"), EForgeSeverity::Error),
			TEXT("诊断应指向该参数")));
	}

	TEST_METHOD(合法配置在_dry_run_下给出计划)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(
			MakeEmitter(TEXT("MuzzleFlash"), SpriteBurstTemplate),
			TEXT(R"({ "User.SpriteSize": 12.0 })"),
			Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsTrue(NiagaraForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("合法配置应给出计划，而不是报错")));
		ASSERT_THAT(IsTrue(Report.Actions.Num() > 0,
			TEXT("目标资产不存在 → 计划里应有动作")));
	}

};

#endif // WITH_AUTOMATION_WORKER
