// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestValue.h"
#include "Observe/Ref/GameTestPropKind.h"

class FGameTestFlowContext;
class UObject;
class UWidget;

/** 属性路径的一段（docs/game-test-ref.md 3.1）。 */
enum class EGameTestPathSeg : uint8
{
	/** `/名字`：子控件（WBP 设计器名）或 Actor 组件，只能写在属性前面。 */
	Child,
	/** `.名字`：反射属性（UPROPERTY、蓝图变量、结构体字段）；跟在 `#` 后面时是 Lua 表字段。 */
	Prop,
	/** `.名字()` / `.名字(1, "a")`：有返回值的取值函数（只许 BlueprintPure 或 const）；2.1.6 起可以带常量参数。 */
	Fn,
	/** `#名字`：绑的 UnLua 实例表字段。 */
	Lua,
	/** `[i]` / `["键"]`：数组下标 / Map 键（已去引号）。 */
	Index,
};

struct FGameTestPathSeg
{
	EGameTestPathSeg Kind = EGameTestPathSeg::Prop;
	FString Name;
	/** Fn 段括号里的原文（去掉首尾空白）：逗号分开的常量，数字、"文字"、true / false / null；没写参数为空。 */
	FString Args;
};

/**
 * 属性路径 v5（docs/game-test-ref.md 3.1，2.1.0 起）：从 `@`（对象自己）或 `@item`（列表条目的数据对象）出发，每段一个符号。
 *
 * | 写法 | 意思 |
 * | --- | --- |
 * | `@` / `@item` | 对象自己 / 列表条目的数据对象（开头） |
 * | `/名字` | 子控件或组件（只在属性前面，可以连续多段） |
 * | `.名字` | 反射属性 |
 * | `.名字()` / `.名字(1, "a")` | 取值函数（BlueprintPure 或 const），参数只能写常量（2.1.6） |
 * | `#名字` | Lua 实例表字段（之后的 `.b` `[i]` 仍是 Lua 表的键） |
 * | `:名字` | 运行状态（只在最后） |
 * | `[i]` / `["键"]` | 下标 / Map 键 |
 *
 * 规则和面板的 `ue/shared/gt-script/ref/path.ts` 一致（改一边要改另一边）。旧写法（`@self.`、`@lua.`、`@itemLua.`、
 * `@部件.`、开头的 `:状态`）报 path_old 并说新写法，写法不对报 path_syntax 并指出第几个字符。
 */
struct GAMETEST_API FGameTestPropPath
{
	/** 原文（去掉首尾空白）。 */
	FString Text;
	/** 从 `@item` 出发。 */
	bool bItem = false;
	/** 状态之前的各段。 */
	TArray<FGameTestPathSeg> Segs;
	/** 运行状态名（不带 `:`）；没写为空。 */
	FString State;
	/** Parse 失败时的错误码：path_syntax / path_old。 */
	FName FailCode;

	/** 拆开；写法不对返回 false，OutError 写原因（path_syntax 带第几个字符，path_old 带新写法），FailCode 写错误码。 */
	static bool Parse(const FString& InText, FGameTestPropPath& OutPath, FString& OutError);

	bool HasState() const { return !State.IsEmpty(); }

	/** 开头连续的子控件段数。 */
	int32 NumChildren() const;

	/** 开头的子控件段拼成部件写法（`Header.TxtTitle`，和 target 的 part 一样）；没有为空。 */
	FString ChildPart() const;

	/** 拼回文字（Parse 的反操作）。 */
	/** 函数段的参数原文 → 值（`1, "a", true`）。写法不对返回 false 并写原因（第几个参数、只能写什么）。 */
	static bool ParseArgs(const FString& InText, TArray<FGameTestValue>& OutArgs, FString& OutError);

	FString Format() const;

	/** 部件写法（`Header.TxtTitle`）→ 路径里的子控件段（`/Header/TxtTitle`）。空部件返回空串。 */
	static FString PartToPath(const FString& InPart);

	/** 对象上的属性名 → 路径（`ShopId` → `@.ShopId`）。 */
	static FString OfProp(const FString& InName);

	/** 运行状态名 → 路径（`text` → `@:text`）。 */
	static FString OfState(const FString& InState);

	/** 旧写法（v4）→ 建议的新写法（报 path_old 用）；猜不出时返回空。`@部件.X` 有两种可能，返回两种都写的说明。 */
	static FString SuggestNew(const FString& InOld);
};

/** 读到的一个属性值（判断、`gt.props`、`obj.set` 共用）。 */
struct GAMETEST_API FGameTestPropRead
{
	/** 值；读不到时是 Unresolved，原因写哪一段读不到。 */
	FGameTestValue Value;
	EGameTestKind Kind = EGameTestKind::None;
	/** 引擎类型名（`int32`、`FText`）；Lua、状态写 lua / state 的类型。 */
	FString Type;
	/** `cpp` / `bp` / `lua` / `state` / `fn`。 */
	FString Src;
	/** FText 的本地化键（`命名空间,键`）：比较 `key` 用；没有键为空。 */
	FString TextKey;
	/** 只读（委托、算出状态、函数返回值、BlueprintReadOnly）：`obj.set` 报 prop_readonly。 */
	bool bReadOnly = false;
	/** 读到最后落在哪个对象上（子控件、条目数据对象…）：`obj.set` 往它上面写。 */
	TWeakObjectPtr<UObject> Holder;
	/** 落在反射属性上时的属性名；Lua 字段是 `lua.` + 字段路径（`obj.set` 按它分开写）。 */
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
 * 不改游戏（回放时每帧读）；`.名字()` 只调只读函数。只在游戏线程上用。
 */
namespace GameTestPropRead
{
	GAMETEST_API FGameTestPropRead Read(UObject* InObject, const FGameTestPropPath& InPath, FGameTestFlowContext& InContext, UObject* InItem = nullptr);

	/** 文字写的路径（解析 + 读）。写法错时 Unresolved（code = path_syntax / path_old）。 */
	GAMETEST_API FGameTestPropRead ReadText(UObject* InObject, const FString& InPath, FGameTestFlowContext& InContext, UObject* InItem = nullptr);

	/**
	 * 只走部件（target 的 `part`）：`Header.BtnClose`（`/` 分开也认）→ 控件（UUserWidget 时按类自己的 WidgetTree 名字一层层进去；Actor 时是组件名）。
	 * 找不到返回空，`OutError` 写哪一段没找到。空路径返回对象本身。
	 */
	GAMETEST_API UObject* FindPart(UObject* InObject, const FString& InPart, FString& OutError);

	/** 列表条目控件 → 它显示的数据对象（往外找第一个实现 IUserObjectListEntry 的 WBP）。 */
	GAMETEST_API UObject* ListItemOf(UObject* InObject);

	/** 部件在对象里的写法（`Header.BtnClose`）：从 InWidget 往外到 InOwner，每层用它在所在 WBP 树里的名字。写不出返回 false。 */
	GAMETEST_API bool PartPathOf(const UWidget* InWidget, const UObject* InOwner, FString& OutPart);
}
