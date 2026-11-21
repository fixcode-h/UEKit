// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FGameTestDiagSink;
class FJsonObject;
class FJsonValue;
struct FGameTestExprScope;

/**
 * 判断项（docs/game-test-ref.md 第 3.3 节）：`{prop, op, value, tol?, ignoreCase?, cmp?}`。
 * 引用的 `ident`、条件里的对象判断、`obj.wait`、录制的断言都用它。
 */
struct GAMETEST_API FGameTestIdentItem
{
	/** 属性路径（`@.ShopId`、`@/TxtTitle:text`、`@:exists`）。 */
	FString Prop;
	/** 比较名（3.4 表）；空 = 按属性类型推（bool 真值、其余 eq）。 */
	FString Op;
	/** 期望值：常量 / `$var` / `"=表达式"`；`between` 是 `[a,b]`，`in` 是数组。 */
	TSharedPtr<FJsonValue> Value;
	/** `count` 的二级比较（`eq` / `gte` …），默认 `eq`。 */
	FString Cmp;
	TOptional<double> Tol;
	TOptional<bool> IgnoreCase;

	/** 认得的字段。 */
	static const TArray<FString>& Fields();

	/** 读一条（不校验，校验在 GameTestRefCheck）。`prop` 缺省时不报错（条件里省略 = `:exists`）。 */
	static FGameTestIdentItem FromJson(const FJsonObject& InJson);

	TSharedRef<FJsonObject> ToJson() const;

	/** 给人看的一句：`@.ShopId 等于 1001`。 */
	FString Describe() const;
};

/** `in` 的几种写法。 */
enum class EGameTestRefIn : uint8
{
	/** 省略：按类推（界面 → screen，Actor → world，子系统 → 单例，组件 → 要写 in）。 */
	Auto,
	Screen,
	World,
	/** 绑了 UnLua 的 UObject（UELink lua.objects）。 */
	Lua,
	All,
	/** 数组写法（`["screen", "lua"]`）：InList 里几处的并集。 */
	List,
	/** `@来源.路径`：容器属性里的对象。 */
	Source,
	/** `{obj, part}`：另一个对象里某个容器控件的条目。 */
	Container,
	/** `{obj}` 且 class 是组件类：那个 Actor 的组件。 */
	Owner,
};

/**
 * 对象引用（第 1 节）= 类 + 唯一标识组。脚本里只存它，不存 UObject、运行时名字、序号、选择器。
 */
struct GAMETEST_API FGameTestRef
{
	/** `objects` 里的名字（命名引用时），报告用。 */
	FString Name;
	FString Class;
	EGameTestRefIn In = EGameTestRefIn::Auto;
	/** `In == List` 时的几处（只会是 Screen / World / Lua）。 */
	TArray<EGameTestRefIn> InList;
	/** `In == Source` 时的 `@来源.路径`。 */
	FString InSource;
	/** `In == Container / Owner` 时外层对象的引用（三种写法之一）。 */
	TSharedPtr<FJsonValue> InObj;
	/** `In == Container` 时容器控件的部件写法。 */
	FString InPart;
	TArray<FGameTestIdentItem> Ident;
	/** `error` / `first` / `last` / `nearest`；空 = 运行配置 refDefaults.many。 */
	FString Many;
	TSharedPtr<FJsonValue> Near;
	/** 多客户端时在哪个世界找（`server` 或序号）；空 = 步骤的世界。 */
	FString Client;

	static const TArray<FString>& Fields();

	/** `in` 的一个词（screen / world / lua / all）→ 枚举；不是这几个返回 Auto。 */
	static EGameTestRefIn InWord(const FString& InText);

	/** 读内联引用对象（不校验字段，校验在 GameTestRefCheck）。`class` 缺失时返回 false。 */
	static bool FromJson(const FJsonObject& InJson, FGameTestRef& OutRef);

	TSharedRef<FJsonObject> ToJson() const;

	/** `WBP_Shop（@.ShopId=1001）`。 */
	FString Describe() const;
};

/** 写引用的地方（`obj`、`in.obj`、`target`）接受的三种写法（第 1 节）。 */
enum class EGameTestRefForm : uint8
{
	Invalid,
	/** `"shop"`：场景 `objects` 里的名字。 */
	Named,
	/** `"$var"`：变量里存的对象。 */
	Var,
	/** `{class, …}` */
	Inline,
	/** 旧的选择器字符串（`WBP_X:Root/Btn`、`@句柄`）：v4 不认（selector_removed）。 */
	Selector,
};

namespace GameTestRefForm
{
	/** 认写法；`OutText` 是名字或变量名（不带 `$`），`OutInline` 是内联对象。 */
	GAMETEST_API EGameTestRefForm Classify(const TSharedPtr<FJsonValue>& InValue, FString& OutText, TSharedPtr<FJsonObject>& OutInline);

	/** 名字只能是字母数字下划线（不以数字开头）：别的字符串多半是旧选择器。 */
	GAMETEST_API bool IsName(const FString& InText);
}

/**
 * 部件写法（第 2 节）：`{obj, part?, slate?}`；也接受直接写一个引用（= `{obj: 它}`）。
 */
struct GAMETEST_API FGameTestTarget
{
	TSharedPtr<FJsonValue> Obj;
	/** 类自己的 WidgetTree 里的名字，嵌套 WBP 用 `.` 进去；Actor 时是组件名。空 = 对象本身。 */
	FString Part;
	/** 纯 Slate 段：`类型[text=子串][序号]`，多段 `/`。 */
	FString Slate;

	static const TArray<FString>& Fields();

	/** 读（不校验）。不是部件写法也不是引用时返回 false。 */
	static bool FromJson(const TSharedPtr<FJsonValue>& InValue, FGameTestTarget& OutTarget);

	TSharedRef<FJsonObject> ToJson() const;

	FString Describe() const;
};

/**
 * 写法检查（加载、`gt.validate`）：不开游戏，只看字段、类型、名字。类在不在、部件在不在类树里、
 * 属性类型和 op 对不对得上由深度检查做（GameTestDeepRefCheck）。
 */
namespace GameTestRefCheck
{
	/** 一个写引用的值（三种写法之一）。`InScope.KnownObjects` 给了时查名字在不在 `objects` 里。 */
	GAMETEST_API void Ref(const TSharedPtr<FJsonValue>& InValue, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink);

	/** 内联引用对象（`objects` 的值也是它；`bInObjects` 时不许再引用别的名字）。 */
	GAMETEST_API void RefObject(const FJsonObject& InJson, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink);

	/** 一条判断项。 */
	GAMETEST_API void Ident(const FJsonObject& InJson, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink, bool bInPropRequired = true);

	/** 部件写法（target、toTarget、each、widget）。 */
	GAMETEST_API void Target(const TSharedPtr<FJsonValue>& InValue, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink);

	/** 属性路径写法。 */
	GAMETEST_API void Prop(const FString& InProp, FGameTestDiagSink& InSink);

	/** `objects` 段：每个值是内联引用、`in.obj` 引用的名字不成环（ref_cycle）。 */
	GAMETEST_API void Objects(const FJsonObject& InObjects, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink);

	/** 旧选择器字符串的统一报错（selector_removed）。 */
	GAMETEST_API void SelectorRemoved(const FString& InSelector, FGameTestDiagSink& InSink);
}
