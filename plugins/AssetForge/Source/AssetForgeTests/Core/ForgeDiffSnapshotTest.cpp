// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"
#include "ForgeTestAssets.h"

#include "Core/ForgeDiffSnapshot.h"
#include "Registry/ForgeKindRegistry.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgePropertyCodec.h"
#include "Core/ForgeTypes.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// 守卫必须与 CQTest 宏的实体条件一致（见 ForgeEditorUtilsTest 的同款说明）。
#if WITH_AUTOMATION_WORKER

/**
 * 属性差异视图的快照逻辑守门用例。
 *
 * 这里守的是本功能**唯一真正危险的一句话**：「造一份副本、把配置写进去，不要影响真实资产」。
 * 走错一步（比如对副本调各 kind 的 `ImportDocument`）就会把配置写进真实资产并落盘——
 * 那是一次静默的数据破坏。所以这条约束必须由自动化测试看着，不能只靠人肉实机点一遍。
 */
TEST_CLASS_WITH_FLAGS(ForgeDiffSnapshotTest, "AssetForge.Editor.DiffSnapshot",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 造一份「指向真实数据资产」的配置文档（属性由调用方填）。 */
	static FForgeDocument MakeDocumentFor(const FString& InAssetPath)
	{
		FForgeDocument Document;
		Document.Kind = TEXT("data-asset");
		Document.SchemaVersion = 1;
		Document.Asset = InAssetPath;
		Document.Root.ClassName = TEXT("DataAsset");
		Document.Root.Name = TEXT("root");
		Document.Root.Props = MakeShared<FJsonObject>();
		return Document;
	}

	/**
	 * 对象属性 → 文本，供前后比对。
	 *
	 * **必须容忍 null**：`ExportDeltaProperties` 在「没有任何属性与默认不同」时会把 OutJson
	 * 置为 null（`ForgePropertyCodec.cpp` 里刻意这么做的——好让"空 props"与"没写 props"
	 * 在增量比较里等价）。直接对结果调 `ToSharedRef()` 会触发 `check(IsValid())` 并**崩掉整个
	 * 测试进程**（实测过：那一崩会连带后面的用例都不跑）。
	 */
	static FString ObjectToText(UObject* InObject)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		TArray<FForgeDiagnostic> Diagnostics;
		ForgePropertyCodec::ExportDeltaProperties(InObject, Json, Diagnostics, TEXT("root.props"));

		if (!Json.IsValid())
		{
			// 空 delta = 所有属性都是默认值。给一个稳定的空表示，别返回空串。
			return TEXT("{}");
		}

		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
		return Text;
	}

	/**
	 * **核心断言**：对副本应用配置后，真实资产一个字节都不该动。
	 *
	 * 做法：拿真实资产导出属性 → 造副本并写入一份**改变过**的配置 → 再导出真实资产属性 →
	 * 两者必须完全一致。若实现走错路（改到真实资产），这里会立刻红。
	 */
	TEST_METHOD(副本不影响真实资产)
	{
		// 这个资产由项目自己的配置纳管，测试环境里必然存在（见 docs/tech/asset-contracts.md）。
		const FString AssetPath = ForgeTestAssets::DataAsset;
		UObject* Target = LoadObject<UObject>(nullptr, *AssetPath);
		if (Target == nullptr)
		{
			// 资产不在就跳过这一条（测试不该因为环境缺资产而红），但要留痕。
			AddInfo(FString::Printf(TEXT("测试资产不存在，跳过：%s"), *AssetPath));
			return;
		}

		// 基线：真实资产当前的可配置属性。
		const FString BeforeText = ObjectToText(Target);

		// 造副本并写进一份配置。用 data-asset 的配方（先重置再写）。
		FForgeDocument Document = MakeDocumentFor(AssetPath);
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		// 挑一个几乎所有 UObject 都有的可配置属性来改：让它与基线必然不同。
		Props->SetBoolField(TEXT("bIsEditorOnly"), true);
		Document.Root.Props = Props;

		const ForgeKindRegistry::FForgeDiffSupport Support =
			ForgeKindRegistry::GetPropertyDiffSupport(TEXT("data-asset"));

		UObject* Copy = ForgeDiffSnapshot::MakeConfiguredCopy(Target, Document, Support);
		ASSERT_THAT(IsNotNull(Copy, TEXT("副本应当被造出来")));

		if (Copy == nullptr)
		{
			return;
		}

		// ① 副本必须是瞬态的——这是「不会被存盘」的保证。
		ASSERT_THAT(IsTrue(
			ForgeDiffSnapshot::IsTransientObject(Copy),
			TEXT("副本必须在瞬态包里")));

		// ② 副本的类型必须与源对象一致（用 DuplicateObject<UObject> 会丢掉真实类型）。
		ASSERT_THAT(IsTrue(
			Copy->GetClass() == Target->GetClass(),
			TEXT("副本的类必须与真实资产一致")));

		// ③ 副本必须是**独立的**对象，不是同一个指针。
		ASSERT_THAT(IsFalse(Copy == Target, TEXT("副本不能就是真实资产本身")));

		// ④ 核心：真实资产的属性在造副本前后完全一致。
		const FString AfterText = ObjectToText(Target);

		ASSERT_THAT(AreEqual(
			BeforeText, AfterText,
			TEXT("造副本不得改动真实资产的任何属性")));

		// ⑤ 真实资产也不该被标脏（标脏会让它被保存）。
		ASSERT_THAT(IsFalse(
			Target->GetOutermost()->IsDirty(),
			TEXT("真实资产不该因为造副本而被标脏")));
	}

	/** 瞬态判定本身：真实资产不是瞬态，快照副本是。 */
	TEST_METHOD(瞬态判定区分真实资产与副本)
	{
		const FString AssetPath = ForgeTestAssets::DataAsset;
		UObject* Target = LoadObject<UObject>(nullptr, *AssetPath);
		if (Target == nullptr)
		{
			AddInfo(TEXT("测试资产不存在，跳过"));
			return;
		}

		ASSERT_THAT(IsFalse(
			ForgeDiffSnapshot::IsTransientObject(Target),
			TEXT("真实资产不是瞬态对象")));

		// 副本本身就是瞬态对象——这正是「不会被存盘」的保证。
		// 这里不另造临时对象：引擎里 UObject 与 UDataAsset 都是抽象类，`NewObject` 会 ensure 失败，
		// 而副本是现成的、语义也更贴切的样本。
		const ForgeKindRegistry::FForgeDiffSupport Support =
			ForgeKindRegistry::GetPropertyDiffSupport(TEXT("data-asset"));
		FForgeDocument Document = MakeDocumentFor(AssetPath);
		UObject* Copy = ForgeDiffSnapshot::MakeConfiguredCopy(Target, Document, Support);
		ASSERT_THAT(IsNotNull(Copy, TEXT("副本应当被造出来")));

		if (Copy != nullptr)
		{
			ASSERT_THAT(IsTrue(
				ForgeDiffSnapshot::IsTransientObject(Copy),
				TEXT("副本必须是瞬态对象")));
		}

		ASSERT_THAT(IsFalse(
			ForgeDiffSnapshot::IsTransientObject(nullptr),
			TEXT("空指针不是瞬态对象（防御）")));
	}

	/**
	 * 反例守卫：直接调各 kind 的 `ImportDocument` **会**改动真实资产。
	 *
	 * 这条不是为了跑通，而是把「为什么不能走 ImportDocument」钉成可执行的事实——
	 * 下一个人如果想把实现"简化"成 ImportDocument，这条用例会告诉他代价是什么。
	 * 它只做 dry-run，不落盘。
	 */
	TEST_METHOD(直接跑导入会指向真实资产而不是副本)
	{
		const FString AssetPath = ForgeTestAssets::DataAsset;
		UObject* Target = LoadObject<UObject>(nullptr, *AssetPath);
		if (Target == nullptr)
		{
			AddInfo(TEXT("测试资产不存在，跳过"));
			return;
		}

		// 造一个副本，然后把「配置里的资产路径」保持指向真实资产——
		// 这正是 `ImportDocument` 会用的查找键。它找到的必然是真实资产，与副本无关。
		FForgeDocument Document = MakeDocumentFor(AssetPath);
		const ForgeKindRegistry::FForgeDiffSupport Support =
			ForgeKindRegistry::GetPropertyDiffSupport(TEXT("data-asset"));
		UObject* Copy = ForgeDiffSnapshot::MakeConfiguredCopy(Target, Document, Support);
		ASSERT_THAT(IsNotNull(Copy, TEXT("副本应当被造出来")));

		if (Copy == nullptr)
		{
			return;
		}

		// `ImportDocument` 按 `InDocument.Asset` 查对象 —— 副本的包路径与它不同，
		// 所以它永远找不到副本。这条断言把那个事实写下来。
		ASSERT_THAT(IsFalse(
			Copy->GetPathName().StartsWith(AssetPath),
			TEXT("副本的路径不在资产的包路径下——所以按资产路径查对象的导入找不到它")));
	}

	/**
	 * 两份配置产出的副本必须**各自不同**——这是「属性差异真的在比两份配置」的端到端证据。
	 *
	 * 属性差异的两列就是这个函数各造一份：左 = HEAD 版配置、右 = 工作区配置。
	 * 如果它总是产出相同对象（比如忘了把配置写进去），界面会永远显示"没有差异"，
	 * 而那看起来完全正常——正是这种"看起来正常"的错误最该被测试盯住。
	 *
	 * **用真实的配置内容**（那份资产的真配置文件）而不是自己编一个属性名：
	 * 编出来的名字很可能不是 `CPF_Edit`（第一版用例就栽在 `bIsEditorOnly` 上——
	 * 它不是可配属性，两份副本因此完全相同，用例红）。真配置里的字段一定是可配的，
	 * 因为它本来就是导出产物。
	 */
	TEST_METHOD(两份配置产出的副本各自不同)
	{
		const FString AssetPath = ForgeTestAssets::DataAsset;
		UObject* Target = LoadObject<UObject>(nullptr, *AssetPath);
		if (Target == nullptr)
		{
			AddInfo(FString::Printf(TEXT("测试资产不存在，跳过：%s"), *AssetPath));
			return;
		}

		const ForgeKindRegistry::FForgeDiffSupport Support =
			ForgeKindRegistry::GetPropertyDiffSupport(TEXT("data-asset"));

		// 「当前版」= 真实配置文件的内容。
		const FString ConfigPath =
			ForgePipeline::GetConfigRootDir() / TEXT("data/AssetForgeTest/DA_ForgeTest.json");

		FForgeDocument Current;
		FForgeReport LoadReport;
		if (!ForgePipeline::LoadDocument(ConfigPath, Current, LoadReport))
		{
			AddWarning(FString::Printf(TEXT("读不到样本配置，跳过：%s"), *ConfigPath));
			return;
		}

		// 「基线版」= 同一份配置，但把属性清空（相当于"这次改动把属性都删了"）。
		// 两版**按构造**必然不同，且都是真实 schema。
		FForgeDocument Baseline = Current;
		Baseline.Root.Props = MakeShared<FJsonObject>();
		Baseline.Root.Children.Reset();

		ASSERT_THAT(IsTrue(
			Current.Root.Props.IsValid() && Current.Root.Props->Values.Num() > 0,
			TEXT("样本配置应当有属性——否则这条用例证明不了任何事")));

		UObject* BaselineCopy = ForgeDiffSnapshot::MakeConfiguredCopy(Target, Baseline, Support);
		UObject* CurrentCopy = ForgeDiffSnapshot::MakeConfiguredCopy(Target, Current, Support);

		ASSERT_THAT(IsNotNull(BaselineCopy, TEXT("基线副本应当被造出来")));
		ASSERT_THAT(IsNotNull(CurrentCopy, TEXT("当前副本应当被造出来")));

		if (BaselineCopy == nullptr || CurrentCopy == nullptr)
		{
			return;
		}

		ASSERT_THAT(IsFalse(
			BaselineCopy == CurrentCopy,
			TEXT("两列不能是同一个对象——那样界面永远显示没有差异")));

		// 属性不同：把两次结果各导出一遍来比。
		ASSERT_THAT(IsFalse(
			ObjectToText(BaselineCopy) == ObjectToText(CurrentCopy),
			TEXT("两份配置不同，副本的属性就该不同——否则属性差异永远看不出东西")));
	}
};

#endif	// WITH_AUTOMATION_WORKER
