// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Api/GameTestApi.h"
#include "Core/Script/GameTestValue.h"

class AActor;
class FGameTestFlowContext;
class UClass;
class UObject;
class UWorld;
struct FGameTestApiCall;
struct FGameTestObjectLocator;
struct FGameTestWidgetLook;

/**
 * 自带接口的参数转换（协议第 8.1 节的 obj / sel / cls / loc / vector）。
 *
 * 为什么集中一份：几十个接口都收「对象」「控件」「类」参数，每个接口自己判类型，
 * 一个认 "@pawn"、另一个不认，脚本作者就得记每个接口的脾气。这里定一次规则，接口只管业务。
 */
namespace GameTestApiArgs
{
	/** 登记时写参数：`Req(TEXT("target"), TEXT("sel"))`、`Opt(TEXT("client"), TEXT("int"), TEXT("默认当前客户端"))`。 */
	inline FGameTestApiArg Req(const TCHAR* InName, const TCHAR* InType, const TCHAR* InNote = TEXT(""))
	{
		FGameTestApiArg Arg;
		Arg.Name = InName;
		Arg.Type = InType;
		Arg.Note = InNote;
		return Arg;
	}

	inline FGameTestApiArg Opt(const TCHAR* InName, const TCHAR* InType, const TCHAR* InNote = TEXT(""))
	{
		FGameTestApiArg Arg = Req(InName, InType, InNote);
		Arg.bOptional = true;
		return Arg;
	}

	/**
	 * 对象参数：Object 值；字符串 `@来源…`（`@pawn`、`@sub.UQuestSubsystem`）；`{"target": sel}`（界面 / 控件）；
	 * 已经加载的对象路径（`/Game/…`、`/Script/…`）。取不到返回 nullptr 并写原因。
	 */
	GAMETEST_API UObject* Object(const FGameTestValue& InArg, FGameTestFlowContext& InContext, FString& OutError);

	/**
	 * Actor 参数：同上，组件取它的 Owner。`bDefaultPawn` 时参数为 Null 用本地 Pawn（`Character.*` 默认本地玩家）。
	 */
	GAMETEST_API AActor* Actor(const FGameTestValue& InArg, FGameTestFlowContext& InContext, bool bDefaultPawn, FString& OutError);

	/** 类参数：类名（`BP_X_C`、`ANpc`、前缀可省）、路径或 UClass 对象。 */
	GAMETEST_API UClass* Class(const FGameTestValue& InArg, FString& OutError);

	/** 定位器参数：类名字符串（等于 `{class}`）或定位器对象（第 7 节）。 */
	GAMETEST_API bool Locator(const FGameTestValue& InArg, FGameTestObjectLocator& OutLocator, FString& OutError);

	/** 向量参数：向量、`[x,y,z]`、`{X,Y,Z}`、Actor（取位置）、`@pawn` 这类来源。 */
	GAMETEST_API bool Vector(const FGameTestValue& InArg, FGameTestFlowContext& InContext, FVector& OutVector, FString& OutError);

	/** 控件参数：选择器字符串、`{"target": sel}`；解析不到时 `OutLook.bResolved` 为 false、`Error` 是原因。 */
	GAMETEST_API FGameTestWidgetLook Widget(const FGameTestValue& InArg, FGameTestFlowContext& InContext);

	/** 文字参数：字符串原样，别的类型转显示文字；Null 返回 `InDefault`。 */
	GAMETEST_API FString Text(const FGameTestValue& InArg, const FString& InDefault = FString());

	/** 数字参数：Null 返回 `InDefault`，转不了数字返回 false。 */
	GAMETEST_API bool Number(const FGameTestValue& InArg, double InDefault, double& OutNumber);

	/** 当前世界：没有游戏在跑时写错误（engine_unavailable 语义）并返回 nullptr。 */
	GAMETEST_API UWorld* World(FGameTestFlowContext& InContext, FGameTestApiCall& InOutCall);

	/** `client` 参数（Null = 当前步骤的客户端）对应的世界。 */
	GAMETEST_API UWorld* ClientWorld(const FGameTestValue& InClient, FGameTestFlowContext& InContext, FString& OutError);

	/** 参数错的统一写法：`第 2 个参数 path：要字符串，收到 number 12`（arg_type）。 */
	GAMETEST_API void ArgError(FGameTestApiCall& InOutCall, int32 InIndex, const TCHAR* InName, const FString& InWhy);

	/** 对象的显示名（编辑器里 Actor 用 Label），报告和返回里用。 */
	GAMETEST_API FString NameOf(const UObject* InObject);
}
