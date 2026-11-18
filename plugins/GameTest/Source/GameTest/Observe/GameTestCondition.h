// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UWidget;
struct FGameTestWidgetNode;

/**
 * 控件状态条件（1.6.0）：`assert` 和步骤的「已达成就跳过」（`skipIf`）共用一份判断。
 *
 * **为什么抽出来**：断言是「不成立就失败」，跳过条件是「成立就不做」，问的是同一个问题（这个控件此刻是不是这样）。
 * 两边各写一份，`checked`、文字比较这些细节迟早对不上——录制时推断出来的跳过条件，在断言里就该得到同样的结论。
 *
 * 条件字段（写在同一个对象里，都要成立；`any` 是任一成立）：
 * - `exists` / `visible` / `enabled`、`text`（完全一样）/ `textContains`、`count`、`selected`、`selectedProperty` + `equals`、
 *   `stateKey` + `equals`：1.0 起的断言条件，行为不变。
 * - `textLike`：文字按「完全一样 → 忽略数字和标点后一样」两档比（GameTestTextMatch 的前两档）。**不用第三档「包含」**：
 *   「已领取」包含「领取」，用包含比会把没领的当成领过了。
 * - `checked`：勾选状态（UCheckBox；CommonUI 可选中按钮的选中，GameTestCommonUI 写进快照 state 的 `selected`）。
 * - `field` + `equals`：读控件所在 WBP 的字段，`@self.路径`（WBP 自己的 UPROPERTY）、`@lua.路径`（它的 UnLua 实例表）、
 *   `@item.路径` / `@itemLua.路径`（列表条目的数据对象和它的 Lua 表）。和条目标识同一个读取器（GameTestItemKey），只读不调函数。
 * - `image`：UImage 当前贴图的资源名包含这段（勾是换图而不是显隐的项目）。
 * - `any`：`[条件对象, …]`，任一成立就算成立。
 *
 * 只在游戏线程上用。
 */
struct GAMETEST_API FGameTestConditionCheck
{
	/** 一共查了几项（0 = 没写任何条件）。 */
	int32 Count = 0;
	/** 不成立的项，给人看的一句（「期望 checked=true，实际 false」）。 */
	TArray<FString> Mismatches;
	/** 实际值（「checked=false」），成立不成立都记，失败消息和报告里用。 */
	TArray<FString> Actuals;

	bool Holds() const
	{
		return Count > 0 && Mismatches.Num() == 0;
	}
};

namespace GameTestCondition
{
	/** 条件对象里能写的字段（assert 的字段白名单、skipIf 的校验共用）。 */
	GAMETEST_API const TArray<FString>& Keys();

	/** 这个字段是不是条件。 */
	GAMETEST_API bool IsKey(const FString& InKey);

	/**
	 * 检查条件写得对不对：至少一个条件、值的类型对、`stateKey` / `selectedProperty` / `field` 配了 `equals`、
	 * `any` 是非空的对象数组、`field` 的来源认得。`InExtraAllowed` 是条件之外还允许的字段（skipIf 的 `target`、`also`……），
	 * 不在两张表里的字段报错。
	 */
	GAMETEST_API bool Validate(const FJsonObject& InCond, const TArray<FString>& InExtraAllowed, FString& OutError);

	/**
	 * 判断条件。`InNode` 为空表示目标没解析到（`InResolveError` 是原因）：这时只有 `exists: false` 能成立，
	 * 别的条件都记成不成立。
	 */
	GAMETEST_API void Check(const FJsonObject& InCond, const FGameTestWidgetNode* InNode, const FString& InResolveError, FGameTestConditionCheck& OutCheck);

	/**
	 * 条件里有「不存在 / 不可见」这种反面条件（含 `any` 里的）：目标找不到既可能是「领过了，按钮没了」，
	 * 也可能是「界面还没打开」，跳过条件只在所在的屏已经在屏上时才拿它算数。
	 */
	GAMETEST_API bool HasNegative(const FJsonObject& InCond);

	/** 给人看的一句（「checked=true」「文字像「已领取」」），进步骤结果的消息。 */
	GAMETEST_API FString Describe(const FJsonObject& InCond);

	/** 控件的勾选状态：UCheckBox、快照 state 里的 checked / selected；不是能勾选的控件返回 false。 */
	GAMETEST_API bool ReadChecked(const FGameTestWidgetNode& InNode, bool& OutChecked);

	/** 控件显示的文字：文字控件自己的，按钮这类读它里面（和叠在它上面）的文字，和录制的 hint.text 同一套取法。 */
	GAMETEST_API FString ReadText(const FGameTestWidgetNode& InNode);

	/** UImage 当前贴图的资源名（不是 UImage 或没有贴图时为空）。 */
	GAMETEST_API FString ReadImage(const UWidget* InWidget);

	/**
	 * 读控件的字段：`@self.路径` / `@lua.路径` 读控件所在的 WBP（控件本身是 WBP 时就是它），
	 * `@item.路径` / `@itemLua.路径` 读那个 WBP 作为列表条目显示的数据对象。读不到返回空，`OutWhy` 写原因。
	 */
	GAMETEST_API TSharedPtr<FJsonValue> ReadField(UWidget* InWidget, const FString& InField, FString& OutWhy);
}
