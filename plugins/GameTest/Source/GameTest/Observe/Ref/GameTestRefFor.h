// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Observe/Ref/GameTestRef.h"

class FGameTestFlowContext;
class FJsonObject;
class UObject;
class UUserWidget;
class UWidget;

/** 从游戏里的一个东西出引用的结果（gt.refFor、gt.pick、gt.inspect、录制共用）。 */
struct GAMETEST_API FGameTestRefForResult
{
	/** 当对象的那个（最近的 UUserWidget、Actor）。 */
	TWeakObjectPtr<UObject> Object;
	/** 推荐的引用。 */
	TSharedPtr<FJsonObject> Ref;
	/** 点到的是对象里的部件时：`{obj: Ref, part}`；否则空。 */
	TSharedPtr<FJsonObject> Target;
	FString Part;
	TArray<FGameTestIdentItem> Ident;
	/** 按 Ref 此刻命中几个。 */
	int32 Matches = 0;
	/** 同类有几个（同 in）。 */
	int32 Siblings = 0;
	/** 挑不准（没有能区分的组合）：录制要停下来问人，面板弹属性树。 */
	bool bAsk = false;
};

/**
 * 从活的对象出引用（docs/game-test-ref.md 7.4、第 8 节）：控件 → 最近的 UUserWidget 当对象、控件在它类树里的名字当 part；
 * 对象是 ListView 条目时 `in: {obj: 列表所在界面, part: 列表}`；ident 同类只有一个时为空，有 identRules 按规则，否则按打分挑最少的能区分的组合。
 */
namespace GameTestRefFor
{
	/** 控件所在的最近的 UUserWidget（自己是 UUserWidget 时是外层那个）。 */
	GAMETEST_API UUserWidget* OwnerOf(const UWidget* InWidget);

	/** 对象（UUserWidget / Actor / 组件以外的 UObject）本身的引用。 */
	GAMETEST_API bool ForObject(UObject* InObject, FGameTestFlowContext& InContext, FGameTestRefForResult& OutResult);

	/**
	 * 点到的控件 → 对象 + part。`InLevel` 是命中链里第几层当对象（0 = 自动：最近的 UUserWidget；1 = 再外一层…）。
	 * 运行时生成、写不出部件名的控件往外找能写的。
	 */
	GAMETEST_API bool ForWidget(UWidget* InWidget, FGameTestFlowContext& InContext, FGameTestRefForResult& OutResult, int32 InLevel = 0);

	/**
	 * `{chain, ref?, target?, matches, ask}`（gt.refFor、gt.pick at、gt.inspect、录制问标识共用）。
	 * `InNodeId` 给了时链上每层带 gt.tree 的临时 id（`node`）。
	 */
	GAMETEST_API TSharedRef<FJsonObject> ToJson(UWidget* InWidget, FGameTestFlowContext& InContext, int32 InLevel = 0, TFunction<FString(const UWidget*)> InNodeId = nullptr);

	/** 命中链（从点到的往外：控件 → 条目 / 界面 → 外层界面…），每层 `{class, kind, part?, note, widget}`。 */
	GAMETEST_API void Chain(UWidget* InWidget, TArray<TSharedPtr<FJsonObject>>& OutChain, TArray<UWidget*>* OutWidgets = nullptr);
}
