// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestValue.h"
#include "Observe/Ref/GameTestPropKind.h"

class FGameTestFlowContext;
class UObject;
class UWidget;

/**
 * 属性路径（docs/game-test-ref.md 第 3.1 节）：统一 `@` 开头，`.` 是对象上存在的数据，`:` 是算出来的状态。
 *
 * | 写法 | 读哪里 |
 * | --- | --- |
 * | `@self.X` | 对象自己的反射属性（C++ UPROPERTY、蓝图变量），后面 `.成员` `[下标]` `["键"]` |
 * | `@部件.X` | 部件（`Header.TxtTitle` 多段）的属性；第一段按部件名找不到再按 self 的属性名找 |
 * | `@lua.X` / `@部件.lua.X` | 绑的 UnLua 实例表 |
 * | `@item.X` / `@itemLua.X` | 列表条目的数据对象 / 它的 Lua 表 |
 * | `@部件:状态` / `:状态` | 算出状态（`:状态` = `@self:状态`） |
 *
 * 解析只拆段，部件和属性的分界要对着对象（或类树）才知道，读的时候再定。
 */
struct GAMETEST_API FGameTestPropPath
{
	/** 原文。 */
	FString Text;
	/** `:` 之前按 `.` 拆开的段（`[下标]` 留在段里）；`:状态` 简写时为空。 */
	TArray<FString> Segments;
	/** 算出状态名（不带 `:`）；没写为空。 */
	FString State;

	/** 拆开；写法不对返回 false 并写原因（括号不配对、空段、状态名不是标识符）。 */
	static bool Parse(const FString& InText, FGameTestPropPath& OutPath, FString& OutError);

	bool HasState() const { return !State.IsEmpty(); }

	/** 第一段是不是固定来源（self / lua / item / itemLua）。 */
	static bool IsSourceWord(const FString& InSegment);
};

/** 读到的一个属性值（判断、`gt.props`、`obj.set` 共用）。 */
struct GAMETEST_API FGameTestPropRead
{
	/** 值；读不到时是 Unresolved，原因写哪一段读不到。 */
	FGameTestValue Value;
	EGameTestKind Kind = EGameTestKind::None;
	/** 引擎类型名（`int32`、`FText`）；Lua、状态写 lua / state 的类型。 */
	FString Type;
	/** `cpp` / `bp` / `lua` / `state`。 */
	FString Src;
	/** FText 的本地化键（`命名空间,键`）：比较 `key` 用；没有键为空。 */
	FString TextKey;
	/** 只读（委托、算出状态、BlueprintReadOnly）：`obj.set` 报 prop_readonly。 */
	bool bReadOnly = false;
	/** 读到最后落在哪个对象上（部件、条目数据对象…）：`obj.set` 往它上面写。 */
	TWeakObjectPtr<UObject> Holder;
	/** 落在反射属性上时的属性名（`obj.set` 找 setter 用）。 */
	FString PropName;
	/**
	 * 落在反射属性上时的属性和值的地址（`obj.set` 原地写）。只在读的这一刻、同一帧里有效：
	 * 对象没了或数组改了大小就是野指针，不许存下来。
	 */
	const FProperty* RawProp = nullptr;
	void* RawValue = nullptr;

	bool IsResolved() const { return !Value.IsUnresolved(); }
};

/**
 * 对着活的对象读属性路径。
 *
 * `InItem` 是条目的数据对象（`in` 是 ListView 族容器时由引用解析给；为空时从条目控件上取 IUserObjectListEntry 的数据对象）。
 * 不调函数、不改游戏（回放时每帧读）。只在游戏线程上用。
 */
namespace GameTestPropRead
{
	GAMETEST_API FGameTestPropRead Read(UObject* InObject, const FGameTestPropPath& InPath, FGameTestFlowContext& InContext, UObject* InItem = nullptr);

	/** 文字写的路径（解析 + 读）。写法错时 Unresolved（code = prop_missing）。 */
	GAMETEST_API FGameTestPropRead ReadText(UObject* InObject, const FString& InPath, FGameTestFlowContext& InContext, UObject* InItem = nullptr);

	/**
	 * 只走部件：`Header.BtnClose` → 控件（UUserWidget 时按类自己的 WidgetTree 名字一层层进去；Actor 时第一段是组件名）。
	 * 找不到返回空，`OutError` 写哪一段没找到。空路径返回对象本身。
	 */
	GAMETEST_API UObject* FindPart(UObject* InObject, const FString& InPart, FString& OutError);

	/** 列表条目控件 → 它显示的数据对象（往外找第一个实现 IUserObjectListEntry 的 WBP）。 */
	GAMETEST_API UObject* ListItemOf(UObject* InObject);

	/** 部件在对象里的写法（`Header.BtnClose`）：从 InWidget 往外到 InOwner，每层用它在所在 WBP 树里的名字。写不出返回 false。 */
	GAMETEST_API bool PartPathOf(const UWidget* InWidget, const UObject* InOwner, FString& OutPart);
}
