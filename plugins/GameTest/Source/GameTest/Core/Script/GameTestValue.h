// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class FJsonValue;
class UObject;

/** 值的类型。和协议第 3 节一一对应：条件、表达式、接口、invoke 全用这一套，不各自转 JSON。 */
enum class EGameTestValueType : uint8
{
	Null,
	Bool,
	Number,
	String,
	Array,
	/** 键值对（JSON 对象、结构体、Map、Lua 表）。 */
	Map,
	Vector,
	/** UObject 弱引用：对象被销毁后自动变成取不到，不会悬空。 */
	Object,
	/** 取不到（对象不在、字段不存在、下标越界）：带原因，条件里按不成立算，set / 参数里让这一步失败。 */
	Unresolved,
};

/**
 * 脚本里的一个值（GameTest 1.9.0 脚本化的公共货币）。
 *
 * 为什么不直接用 FJsonValue：JSON 没有向量和对象引用，也表达不了「取不到 + 原因」；
 * 每个扩展点各转一次 JSON 会在比较规则（大小写、数字字符串）上各写一套，迟早对不上。
 *
 * 数组和 Map 用共享的只读存储：值在表达式里频繁拷贝（取成员、传参），拷贝只加引用计数。
 * 要改就整份换掉（`FromArray` / `FromMap` 重新构造），没有原地修改，不会出现两处共享一份被改。
 */
struct GAMETEST_API FGameTestValue
{
	EGameTestValueType Type = EGameTestValueType::Null;

	//~ 构造 --------------------------------------------------------------
	static FGameTestValue Null();
	static FGameTestValue FromBool(bool bIn);
	static FGameTestValue FromNumber(double InNumber);
	static FGameTestValue FromString(const FString& InText);
	static FGameTestValue FromArray(TArray<FGameTestValue>&& InItems);
	static FGameTestValue FromArray(const TArray<FGameTestValue>& InItems);
	static FGameTestValue FromMap(TMap<FString, FGameTestValue>&& InFields);
	static FGameTestValue FromMap(const TMap<FString, FGameTestValue>& InFields);
	static FGameTestValue FromVector(const FVector& InVector);
	/** 空指针得到 Null（不是 Unresolved）：「这个属性是空的」本身是一个有效的值。 */
	static FGameTestValue FromObject(UObject* InObject);
	/**
	 * 取不到。`InReason` 是给人看的中文（「@actor[tag=Boss] 没有找到」），`InCode` 是协议第 12 节的错误码。
	 */
	static FGameTestValue Unresolved(const FString& InReason, FName InCode = TEXT("unresolved"));

	//~ 读 ----------------------------------------------------------------
	bool IsNull() const { return Type == EGameTestValueType::Null; }
	bool IsUnresolved() const { return Type == EGameTestValueType::Unresolved; }
	bool IsNumber() const { return Type == EGameTestValueType::Number; }
	bool IsString() const { return Type == EGameTestValueType::String; }

	bool GetBool() const { return bValue; }
	double GetNumber() const { return Number; }
	/** String 的文字；Unresolved 时是原因。别的类型返回空串，要显示用 ToDisplay。 */
	const FString& GetString() const { return Text; }
	const FVector& GetVector() const { return Vec; }
	FName GetCode() const { return Code; }
	/** Object 类型且对象还活着时返回它，否则 nullptr。 */
	UObject* GetObject() const;

	/** Array 的元素；不是数组时返回空数组。 */
	const TArray<FGameTestValue>& GetArray() const;
	/** Map 的字段；不是 Map 时返回空表。 */
	const TMap<FString, FGameTestValue>& GetMap() const;

	/** 按名字取 Map 字段（忽略大小写，和蓝图属性名一致）；Vector 支持 X/Y/Z。取不到返回 false。 */
	bool TryGetField(const FString& InName, FGameTestValue& OutValue) const;

	/** 长度：数组元素数、Map 字段数、字符串字数；别的类型 -1。 */
	int32 Len() const;

	/**
	 * 真值：非 0 数字、非空字符串（且不是 "false" / "0"）、true、非空数组 / Map、活着的对象、任意向量。
	 * Null 和 Unresolved 为假。和协议第 4.1 节「没写比较时按真值算」一致。
	 */
	bool Truthy() const;

	/** 转数字：数字直接给；布尔 1/0；字符串按整段解析（"12" 可以，"12 金币" 不行——从文字里取数用 number()）。 */
	bool TryNumber(double& OutNumber) const;

	/** 给人看的一句，超过 `InMaxChars` 截断加「…」（报告、失败信息、调试面板）。 */
	FString ToDisplay(int32 InMaxChars = 200) const;

	/** 类型名（`number`、`string`、`object`…），进报告和 `gt.resolve`。 */
	FString TypeName() const;

	/**
	 * 宽松相等（协议第 3 节）：数字和数字字符串按数字比，字符串忽略大小写，向量按分量 1e-3 容差，
	 * 对象按是不是同一个，数组 / Map 逐项比。任一边 Unresolved 都不相等。
	 */
	bool LooseEquals(const FGameTestValue& InOther) const;

	/**
	 * 大小比较：两边都能转数字时按数字，否则两边都是字符串时按忽略大小写的字典序。
	 * 比不了（类型不搭、Unresolved）返回 false，`OutOrder` 不动。
	 */
	bool Compare(const FGameTestValue& InOther, int32& OutOrder) const;

	//~ JSON ---------------------------------------------------------------
	/** 转 JSON：向量写成 {X,Y,Z}，对象写成路径字符串，Unresolved 写成 null（原因另外报）。 */
	TSharedPtr<FJsonValue> ToJson() const;
	/** 从 JSON 来：对象 → Map，数组 → Array，不认 `$` / `=` 这类引用（那是表达式求值的事）。 */
	static FGameTestValue FromJson(const TSharedPtr<FJsonValue>& InJson);

private:
	bool bValue = false;
	double Number = 0.0;
	/** String 的文字，或 Unresolved 的原因。 */
	FString Text;
	FName Code;
	FVector Vec = FVector::ZeroVector;
	TWeakObjectPtr<UObject> Obj;
	TSharedPtr<const TArray<FGameTestValue>> Items;
	TSharedPtr<const TMap<FString, FGameTestValue>> Fields;
};
