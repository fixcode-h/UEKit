// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FProperty;
struct FGameTestValue;

/**
 * 属性类型分类（docs/game-test-ref.md 第 3.4 节的 kind）：比较按它选、`gt.props` 按它列可用的 op。
 * 面板 `shared/gt-script/ref/compare.ts` 有同名同表的一份，名字改了两边一起改。
 */
enum class EGameTestKind : uint8
{
	/** 委托、接口、原始指针：不能当标识也不能判断。 */
	None,
	Bool,
	Int,
	Float,
	String,
	Text,
	Enum,
	/** ESlateVisibility（enum 的特例，多 visible / hidden）。 */
	Visibility,
	Object,
	Asset,
	Vector,
	Rotator,
	Vector2,
	Color,
	Array,
	/** 其他结构体：不能直接比，展开成员选。 */
	Struct,
};

struct GAMETEST_API FGameTestPropKind
{
	/** 反射属性的 kind。`OutWhy` 在 None / Struct 时写为什么不能比。 */
	static EGameTestKind Of(const FProperty* InProp, FString* OutWhy = nullptr);

	/** 没有反射属性时（Lua 值、算出状态之外的运行时值）按值推（协议 3.4 末段）。 */
	static EGameTestKind OfValue(const FGameTestValue& InValue);

	/** `bool`、`int`…，和协议表格一致。 */
	static const TCHAR* Name(EGameTestKind InKind);

	/** 名字 → kind（`---@field` 映射、面板传来的 kind）；不认识返回 false。 */
	static bool FromName(const FString& InName, EGameTestKind& OutKind);

	/** 能不能直接判断（struct / none 不能）。 */
	static bool IsComparable(EGameTestKind InKind);

	/** 引擎类型名（`int32`、`FText`、`TArray<FItem>`），`gt.props` 的 type。 */
	static FString TypeName(const FProperty* InProp);

	/** 两个 kind 对比较来说算不算同一类：int 收 float 的 op，visibility 收 enum 的 op（协议 3.4）。 */
	static bool Accepts(EGameTestKind InOpKind, EGameTestKind InValueKind);
};
