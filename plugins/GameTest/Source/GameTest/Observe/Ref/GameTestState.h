// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestRegistry.h"
#include "Core/Script/GameTestValue.h"
#include "Observe/Ref/GameTestPropKind.h"

class FGameTestFlowContext;
class UListView;
class UObject;
// 前置声明放在全局：写在命名空间里的 `class UWidget*` 会声明出 GameTestStateUtil::UWidget，和引擎的 UWidget 不是一个类型
class UWidget;

/** 算出状态适用于什么（`gt.props` 的状态组只列适用的）。 */
enum class EGameTestStateScope : uint8
{
	Any,
	/** UWidget（界面、部件）。 */
	Widget,
	/** AActor 和场景组件。 */
	Actor,
};

/** 读状态时的环境。 */
struct GAMETEST_API FGameTestStateContext
{
	FGameTestFlowContext& Flow;
	/** 条目的数据对象（`:selected`、`:index` 用）；不是条目时为空。 */
	UObject* Item = nullptr;
	/** 条目在容器里的位置（引用解析给的）；不知道时 INDEX_NONE。 */
	int32 Index = INDEX_NONE;

	explicit FGameTestStateContext(FGameTestFlowContext& InFlow)
		: Flow(InFlow)
	{
	}
};

/** 读到的状态：值 + 文字的本地化键（`:text` 给，比较 `key` 用）。 */
struct GAMETEST_API FGameTestStateValue
{
	FGameTestValue Value;
	FString TextKey;
};

/**
 * 一种算出状态（docs/game-test-ref.md 第 3.2 节）：`:onScreen`、`:text`、`:checked`……
 * 一种一个文件（`Observe/Ref/States/GameTestState<名字>.cpp`），登记进 FGameTestStateRegistry。
 * 读不到（不适用、没有键）返回 Unresolved，判断按不成立算。
 */
class GAMETEST_API IGameTestState
{
public:
	virtual ~IGameTestState() = default;

	/** 不带冒号：`onScreen`。 */
	virtual FName GetKey() const = 0;

	virtual EGameTestStateScope GetScope() const = 0;

	virtual EGameTestKind GetKind() const = 0;

	virtual FString GetSummary() const = 0;

	/** 能不能当标识（`:index`、`:pos` 这类面板标「不建议」）：影响 `gt.props` 的 score。 */
	virtual bool IsStable() const { return true; }

	/** 这个对象适不适用（`gt.props` 只列适用的）。 */
	virtual bool AppliesTo(const UObject* InObject) const;

	virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const = 0;
};

class GAMETEST_API FGameTestStateRegistry
{
public:
	static TGameTestRegistry<IGameTestState>& Get();

	static const IGameTestState* Find(const FString& InName);
};

/** 状态实现共用的小工具。 */
namespace GameTestStateUtil
{
	/** 对象是控件时给控件（UUserWidget 也算），否则空。 */
	GAMETEST_API UWidget* WidgetOf(UObject* InObject);

	/** Actor 或场景组件的世界位置；都不是返回 false。 */
	GAMETEST_API bool LocationOf(const UObject* InObject, FVector& OutLocation);

	/** 不适用时的统一 Unresolved。 */
	GAMETEST_API FGameTestStateValue NotApplicable(const UObject* InObject, const TCHAR* InState);

	/** 控件的上一层：面板里的父控件；WBP 树的根往外到这个 WBP 实例（它又在外层 WBP 的树里）。 */
	GAMETEST_API UWidget* ParentOf(const UWidget* InWidget);

	/** 显示文字和本地化键（文字类控件直接取 FText；按钮、容器取里面第一个文字控件）。取不到文字控件返回 false。 */
	GAMETEST_API bool TextWidgetText(UWidget* InWidget, FText& OutText);

	/** 条目控件所在的列表和它的数据对象（不是 ListView 族条目返回空）。 */
	GAMETEST_API UListView* OwningList(UObject* InObject, UObject*& OutItem);

	/** 取值结果包一层。 */
	GAMETEST_API FGameTestStateValue Of(const FGameTestValue& InValue);
}
