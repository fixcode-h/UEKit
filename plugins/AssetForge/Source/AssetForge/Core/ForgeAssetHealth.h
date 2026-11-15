#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

/**
 * 导入后的**资产健康检查**：把资产自己的报错（蓝图编译、材质编译）收进导入报告。
 *
 * 为什么需要它：管道的「导入成功」只说明**结构写进去了**，不代表**资产本身能用**——
 * 蓝图可能编译不过、材质可能有表达式错误。没有这一步，坏资产会静默通过
 * （2026-09-15 需求侧反馈：`M_ForgeShowcase` 在编辑器里报错而导入端一无所知）。
 *
 * 失败语义：检查出 Error 时调用方应把报告判为**失败**——「写进去了但坏了」不算导入成功。
 */
namespace ForgeAssetHealth
{
	/** 诊断的字段路径固定值：健康检查不是配置里的某个字段，用它把来源标出来。 */
	extern ASSETFORGE_API const TCHAR* const FieldPath;

	/** 就地检查一个资产；错误/警告写进 OutDiagnostics。不认识的类型静默跳过。 */
	ASSETFORGE_API void CheckAsset(UObject* InAsset, TArray<FForgeDiagnostic>& OutDiagnostics);
}
