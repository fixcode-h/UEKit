// 引擎版本差异集中在这里，各操作里不写版本判断。
// 插件以 UE 5.4 为基准开发：默认分支（#else）写 5.4 的写法，5.5+ / 5.8+ 才有的接口用版本判断包住。
// 2.2 起放在 UELinkCore/Public：UEKit 里的扩展插件（AssetForge、GameTest）直接 include，不各抄一份。
#pragma once

#include "Misc/EngineVersionComparison.h"
#include "Runtime/Launch/Resources/Version.h"

// 5.6 起部分图表节点的坐标接口（如 FEdGraphSchemaAction::PerformAction）换成了 FVector2f 系列。
// 注意不是全部：UBlueprintNodeSpawner::Invoke 到 5.8 仍收 FVector2D，而 FVector2f 不能隐式转 FVector2D，
// 用这个宏前先对照目标版本的函数签名。
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
#define UELINK_GRAPH_POS(X, Y) FVector2f(static_cast<float>(X), static_cast<float>(Y))
#else
#define UELINK_GRAPH_POS(X, Y) FVector2D(static_cast<double>(X), static_cast<double>(Y))
#endif

// 5.8 起 FJsonObject::Values 的键从 FString 换成了 UE::FSharedString，不能隐式转 FString。
// 两者的 operator* 都返回 const TCHAR*，按这个取键在各版本都能编过。
#define UELINK_JSON_KEY(Key) FString(*(Key))

// 5.8 起 GetObjectsWithOuter 的 bool bIncludeNestedObjects 弃用，改收 EGetObjectsFlags（UObject/FindObjectFlags.h）
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
#include "UObject/FindObjectFlags.h"
#define UELINK_OUTER_NESTED(bNested) ((bNested) ? EGetObjectsFlags::IncludeNestedObjects : EGetObjectsFlags::None)
#else
#define UELINK_OUTER_NESTED(bNested) (bNested)
#endif

// 是否 5.8 及以上：下面几条 5.8 差异共用这一个判断
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
#define UELINK_UE58 1
#else
#define UELINK_UE58 0
#endif

// 5.8 起 FCoreDelegates::OnPostEngineInit 弃用，改用 GetOnPostEngineInit()；5.4 只有前者
#if UELINK_UE58
#define UELINK_POST_ENGINE_INIT FCoreDelegates::GetOnPostEngineInit()
#else
#define UELINK_POST_ENGINE_INIT FCoreDelegates::OnPostEngineInit
#endif

// 5.8 起 ITimingProfilerProvider::ReadTimers 弃用，改 GetTimerReader()（AgentPanelTrace.cpp）
#define UELINK_TRACE_TIMER_READER UELINK_UE58

// Draw Calls / 图元数的全局数组在 5.4–5.6 一直在；之后的版本 RHI 统计在改，没在真实引擎验证过的版本先不读，网页显示「—」
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION <= 6
#define UELINK_DRAW_STATS 1
#else
#define UELINK_DRAW_STATS 0
#endif
