// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgeTypes.h"
#include "Kinds/Material/MaterialForge.h"
#include "Kinds/Material/MaterialGraphCodec.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionTextureSample.h"

#if WITH_AUTOMATION_WORKER

/**
 * 材质处理器的守门用例。
 *
 * 测的是「这个 kind 才知道、通用层给不了」的判定：
 *
 * - 材质图是 DAG，节点身份是**配置自己发明的名字**——所以重名、悬空引用、环都要在这里拦下；
 * - 引脚名有两套写法（编辑器显示的短名、引擎头文件里的长名），映射错了就是「导入成功但没连上」；
 * - 判等靠结构路径，手写短名与导出形态必须能对上，否则每次导入都会误报一遍改动。
 *
 * 走 dry-run 与纯函数：不建包、不写盘，用例不依赖任何已有资产。
 */
TEST_CLASS_WITH_FLAGS(MaterialForgeTest, "AssetForge.Material",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 造一份最小文档，只换 root 那一段（props / inputs / nodes 都写在这里）。 */
	static bool ParseDocument(const FString& InRootBody, FForgeDocument& OutDocument)
	{
		const FString Text = FString::Printf(TEXT(R"({
			"kind": "material",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "/Game/Test/M_ForgeProbe",
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

	static FString MakeRoot(const FString& InInputs, const FString& InNodes)
	{
		return FString::Printf(TEXT(R"({
			"class": "Material",
			"name": "M_ForgeProbe",
			"inputs": %s,
			"nodes": %s
		})"), *InInputs, *InNodes);
	}

	TEST_METHOD(表达式类短名解析)
	{
		FString Error;

		ASSERT_THAT(IsNotNull(MaterialGraphCodec::ResolveExpressionClass(TEXT("Multiply"), Error),
			TEXT("`Multiply` 应解析成 /Script/Engine.MaterialExpressionMultiply")));
		ASSERT_THAT(IsNotNull(MaterialGraphCodec::ResolveExpressionClass(TEXT("TextureSample"), Error),
			TEXT("`TextureSample` 应解析成 /Script/Engine.MaterialExpressionTextureSample")));
		ASSERT_THAT(IsNotNull(
			MaterialGraphCodec::ResolveExpressionClass(TEXT("/Script/Engine.MaterialExpressionConstant"), Error),
			TEXT("完整路径也要认")));

		Error.Reset();
		ASSERT_THAT(IsNull(MaterialGraphCodec::ResolveExpressionClass(TEXT("NotARealExpression"), Error),
			TEXT("不存在的类必须解析失败，不能猜")));
		ASSERT_THAT(IsFalse(Error.IsEmpty(), TEXT("失败时要给出原因")));
	}

	TEST_METHOD(材质属性名三种写法等价)
	{
		const int32 Bare = MaterialGraphCodec::ResolveMaterialProperty(TEXT("BaseColor"));

		ASSERT_THAT(AreEqual(Bare, MaterialGraphCodec::ResolveMaterialProperty(TEXT("MP_BaseColor")),
			TEXT("`BaseColor` 与 `MP_BaseColor` 是同一个属性")));
		ASSERT_THAT(AreEqual(Bare, MaterialGraphCodec::ResolveMaterialProperty(TEXT("basecolor")),
			TEXT("大小写不敏感")));
		ASSERT_THAT(AreEqual(FString(TEXT("BaseColor")), MaterialGraphCodec::MakeMaterialPropertyKey(Bare),
			TEXT("写回配置时去掉 MP_ 前缀")));
		ASSERT_THAT(AreEqual(INDEX_NONE, MaterialGraphCodec::ResolveMaterialProperty(TEXT("MP_MAX")),
			TEXT("MP_MAX 是哨兵，不是可连的属性")));
	}

	TEST_METHOD(引脚名在短名与长名之间互通)
	{
		UMaterialExpression* TextureSampleDefault =
			Cast<UMaterialExpression>(UMaterialExpressionTextureSample::StaticClass()->GetDefaultObject());
		ASSERT_THAT(IsNotNull(TextureSampleDefault, TEXT("TextureSample 的默认对象应存在")));

		// 引擎把 `Coordinates` 缩成 `UVs`：导出写短名，两种写法都要能解析回同一个引脚。
		const int32 ByShort = MaterialGraphCodec::ResolveInputIndex(TextureSampleDefault, TEXT("UVs"));
		ASSERT_THAT(IsTrue(ByShort != INDEX_NONE, TEXT("编辑器里显示的引脚名要能解析")));
		ASSERT_THAT(AreEqual(ByShort, MaterialGraphCodec::ResolveInputIndex(TextureSampleDefault, TEXT("Coordinates")),
			TEXT("引擎头文件里的长名字也认，指的是同一个引脚")));
		ASSERT_THAT(AreEqual(INDEX_NONE, MaterialGraphCodec::ResolveInputIndex(TextureSampleDefault, TEXT("NoSuchPin")),
			TEXT("不存在的引脚要报找不到")));

		// 输出侧：空串是第一个输出，`RGB` 是编辑器里那个整块输出的显示名。
		ASSERT_THAT(AreEqual(0, MaterialGraphCodec::ResolveOutputIndex(TextureSampleDefault, TEXT("")),
			TEXT("空串 = 第一个输出")));
		ASSERT_THAT(AreEqual(0, MaterialGraphCodec::ResolveOutputIndex(TextureSampleDefault, TEXT("RGB")),
			TEXT("`RGB` 指整块输出")));
		ASSERT_THAT(IsTrue(MaterialGraphCodec::ResolveOutputIndex(TextureSampleDefault, TEXT("A")) > 0,
			TEXT("TextureSample 的 A 通道是独立输出")));
	}

	TEST_METHOD(节点重名被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("{}"),
			TEXT(R"([
				{ "name": "Tint", "class": "Constant3Vector" },
				{ "name": "Tint", "class": "Constant3Vector" }
			])")), Document), TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(MaterialForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("重名会让匹配键失去唯一性，必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.nodes[1].name"), EForgeSeverity::Error),
			TEXT("诊断应指向第二个重名节点")));
	}

	TEST_METHOD(悬空引用被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT(R"({ "BaseColor": "Missing" })"),
			TEXT(R"([ { "name": "Tint", "class": "Constant3Vector" } ])")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(MaterialForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("连到不存在的节点必须拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.inputs"), EForgeSeverity::Error),
			TEXT("诊断应指向 inputs")));
	}

	TEST_METHOD(引脚名写错被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("{}"),
			TEXT(R"([
				{ "name": "Sum", "class": "Add", "inputs": { "X": "C" } },
				{ "name": "C", "class": "Constant" }
			])")), Document), TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(MaterialForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("Add 的引脚叫 A/B，写 X 必须在计划阶段就拦下")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.nodes[0].inputs.X"), EForgeSeverity::Error),
			TEXT("诊断要精确到那根引脚")));
	}

	TEST_METHOD(成环被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("{}"),
			TEXT(R"([
				{ "name": "A", "class": "Add", "inputs": { "A": "B" } },
				{ "name": "B", "class": "Add", "inputs": { "A": "A" } }
			])")), Document), TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(MaterialForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("材质图必须是有向无环图")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.nodes"), EForgeSeverity::Error),
			TEXT("诊断应说出成环")));
	}

	TEST_METHOD(简写连线与对象写法等价)
	{
		const FString Nodes = TEXT(R"([
			{ "name": "Tex", "class": "TextureSample" },
			{ "name": "Tint", "class": "Multiply", "inputs": { "A": "Tex", "B": { "from": "Tex", "output": "A" } } }
		])");

		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(TEXT(R"({ "BaseColor": "Tint" })"), Nodes), Document),
			TEXT("简写与对象两种连线写法都要能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsTrue(MaterialForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("同一个源节点的两个不同输出是合法的（DAG 的共享节点）")));
	}

	TEST_METHOD(材质属性写两遍被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT(R"({ "BaseColor": "Tint", "MP_BaseColor": "Tint" })"),
			TEXT(R"([ { "name": "Tint", "class": "Constant3Vector" } ])")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(MaterialForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("同一属性的两种写法指的是同一个键，写两遍必须拒绝")));
	}

	TEST_METHOD(抽象类与折叠图被拒绝)
	{
		FString Error;

		// 折叠图（Composite）与注释的信息不只在连线上，导出去再导入会静默丢掉结构。
		ASSERT_THAT(IsNotNull(MaterialGraphCodec::ResolveExpressionClass(TEXT("Composite"), Error),
			TEXT("类本身是存在的")));

		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(MakeRoot(
			TEXT("{}"),
			TEXT(R"([ { "name": "Fold", "class": "Composite" } ])")), Document),
			TEXT("测试文档本身应能解析")));

		FForgeReport Report;
		ASSERT_THAT(IsFalse(MaterialForge::ImportDocument(Document, /*bDryRun=*/true, Report),
			TEXT("Composite 表达不了，必须在计划阶段拒绝")));
		ASSERT_THAT(IsTrue(HasDiagnosticAt(Report, TEXT("root.nodes[0].class"), EForgeSeverity::Error),
			TEXT("诊断应指出是类的问题")));
	}

	/**
	 * 排布的守门用例：直接量**资产里节点的真实坐标**。
	 *
	 * 为什么值得有这条：坐标不进配置，所以「导入后节点是否叠在一起」在配置里根本看不出来，
	 * 只能问资产。它也是唯一能自动化验证「排布」这件事的角度——肉眼看一次不算回归保护。
	 *
	 * 断言的是三条：① 没有两个节点占同一个位置；② 连到材质属性的节点按列排开（X 不重复）；
	 * ③ 孤儿节点被安置在所有已排布节点的**左边**（我们自己的摆放规则）。
	 */
	TEST_METHOD(导入后的节点不重叠且按列排开)
	{
		UMaterial* Showcase = LoadObject<UMaterial>(nullptr, TEXT("/Game/Test/M_ForgeShowcase.M_ForgeShowcase"));
		if (Showcase == nullptr)
		{
			// 自检资产没建起来时跳过而不是误报：这条用例测的是排布，不负责建资产。
			AddWarning(TEXT("找不到 /Game/Test/M_ForgeShowcase，跳过排布断言"));
			return;
		}

		const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Showcase->GetExpressions();
		ASSERT_THAT(IsTrue(Expressions.Num() >= 5, TEXT("自检材质应有至少 5 个表达式节点")));

		TSet<FString> Positions;
		TMap<int32, int32> CountByX;

		// 自检配置里那个 `Constant`（R=0.5）是孤儿节点：没连任何材质属性，引擎的排版不管它。
		constexpr int32 Unset = MAX_int32;
		int32 OrphanX = Unset;
		int32 ConnectedLeftmostX = Unset;

		for (const TObjectPtr<UMaterialExpression>& Expression : Expressions)
		{
			if (Expression == nullptr)
			{
				continue;
			}

			Positions.Add(FString::Printf(TEXT("%d,%d"), Expression->MaterialExpressionEditorX, Expression->MaterialExpressionEditorY));
			CountByX.FindOrAdd(Expression->MaterialExpressionEditorX)++;

			if (Expression->IsA<UMaterialExpressionConstant>())
			{
				OrphanX = FMath::Min(OrphanX, Expression->MaterialExpressionEditorX);
			}
			else
			{
				ConnectedLeftmostX = FMath::Min(ConnectedLeftmostX, Expression->MaterialExpressionEditorX);
			}
		}

		ASSERT_THAT(AreEqual(Expressions.Num(), Positions.Num(),
			TEXT("没有任何两个节点该占同一个位置——叠在一起正是要避免的")));

		// 引擎的排版把每个深度放一列（同列里 Y 不同），所以至少要有两列才算排开。
		ASSERT_THAT(IsTrue(CountByX.Num() >= 2, TEXT("节点应分散在多个 X 列上，而不是一字排开或堆在一列")));

		ASSERT_THAT(IsTrue(OrphanX != Unset && ConnectedLeftmostX != Unset, TEXT("自检材质里应同时有孤儿节点与连线节点")));
		ASSERT_THAT(IsTrue(OrphanX < ConnectedLeftmostX,
			TEXT("孤儿节点要被摆在已排布节点的左边——引擎的排版不覆盖它们，堆在原点就是 bug")));
	}
};

#endif // WITH_AUTOMATION_WORKER
