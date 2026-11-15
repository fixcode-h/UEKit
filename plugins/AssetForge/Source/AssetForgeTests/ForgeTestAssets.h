// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/PackageName.h"

/**
 * 需要「真实资产」的用例用的资产路径：都放在 `/Game/AssetForgeTest/` 下。
 *
 * 插件不带项目资产（骨骼、动画、纹理这些只能由项目提供），所以这些用例在资产不存在时跳过，
 * 不算失败。想跑它们就在项目里放一份同名资产（任意内容，类型对上即可）。
 */
namespace ForgeTestAssets
{
	inline const TCHAR* const Skeleton = TEXT("/Game/AssetForgeTest/SKEL_ForgeTest");
	inline const TCHAR* const SkeletalMesh = TEXT("/Game/AssetForgeTest/SK_ForgeTest");
	inline const TCHAR* const PhysicsAsset = TEXT("/Game/AssetForgeTest/SK_ForgeTest_PhysicsAsset");
	inline const TCHAR* const AnimSequence = TEXT("/Game/AssetForgeTest/A_ForgeTest_Idle");
	inline const TCHAR* const Texture = TEXT("/Game/AssetForgeTest/T_ForgeTest_LUT");
	inline const TCHAR* const Font = TEXT("/Game/AssetForgeTest/F_ForgeTest");
	inline const TCHAR* const DataAsset = TEXT("/Game/AssetForgeTest/DA_ForgeTest");
}

/** 在 TEST_METHOD 开头用：资产不存在就记一条说明并跳过。 */
#define FORGE_SKIP_WITHOUT_ASSET(Path) \
	if (!FPackageName::DoesPackageExist(Path)) \
	{ \
		AddInfo(FString::Printf(TEXT("跳过：项目里没有测试资产 %s（见 ForgeTestAssets.h）"), Path)); \
		return; \
	}
