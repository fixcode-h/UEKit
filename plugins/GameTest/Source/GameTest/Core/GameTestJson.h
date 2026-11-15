// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * JSON 读写的小工具。
 *
 * 注释剥离与 AssetForge 的 `ForgeJsonText` 同一约定（`//` 行注释在字符串外才剥），
 * 但那份实现住在 Editor-only 的模块里，Runtime 侧不能依赖它，所以这里独立一份。
 * 引擎自带的 JSON 解析器**不支持注释**（实测），不剥就会整份配置解析失败。
 *
 * 做成类而不是命名空间：测试模块（GameTestTests）要跨 DLL 调这些函数，
 * 类上的导出宏一次性覆盖全部成员，免得每加一个工具函数就撞一次链接错误。
 */
class GAMETEST_API GameTestJson
{
public:
	/** 剥掉字符串外的 `//` 行注释。 */
	static FString StripComments(const FString& InText);

	static TSharedPtr<FJsonObject> ParseObjectFromText(const FString& InText, FString& OutError);

	static TSharedPtr<FJsonObject> ParseObjectFromFile(const FString& InAbsolutePath, FString& OutError);

	/** 把文件读成文本；失败时 OutError 带系统错误。 */
	static bool ReadTextFile(const FString& InAbsolutePath, FString& OutText, FString& OutError);

	static bool TryGetString(const FJsonObject& InObject, const TCHAR* InField, FString& OutValue);
	static bool TryGetInt(const FJsonObject& InObject, const TCHAR* InField, int32& OutValue);
	static bool TryGetNumber(const FJsonObject& InObject, const TCHAR* InField, double& OutValue);
	static bool TryGetBool(const FJsonObject& InObject, const TCHAR* InField, bool& OutValue);
	static TSharedPtr<FJsonObject> TryGetObject(const FJsonObject& InObject, const TCHAR* InField);
	static const TArray<TSharedPtr<FJsonValue>>* TryGetArray(const FJsonObject& InObject, const TCHAR* InField);

	/** 单行字符串，进日志用。 */
	static FString ToCompactString(const TSharedPtr<FJsonObject>& InJson);

	/** 带缩进的字符串，落盘给人读。 */
	static FString ToPrettyString(const TSharedPtr<FJsonObject>& InJson);

	static FString ToCompactString(const TArray<TSharedPtr<FJsonValue>>& InArray);
};
