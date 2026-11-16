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
