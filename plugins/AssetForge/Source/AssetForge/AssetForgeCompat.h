// 引擎版本差异只写在这里（UEKit 约定）：业务代码只用这里定义的宏，不直接判断 ENGINE_*_VERSION。
// AssetForge 现在只支持 UE 5.8（StateTree 的属性绑定、材质编辑接口在 5.8 之前差别很大），
// 以后要往下兼容时，把差异定义成这里的宏或内联函数，调用处不改。
#pragma once

#include "Misc/EngineVersionComparison.h"

#if !UE_VERSION_NEWER_THAN(5, 7, 99)
	#error "AssetForge 需要 UE 5.8 或更新的版本"
#endif
