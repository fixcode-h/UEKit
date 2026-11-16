// 引擎版本差异只写在这里（UEKit 约定）：业务代码只用这里定义的宏 / 类型，不直接判断 ENGINE_*_VERSION。
// AssetForge 以 UE 5.4 为基准开发，同时兼容 5.8：默认写 5.4 的写法，只有 5.8 才有的接口在这里用版本判断包住。
// 新接口只在实测编过的 5.8 上用（UE_VERSION_NEWER_THAN(5, 7, 99)），中间版本走 5.4 的写法，不去猜哪一版加的。
// 状态树整块只在 5.8 上编，开关 ASSETFORGE_WITH_STATETREE 在 AssetForge.Build.cs 里按 Target.Version 定义。
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/EngineVersionComparison.h"

#include <type_traits>

#if UE_VERSION_OLDER_THAN(5, 4, 0)
	#error "AssetForge 需要 UE 5.4 或更新的版本"
#endif

#ifndef ASSETFORGE_WITH_STATETREE
	#define ASSETFORGE_WITH_STATETREE 0
#endif

/**
 * FJsonObject::Values 的键类型：5.4 是 FString，5.8 换成了 UE::FSharedString（不能和 FString 互相隐式转换）。
 * 直接从 Values 推出来，不按版本号猜；查找时用 FForgeJsonKey(Key) 构造键。
 */
using FForgeJsonKey = std::decay_t<decltype((*DeclVal<FJsonObject&>().Values.begin()).Key)>;

/** 遍历 Values 时把键转成 FString：两种键的 operator* 都给 const TCHAR*。遍历写 `for (const auto& Pair : X->Values)` */
#define ASSETFORGE_JSON_KEY(Key) FString(*(Key))

/** UWorldPartitionSettings 的「新建地图默认开不开世界分区」：5.4 没有这组设置，新建空白关卡不开世界分区 */
#define ASSETFORGE_HAS_NEW_MAP_WORLD_PARTITION_SETTINGS UE_VERSION_NEWER_THAN(5, 7, 99)
