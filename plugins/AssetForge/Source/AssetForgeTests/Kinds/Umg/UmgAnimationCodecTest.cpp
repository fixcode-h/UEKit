// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgeTypes.h"
#include "Kinds/Umg/UmgAnimationCodec.h"

#if WITH_AUTOMATION_WORKER

/**
 * UMG 动画编解码的守门用例（openspec/changes/umg-animation）。
 *
 * 只说这个 kind 才知道的判定与纯算法：`animations` 的字段级校验（重名动画、非法通道、
 * 缺字段、空 keys、target 解析与同名歧义）、秒 ↔ 帧换算与帧层比较、按名增量的计划。
 *
 * 全部走纯逻辑入口（`ParseAnimations` / `ComputePlan` / 时间换算）——不建资产、不碰
 * UObject，所以用例不依赖任何已有资产；真导入与「动画能播」由自检配置在 Editor 内验证。
 */
TEST_CLASS_WITH_FLAGS(UmgAnimationCodecTest, "AssetForge.UmgAnimation",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	/** 一棵最小控件树：根 `Root` 下挂一个 `Title`。 */
	static FString SimpleRoot()
	{
		return TEXT(R"({ "class": "SizeBox", "name": "Root", "children": [ { "class": "TextBlock", "name": "Title" } ] })");
	}

	/**
	 * 造一份最小文档。
	 *
	 * `InAnimationsField` 形如 `"animations": [ ... ]`；传空串表示整段省略
	 * （语义是「资产里不该有动画」）。
	 */
	static bool ParseDocument(const FString& InRootJson, const FString& InAnimationsField, FForgeDocument& OutDocument)
	{
		const FString Text = FString::Printf(TEXT(R"({
			"kind": "umg-widget",
			"schemaVersion": 1,
			"revision": 1,
			"asset": "/Game/UI/Test/WBP_ForgeAnimProbe",
			"parent": "/Script/UMG.UserWidget",
			"root": %s%s%s
		})"),
			*InRootJson,
			InAnimationsField.IsEmpty() ? TEXT("") : TEXT(",\n\t\t\t"),
			*InAnimationsField);

		TSharedPtr<FJsonObject> Json;
		FString Error;
		if (!ForgeJsonText::ParseObject(Text, Json, Error))
		{
			return false;
		}

		TArray<FForgeDiagnostic> Diagnostics;
		return ForgeDoc::Parse(Json, TEXT("test.json"), OutDocument, Diagnostics);
	}

	static bool HasErrorAt(const FForgeReport& InReport, const FString& InFieldPath)
	{
		for (const FForgeDiagnostic& Diagnostic : InReport.Diagnostics)
		{
			if (Diagnostic.FieldPath == InFieldPath && Diagnostic.Severity == EForgeSeverity::Error)
			{
				return true;
			}
		}
		return false;
	}

	/** 一条 0 → 目标值的轨道（供计划用例构造两侧内容）。 */
	static UmgAnimationCodec::FUmgTrack MakeTrack(const FString& InTarget, const FString& InChannel, double InEndSeconds, double InEndValue)
	{
		UmgAnimationCodec::FUmgTrack Track;
		Track.Target = InTarget;
		Track.Channel = InChannel;

		UmgAnimationCodec::FUmgKey Start;
		Start.Frame = UmgAnimationCodec::SecondsToFrame(0.0);
		Start.Value = 0.0;
		Track.Keys.Add(Start);

		UmgAnimationCodec::FUmgKey End;
		End.Frame = UmgAnimationCodec::SecondsToFrame(InEndSeconds);
		End.Value = InEndValue;
		Track.Keys.Add(End);

		return Track;
	}

	/** 一条 `RenderOpacity` 的 0 → 目标值淡入动画。 */
	static UmgAnimationCodec::FUmgAnimation MakeFadeAnimation(const FString& InName, double InEndSeconds, double InEndValue)
	{
		UmgAnimationCodec::FUmgAnimation Animation;
		Animation.Name = InName;
		Animation.Tracks.Add(MakeTrack(TEXT("Root/Title"), TEXT("RenderOpacity"), InEndSeconds, InEndValue));
		return Animation;
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

	//~ 时间换算 ---------------------------------------------------------------

	TEST_METHOD(秒转帧按60000分辨率)
	{
		ASSERT_THAT(IsTrue(UmgAnimationCodec::SecondsToFrame(0.0) == 0, TEXT("0 秒是第 0 帧")));
		ASSERT_THAT(IsTrue(UmgAnimationCodec::SecondsToFrame(0.001) == 60, TEXT("0.001 秒 = 60 帧")));
		ASSERT_THAT(IsTrue(UmgAnimationCodec::SecondsToFrame(0.33) == 19800, TEXT("0.33 秒 = 19800 帧")));
		ASSERT_THAT(IsTrue(UmgAnimationCodec::SecondsToFrame(0.6) == 36000, TEXT("0.6 秒 = 36000 帧")));
		ASSERT_THAT(IsTrue(UmgAnimationCodec::SecondsToFrame(1.0) == 60000, TEXT("1 秒 = 60000 帧")));
	}

	TEST_METHOD(秒帧往返无漂移)
	{
		const double Samples[] = { 0.6, 0.33, 0.001 };
		for (const double Seconds : Samples)
		{
			const int32 Frame = UmgAnimationCodec::SecondsToFrame(Seconds);
			ASSERT_THAT(IsTrue(FMath::IsNearlyEqual(UmgAnimationCodec::FrameToSeconds(Frame), Seconds, 1.0e-9),
				FString::Printf(TEXT("%f 秒往返后仍是它自己"), Seconds)));
		}
	}

	//~ 解析与校验 -------------------------------------------------------------

	TEST_METHOD(配置的动画被解析成轨道与帧)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(SimpleRoot(), TEXT(R"("animations": [
			{ "name": "Intro", "tracks": [
				{ "target": "Root/Title", "channel": "RenderOpacity", "keys": [
					{ "time": 0.0, "value": 0.0 },
					{ "time": 0.6, "value": 1.0 }
				] }
			] }
		])"), Document), TEXT("测试文档本身应能解析")));

		TArray<UmgAnimationCodec::FUmgAnimation> Animations;
		FForgeReport Report;
		ASSERT_THAT(IsTrue(UmgAnimationCodec::ParseAnimations(Document, Animations, Report), TEXT("合法配置应通过")));

		ASSERT_THAT(IsTrue(Animations.Num() == 1, TEXT("一条动画")));
		ASSERT_THAT(IsTrue(Animations[0].Name == TEXT("Intro"), TEXT("动画名原样保留")));
		ASSERT_THAT(IsTrue(Animations[0].Tracks.Num() == 1, TEXT("一条轨道")));
		ASSERT_THAT(IsTrue(Animations[0].Tracks[0].Keys.Num() == 2, TEXT("两个关键帧")));
		ASSERT_THAT(IsTrue(Animations[0].Tracks[0].Keys[1].Frame == 36000, TEXT("0.6 秒落在第 36000 帧")));
		ASSERT_THAT(IsTrue(Animations[0].Tracks[0].Keys[0].Interp == UmgAnimationCodec::EUmgInterp::Cubic,
			TEXT("interp 缺省是 cubic")));
	}

	TEST_METHOD(动画名重复被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(SimpleRoot(), TEXT(R"("animations": [
			{ "name": "Intro" },
			{ "name": "Intro" }
		])"), Document), TEXT("测试文档本身应能解析")));

		TArray<UmgAnimationCodec::FUmgAnimation> Animations;
		FForgeReport Report;
		ASSERT_THAT(IsFalse(UmgAnimationCodec::ParseAnimations(Document, Animations, Report), TEXT("重名应失败")));
		ASSERT_THAT(IsTrue(HasErrorAt(Report, TEXT("animations[1].name")), TEXT("错误指向第二条动画的名字")));
	}

	TEST_METHOD(不支持的通道被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(SimpleRoot(), TEXT(R"("animations": [
			{ "name": "Intro", "tracks": [
				{ "target": "Root/Title", "channel": "Opacity", "keys": [ { "time": 0.0, "value": 1.0 } ] }
			] }
		])"), Document), TEXT("测试文档本身应能解析")));

		TArray<UmgAnimationCodec::FUmgAnimation> Animations;
		FForgeReport Report;
		ASSERT_THAT(IsFalse(UmgAnimationCodec::ParseAnimations(Document, Animations, Report), TEXT("白名单外的通道应失败")));
		ASSERT_THAT(IsTrue(HasErrorAt(Report, TEXT("animations[0].tracks[0].channel")), TEXT("错误带字段级路径")));
	}

	TEST_METHOD(关键帧缺字段被拒绝)
	{
		FForgeReport Report;
		TArray<UmgAnimationCodec::FUmgAnimation> Animations;

		{
			FForgeDocument Document;
			ASSERT_THAT(IsTrue(ParseDocument(SimpleRoot(), TEXT(R"("animations": [
				{ "name": "Intro", "tracks": [
					{ "target": "Root/Title", "channel": "RenderOpacity", "keys": [ { "value": 1.0 } ] }
				] }
			])"), Document), TEXT("测试文档本身应能解析")));

			ASSERT_THAT(IsFalse(UmgAnimationCodec::ParseAnimations(Document, Animations, Report), TEXT("缺 time 应失败")));
			ASSERT_THAT(IsTrue(HasErrorAt(Report, TEXT("animations[0].tracks[0].keys[0].time")), TEXT("错误指向缺失的 time")));
		}

		{
			Report = FForgeReport();
			FForgeDocument Document;
			ASSERT_THAT(IsTrue(ParseDocument(SimpleRoot(), TEXT(R"("animations": [
				{ "name": "Intro", "tracks": [
					{ "target": "Root/Title", "channel": "RenderOpacity", "keys": [ { "time": 0.6 } ] }
				] }
			])"), Document), TEXT("测试文档本身应能解析")));

			ASSERT_THAT(IsFalse(UmgAnimationCodec::ParseAnimations(Document, Animations, Report), TEXT("缺 value 应失败")));
			ASSERT_THAT(IsTrue(HasErrorAt(Report, TEXT("animations[0].tracks[0].keys[0].value")), TEXT("错误指向缺失的 value")));
		}
	}

	TEST_METHOD(空keys被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(SimpleRoot(), TEXT(R"("animations": [
			{ "name": "Intro", "tracks": [
				{ "target": "Root/Title", "channel": "RenderOpacity", "keys": [] }
			] }
		])"), Document), TEXT("测试文档本身应能解析")));

		TArray<UmgAnimationCodec::FUmgAnimation> Animations;
		FForgeReport Report;
		ASSERT_THAT(IsFalse(UmgAnimationCodec::ParseAnimations(Document, Animations, Report), TEXT("空 keys 应失败")));
		ASSERT_THAT(IsTrue(HasErrorAt(Report, TEXT("animations[0].tracks[0].keys")), TEXT("错误指向 keys 数组")));
	}

	TEST_METHOD(target解析不到被拒绝)
	{
		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(SimpleRoot(), TEXT(R"("animations": [
			{ "name": "Intro", "tracks": [
				{ "target": "Root/Missing", "channel": "RenderOpacity", "keys": [ { "time": 0.0, "value": 1.0 } ] }
			] }
		])"), Document), TEXT("测试文档本身应能解析")));

		TArray<UmgAnimationCodec::FUmgAnimation> Animations;
		FForgeReport Report;
		ASSERT_THAT(IsFalse(UmgAnimationCodec::ParseAnimations(Document, Animations, Report), TEXT("找不到控件应失败")));
		ASSERT_THAT(IsTrue(HasErrorAt(Report, TEXT("animations[0].tracks[0].target")), TEXT("错误指向 target")));
	}

	TEST_METHOD(同名控件让target有歧义被拒绝)
	{
		// 引擎按控件名绑定，同名会让运行时绑错——不同父节点下的同名也算歧义。
		const FString Tree = TEXT(R"({
			"class": "HorizontalBox", "name": "Root", "children": [
				{ "class": "VerticalBox", "name": "Left", "children": [ { "class": "TextBlock", "name": "Label" } ] },
				{ "class": "VerticalBox", "name": "Right", "children": [ { "class": "TextBlock", "name": "Label" } ] }
			]
		})");

		FForgeDocument Document;
		ASSERT_THAT(IsTrue(ParseDocument(Tree, TEXT(R"("animations": [
			{ "name": "Intro", "tracks": [
				{ "target": "Root/Left/Label", "channel": "RenderOpacity", "keys": [ { "time": 0.0, "value": 1.0 } ] }
			] }
		])"), Document), TEXT("测试文档本身应能解析")));

		TArray<UmgAnimationCodec::FUmgAnimation> Animations;
		FForgeReport Report;
		ASSERT_THAT(IsFalse(UmgAnimationCodec::ParseAnimations(Document, Animations, Report), TEXT("同名控件应失败")));
		ASSERT_THAT(IsTrue(HasErrorAt(Report, TEXT("animations[0].tracks[0].target")), TEXT("错误指向 target")));
	}

	TEST_METHOD(animations省略与空数组都表示空集)
	{
		TArray<UmgAnimationCodec::FUmgAnimation> Animations;
		FForgeReport Report;

		{
			FForgeDocument Document;
			ASSERT_THAT(IsTrue(ParseDocument(SimpleRoot(), TEXT(""), Document), TEXT("测试文档本身应能解析")));
			ASSERT_THAT(IsTrue(UmgAnimationCodec::ParseAnimations(Document, Animations, Report), TEXT("省略 animations 是合法的")));
			ASSERT_THAT(IsTrue(Animations.Num() == 0, TEXT("省略 → 空集")));
			ASSERT_THAT(IsFalse(Report.HasErrors(), TEXT("省略不该产生错误")));
		}

		{
			FForgeDocument Document;
			ASSERT_THAT(IsTrue(ParseDocument(SimpleRoot(), TEXT(R"("animations": [])"), Document), TEXT("测试文档本身应能解析")));
			ASSERT_THAT(IsTrue(UmgAnimationCodec::ParseAnimations(Document, Animations, Report), TEXT("空数组是合法的")));
			ASSERT_THAT(IsTrue(Animations.Num() == 0, TEXT("空数组 → 空集")));
		}
	}

	//~ 计划（按名增量，比较在帧层） -------------------------------------------

	TEST_METHOD(相同内容得到空计划)
	{
		TArray<UmgAnimationCodec::FUmgAnimation> Target;
		Target.Add(MakeFadeAnimation(TEXT("Intro"), 0.6, 1.0));

		TArray<FForgePlanEntry> Plan;
		UmgAnimationCodec::ComputePlan(Target, Target, Plan);
		ASSERT_THAT(IsTrue(Plan.Num() == 0, TEXT("配置与资产一致时计划为空（收敛）")));
	}

	TEST_METHOD(帧层比较抵消浮点假差异)
	{
		// 0.6 与 0.6000000001 落在同一帧（36000），计划不该因此判为「要重建」。
		TArray<UmgAnimationCodec::FUmgAnimation> Target;
		Target.Add(MakeFadeAnimation(TEXT("Intro"), 0.6000000001, 1.0));

		TArray<UmgAnimationCodec::FUmgAnimation> Current;
		Current.Add(MakeFadeAnimation(TEXT("Intro"), 0.6, 1.0));

		TArray<FForgePlanEntry> Plan;
		UmgAnimationCodec::ComputePlan(Target, Current, Plan);
		ASSERT_THAT(IsTrue(Plan.Num() == 0, TEXT("换算到整数帧后，等价的秒不该产生差异")));
	}

	TEST_METHOD(计划给出新建重建与删除三种动作)
	{
		TArray<UmgAnimationCodec::FUmgAnimation> Target;
		Target.Add(MakeFadeAnimation(TEXT("Intro"), 0.6, 1.0));   // 与资产里的同名但值不同 → Update
		Target.Add(MakeFadeAnimation(TEXT("Fresh"), 0.3, 1.0));   // 资产里没有 → Create

		TArray<UmgAnimationCodec::FUmgAnimation> Current;
		Current.Add(MakeFadeAnimation(TEXT("Intro"), 0.6, 0.5));
		Current.Add(MakeFadeAnimation(TEXT("Stale"), 0.6, 1.0));  // 配置里没有 → Delete

		TArray<FForgePlanEntry> Plan;
		UmgAnimationCodec::ComputePlan(Target, Current, Plan);

		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Create) == 1, TEXT("缺的建")));
		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Update) == 1, TEXT("内容不同的整体重建")));
		ASSERT_THAT(IsTrue(CountAction(Plan, EForgeAction::Delete) == 1, TEXT("配置里没有的删")));
	}

	TEST_METHOD(轨道顺序不同不算差异)
	{
		// 资产里的轨道顺序由引擎的 possessable 决定，与配置写的先后无关——按下标比会让每次导入都报 Update。
		UmgAnimationCodec::FUmgAnimation Configured;
		Configured.Name = TEXT("Intro");
		Configured.Tracks.Add(MakeTrack(TEXT("Root/Title"), TEXT("RenderOpacity"), 0.6, 1.0));
		Configured.Tracks.Add(MakeTrack(TEXT("Root/Title"), TEXT("slot.Padding.Bottom"), 0.6, 12.0));

		UmgAnimationCodec::FUmgAnimation InAsset;
		InAsset.Name = TEXT("Intro");
		InAsset.Tracks.Add(MakeTrack(TEXT("Root/Title"), TEXT("slot.Padding.Bottom"), 0.6, 12.0));
		InAsset.Tracks.Add(MakeTrack(TEXT("Root/Title"), TEXT("RenderOpacity"), 0.6, 1.0));

		TArray<UmgAnimationCodec::FUmgAnimation> Target;
		Target.Add(Configured);
		TArray<UmgAnimationCodec::FUmgAnimation> Current;
		Current.Add(InAsset);

		TArray<FForgePlanEntry> Plan;
		UmgAnimationCodec::ComputePlan(Target, Current, Plan);
		ASSERT_THAT(IsTrue(Plan.Num() == 0, TEXT("轨道集合无序，顺序不同不该判为差异")));
	}

	TEST_METHOD(值的float精度差不算差异)
	{
		// 通道是 FMovieSceneFloatChannel：配置写 0.9，资产读回的是 0.9f（0.8999999761581421）。
		TArray<UmgAnimationCodec::FUmgAnimation> Target;
		Target.Add(MakeFadeAnimation(TEXT("Intro"), 0.6, 0.9));

		TArray<UmgAnimationCodec::FUmgAnimation> Current;
		Current.Add(MakeFadeAnimation(TEXT("Intro"), 0.6, 0.8999999761581421));

		TArray<FForgePlanEntry> Plan;
		UmgAnimationCodec::ComputePlan(Target, Current, Plan);
		ASSERT_THAT(IsTrue(Plan.Num() == 0, TEXT("double 与 float 之间的精度差不该判为差异")));
	}
};

#endif // WITH_AUTOMATION_WORKER
