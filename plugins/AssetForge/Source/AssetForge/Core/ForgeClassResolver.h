#pragma once

#include "CoreMinimal.h"

/**
 * 类引用解析：把配置里的「父类 / 控件类」字符串解析成已加载的 UClass。
 *
 * 存在的理由：UnrealSharp 的 C# 类**不是包资产**——它们由托管程序集在运行时创建
 * （`UCSClass : UBlueprintGeneratedClass`，outer 在 `/Script/UnrealSharp` 脚本包下），
 * `LoadClass` 对它们的命中与否取决于加载时机；而配置（由导出得到）里写的正是这类路径
 * （形如 `/Script/UnrealSharp.UIWidgetBase_C`）。因此除了常规加载，还必须提供
 * 「按类名在已加载类里查找」的兜底，并在失败时给出可操作的诊断（例如"C# 程序集未编译"）。
 *
 * 只读辅助：不修改任何状态，失败返回 nullptr 并把中文原因写进 OutError。
 */
struct ASSETFORGE_API FForgeClassResolver
{
	/**
	 * 解析类引用。
	 *
	 * 接受两类写法：
	 * - 全路径：`/Script/UMG.UserWidget`、`/Game/UI/HUD/WBP_X.WBP_X_C`（`_C` 后缀可省）
	 * - 短名：`TextBlock`（仅在 `InShortNamePrefixes` 给定的前缀下尝试，见 ResolveShortName）
	 *
	 * @param InClassReference 类引用字符串
	 * @param InRequiredBase 目标基类（结果必须派生自它；nullptr 表示不限制）
	 * @param OutError 失败原因（面向使用者的中文诊断）
	 */
	static UClass* Resolve(const FString& InClassReference, UClass* InRequiredBase, FString& OutError);

	/**
	 * 短名解析：依次尝试 `InPrefixes` 里的每个前缀（形如 `/Script/UMG.`），都失败后
	 * 再按"已加载类里同名"兜底。短名不做全路径解析——那是 Resolve 的事。
	 */
	static UClass* ResolveShortName(const FString& InShortName, const TArray<FString>& InPrefixes, UClass* InRequiredBase, FString& OutError);

private:
	/** 按"类名"在已加载的 UClass 里查找（UnrealSharp 的运行时类只有这条路）。同名多个视为失败并记日志。 */
	static UClass* FindLoadedClassByName(const FString& InClassName, UClass* InRequiredBase);

	/** 从类引用里取"短名"（最后一个 `.` / `/` 之后），并去掉 `_C` 后缀。 */
	static FString ExtractShortName(const FString& InClassReference);
};
