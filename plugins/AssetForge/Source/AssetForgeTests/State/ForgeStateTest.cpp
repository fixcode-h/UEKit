// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "State/ForgeState.h"

#if WITH_AUTOMATION_WORKER

/**
 * 同步状态的守门用例：状态分类、机器可读名、资产路径归一化。
 *
 * 判定本身（QuickState / DeepCheck）依赖磁盘上的配置与资产，由各 kind 的往返用例与手工验证覆盖；
 * 这里钉住的是「哪些状态算需要导入」这类口径——窗口、HTTP、命令行三处都读它。
 */
TEST_CLASS_WITH_FLAGS(ForgeStateTest, "AssetForge.State",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	TEST_METHOD(需要导入的状态口径)
	{
		ASSERT_THAT(IsTrue(ForgeState::NeedsImport(EForgeSyncState::NeverImported), TEXT("从未导入要导入")));
		ASSERT_THAT(IsTrue(ForgeState::NeedsImport(EForgeSyncState::ConfigChanged), TEXT("配置改了要导入")));
		ASSERT_THAT(IsTrue(ForgeState::NeedsImport(EForgeSyncState::AssetMissing), TEXT("资产缺失要导入（重建）")));
		ASSERT_THAT(IsTrue(ForgeState::NeedsImport(EForgeSyncState::Pending), TEXT("深度确认要导入")));
		ASSERT_THAT(IsFalse(ForgeState::NeedsImport(EForgeSyncState::Synced), TEXT("已同步不导入")));
		ASSERT_THAT(IsFalse(ForgeState::NeedsImport(EForgeSyncState::ParseError), TEXT("解析失败导不了")));
		ASSERT_THAT(IsFalse(ForgeState::NeedsImport(EForgeSyncState::Foreign), TEXT("外部配置不归本工具")));
		ASSERT_THAT(IsFalse(ForgeState::NeedsImport(EForgeSyncState::Unmanaged), TEXT("未纳管没有配置可导")));
	}

	TEST_METHOD(只有拿不准的状态才需要深度检查)
	{
		ASSERT_THAT(IsTrue(ForgeState::NeedsDeepCheck(EForgeSyncState::ConfigChanged), TEXT("配置改了可能只是格式变化")));
		ASSERT_THAT(IsTrue(ForgeState::NeedsDeepCheck(EForgeSyncState::AssetChanged), TEXT("资产改了可能语义没变")));
		ASSERT_THAT(IsFalse(ForgeState::NeedsDeepCheck(EForgeSyncState::NeverImported), TEXT("从未导入无需确认")));
		ASSERT_THAT(IsFalse(ForgeState::NeedsDeepCheck(EForgeSyncState::Synced), TEXT("已同步无需确认")));
	}

	TEST_METHOD(机器可读名可往返)
	{
		for (uint8 Index = 0; Index < static_cast<uint8>(EForgeSyncState::Count); ++Index)
		{
			const EForgeSyncState State = static_cast<EForgeSyncState>(Index);
			ASSERT_THAT(IsTrue(ForgeState::FromId(ForgeState::ToId(State)) == State,
				FString::Printf(TEXT("%s 往返后应不变"), ForgeState::ToId(State))));
			ASSERT_THAT(IsFalse(ForgeState::GetLabel(State).IsEmpty(), TEXT("每个状态都要有标签")));
		}
		ASSERT_THAT(IsTrue(ForgeState::FromId(TEXT("no-such-state")) == EForgeSyncState::Unknown, TEXT("认不出的名字给 Unknown")));
	}

	TEST_METHOD(资产路径归一化成包名)
	{
		ASSERT_THAT(AreEqual(FString(TEXT("/Game/UI/WBP_HUD")), ForgeState::NormalizeAssetPath(TEXT("/Game/UI/WBP_HUD")), TEXT("包名原样")));
		ASSERT_THAT(AreEqual(FString(TEXT("/Game/UI/WBP_HUD")), ForgeState::NormalizeAssetPath(TEXT("/Game/UI/WBP_HUD.WBP_HUD")), TEXT("对象路径剥成包名")));
		ASSERT_THAT(AreEqual(FString(TEXT("/Game/UI/WBP_HUD")),
			ForgeState::NormalizeAssetPath(TEXT("/Script/UMGEditor.WidgetBlueprint'/Game/UI/WBP_HUD.WBP_HUD'")),
			TEXT("导出文本剥成包名")));
	}

	TEST_METHOD(没有目标资产时快速判定为无目标)
	{
		ASSERT_THAT(IsTrue(ForgeState::QuickState(TEXT("E:/nowhere/x.json"), FString()) == EForgeSyncState::NoAsset,
			TEXT("配置没写 asset 时不该猜状态")));
		ASSERT_THAT(IsTrue(ForgeState::QuickState(TEXT("E:/nowhere/x.json"), TEXT("/Game/__NoSuchAsset__/X")) == EForgeSyncState::AssetMissing,
			TEXT("资产不存在时是资产缺失")));
	}
};

#endif // WITH_AUTOMATION_WORKER
