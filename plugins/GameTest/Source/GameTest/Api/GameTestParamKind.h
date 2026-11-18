// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FProperty;
class UFunction;

/**
 * 一个函数参数能不能从脚本里给（协议 docs/game-test-script.md 第 6 节「参数类型」表的唯一实现）。
 *
 * invoke 打包参数、gt.vars 标 `constructible` / `callable`、C++ 项目接口扫描（`_api.json` 的 `rejected`）
 * 都按这一份判断，免得三处各写一套、说法对不上。
 */
enum class EGameTestParamKind : uint8
{
	/** 能直接从 JSON 值、表达式构造：数字、字符串、枚举、可构造的结构体、容器、资产路径。 */
	Value,
	/** UObject：只能传「能重新找到的」引用（`{"$obj"}`、`$var`、`{"target"}`、`@来源`）。 */
	Object,
	/** WorldContext 参数：调用时自动填当前世界，脚本不用给。 */
	WorldContext,
	/** 委托、接口、FFieldPath 这类：不支持。 */
	Unsupported,
};

struct GAMETEST_API FGameTestParamInfo
{
	FString Name;
	/** 类型描述（int32、FVector、TArray<FItem>、AActor*…），和 UELink 的 TypeOf 一致。 */
	FString Type;
	EGameTestParamKind Kind = EGameTestParamKind::Value;
	/** 输出参数（非 const 引用）：不用给，调完存进返回值。 */
	bool bOut = false;
	/** Kind 是 Unsupported 时为什么。 */
	FString Why;
};

class GAMETEST_API GameTestParamKind
{
public:
	/** 一个属性（参数或结构体字段）是哪一类；Unsupported 时 OutWhy 写原因。 */
	static EGameTestParamKind Classify(const FProperty* InProp, FString& OutWhy);

	/**
	 * 函数的参数表（不含返回值），WorldContext 参数按 `meta=(WorldContext=…)` 认（编辑器里读元数据；
	 * 游戏进程没有元数据时按参数名 WorldContextObject 认）。
	 */
	static TArray<FGameTestParamInfo> Describe(const UFunction& InFunction);

	/** 有没有脚本给不了的参数（输出参数不算）；有时 OutWhy 写第一条的原因。 */
	static bool IsCallable(const UFunction& InFunction, FString& OutWhy);

	/** 返回值类型描述；没有返回值时为空。 */
	static FString ReturnType(const UFunction& InFunction);

	/** `[{name, type, constructible, object?, out?, auto?, why?}]`，gt.vars 的 args 和 _api.json 的 args 共用。 */
	static TArray<TSharedPtr<class FJsonValue>> ToJson(const TArray<FGameTestParamInfo>& InParams);
};
