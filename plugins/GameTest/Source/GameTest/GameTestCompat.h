// 引擎版本差异只写在这里（UEKit 约定）：业务代码只用这里定义的宏，不直接判断 ENGINE_*_VERSION。
// GameTest 以 UE 5.4 为基准开发：默认写 5.4 的写法，5.5+ / 5.8+ 才有的接口在这里用版本判断包住，调用处只用宏。
#pragma once

#include "Misc/EngineVersionComparison.h"

#if UE_VERSION_OLDER_THAN(5, 4, 0)
	#error "GameTest 需要 UE 5.4 或更新的版本"
#endif

// FJsonObject::Values 的键：5.4 是 FString，5.8 换成了 UE::FSharedString（不能隐式转 FString）。
// 两者的 operator* 都返回 const TCHAR*，按这个取键在各版本都能编过；遍历时用 const auto& 接 Pair。
#define GAMETEST_JSON_KEY(Key) FString(*(Key))

// FStringOutputDevice：5.4 声明在 Containers/UnrealString.h（Core 头里已带上），之后单独搬进 Misc/StringOutputDevice.h。
// 按头文件在不在来 include，不猜具体是哪一版搬的（5.4 上 include 新路径会 C1083）。
#if __has_include("Misc/StringOutputDevice.h")
	#include "Misc/StringOutputDevice.h"
#else
	#include "Containers/UnrealString.h"
#endif

// UEnhancedInputLocalPlayerSubsystem::HasContinuousInputInjectionForAction：5.4 没有（C2039）。
// 只在确认编过的 5.8 用引擎的查询，更早的版本由 GameTestInjection 自己记哪些动作在注入。
#define GAMETEST_HAS_INJECTION_QUERY UE_VERSION_NEWER_THAN(5, 7, 99)
