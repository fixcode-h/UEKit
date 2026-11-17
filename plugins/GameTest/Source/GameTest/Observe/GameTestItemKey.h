// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UObject;
class UWidget;

/**
 * 容器条目按「标识」认（1.4.0，用户 2026-11-17 定稿）：`容器[条件,条件]`。
 *
 * **为什么需要它**：列表、格子里的条目没有名字，只能按序号或文字认，背包整理一下、商店刷新一下就点错。
 * 能认出「这一格是哪件道具」的是它显示的数据（`ItemId`、Lua 表上的 `cfgId`），这个文件负责：
 * - 条件的写法和解析：`Data.ItemId=1001`（ListView 的数据对象，可嵌套）、`@self.SkillId=3`（条目控件）、
 *   `@lua.Data.cfgId=1001`（条目控件的 Lua 表）、`@itemLua.cfgId=1001`（数据对象的 Lua 表）、`@text=魔法药水`、
 *   `@row=1,@col=2`（UniformGrid / Grid 的行列）；多个条件用 `,` 连，都满足才算。
 * - 收容器的条目：ListView 系是**全部**数据项（不只是显示出来的），其他容器是直接子控件加 Slate 上挂的 WBP。
 * - 读值、找命中、给录制挑候选并打分。
 *
 * 不调 Lua 函数、不调蓝图函数取值（有副作用，回放时每帧读）；Lua 只读实例表自己的字段（UELink 2.8 的 ReadFields）。
 * 只在游戏线程上用。
 */
struct GAMETEST_API FGameTestKeyCond
{
	/** 来源：空 = 默认（ListView 是数据对象，其他容器是条目控件），`self`、`lua`、`itemLua`、`text`、`row`、`col`。 */
	FString Source;
	/** 字段路径（`Data.ItemId`，大小写不敏感）；text / row / col 没有。 */
	FString Path;
	FString Value;
};

/** 容器里的一个条目。 */
struct GAMETEST_API FGameTestKeyEntry
{
	/** ListView 系的数据项；其他容器为空。 */
	TWeakObjectPtr<UObject> Item;
	/** 条目控件：ListView 系只有显示出来的才有（滚动时复用）；其他容器就是子控件。 */
	TWeakObjectPtr<UWidget> Widget;
	int32 Index = INDEX_NONE;
};

/** 录制时给一个条目挑的候选标识。 */
struct GAMETEST_API FGameTestKeyCandidate
{
	/** 条件左边（`Data.ItemId`、`@lua.cfgId`、`@row`）。 */
	FString By;
	/** 来源（同 FGameTestKeyCond::Source，默认来源写成 `item` 或 `self`）。 */
	FString Source;
	TSharedPtr<FJsonValue> Value;
	double Score = 0.0;
	/** 容器里有几个条目的这个字段等于这个值（包括它自己）。 */
	int32 Hits = 0;
	/** 按下和松开之间值变了（数量、倒计时）。 */
	bool bChanged = false;
	/** 给人看的一句：像位置、本地化文字、会变…… */
	FString Why;
	/** 和其他候选组合成的（`Data.Name=…,Data.Quality=…`）。 */
	bool bCombo = false;
	/** 写进方括号的整段条件（`Data.ItemId=1001`，组合时 `A=1,B=2`）。 */
	FString Cond;
	/** 打分用：值正好等于条目序号。 */
	bool bEqualsIndex = false;
};

/**
 * 录制时被点控件所在的「条目」：写选择器时记下来，录制器拿它挑标识、问人、改写 target。
 * From / Base / Positional 都是 target 的前缀（写选择器是从外往里拼的），改写时只换前缀。
 */
struct GAMETEST_API FGameTestKeySite
{
	TWeakObjectPtr<UWidget> Container;
	FGameTestKeyEntry Entry;
	bool bList = false;
	/** 容器里一共几个条目（收到的）。 */
	int32 Count = 0;
	/** 容器的选择器（写条件时 Base + "[" + 条件 + "]"）。 */
	FString Base;
	/** 按位置的写法（`列表[3]`、`容器/#类名#2`）。 */
	FString Positional;
	/** target 里现在用的写法（Base[条件] 或 Positional）。 */
	FString From;
	/** 条目的类（规则按它认）。 */
	FString EntryClass;
	TArray<FGameTestKeyCandidate> Candidates;
	/** 现在用的条件；空表示按位置。 */
	FString Cond;
	/** 怎么定的：auto（够准自动挑）/ rule（规则）/ pending（拿不准，待定）/ user（人选的）/ index（人选了按位置）。 */
	FString By;
	/** 写选择器时第几层发现的（越小越靠近被点的控件）。 */
	int32 Depth = MAX_int32;
	int32 SegmentOrder = 0;
};

/** 读值的缓存：一次解析里同一个对象只 dump 一次。 */
struct GAMETEST_API FGameTestKeyCache
{
	TMap<TWeakObjectPtr<UObject>, TSharedPtr<FJsonObject>> Fields;
	TMap<TWeakObjectPtr<UObject>, TSharedPtr<FJsonObject>> LuaFields;
};

namespace GameTestItemKey
{
	/** 一个条目最多看多少个字段（每个来源）、容器最多看多少个条目：录制时一次点击不为几千条数据卡住游戏。 */
	constexpr int32 MaxFieldsPerSource = 300;
	constexpr int32 MaxEntries = 500;
	/** 推荐线：最高分的候选唯一、不会变、分数到这个分，就不问人。 */
	constexpr double ConfidentScore = 5.0;

	//~ 纯逻辑（单测 GameTest.ItemKey）

	/**
	 * `[…]` 里的内容 → 条件。纯数字（`[3]`）不是条件，返回 false 且不写 OutError，调用方按序号处理。
	 * 写法不对返回 false 并写清楚。
	 */
	GAMETEST_API bool ParseConds(const FString& InInner, TArray<FGameTestKeyCond>& OutConds, FString& OutError);

	/** 条件左边的写法：默认来源只写路径，其他写 `@来源.路径`（text / row / col 写 `@来源`）。 */
	GAMETEST_API FString FormatBy(const FString& InSource, const FString& InPath, bool bInDefaultSource);

	/** 值写进选择器的样子：字符串原样、整数不带小数点、布尔 true / false；写不进（带 `[` `]` `,` `>` `/`）返回空。 */
	GAMETEST_API FString FormatValue(const FJsonValue& InValue);

	/**
	 * 给一个候选打分（设计稿第 3 节 A）。`InEqualsIndex`：值正好等于条目序号（多半是位置）。
	 * 不唯一、会变、浮点、布尔不进推荐（分数压到很低）；`OutWhy` 写主要的扣分原因。
	 */
	GAMETEST_API double Score(const FString& InSource, const FString& InPath, const FJsonValue& InValue,
		bool bInUnique, bool bInEqualsIndex, bool bInChanged, bool bInLocalizedText, FString& OutWhy);

	/** 候选按分数排好后，最高的够不够准（不用问人）。 */
	GAMETEST_API bool IsConfident(const TArray<FGameTestKeyCandidate>& InSorted);

	/** JSON 对象里按点分路径取值（每段大小写不敏感）。 */
	GAMETEST_API TSharedPtr<FJsonValue> FindPath(const FJsonObject& InObject, const FString& InPath);

	/** 把对象摊平成「路径 → 标量」（最多 InDepth 层、InMax 个），数组和对象引用不要。 */
	GAMETEST_API void Flatten(const FJsonObject& InObject, int32 InDepth, int32 InMax, TArray<TPair<FString, TSharedPtr<FJsonValue>>>& OutLeaves);

	/** 候选 → JSON（hint.key.cands、gt.keys、推送事件共用一种形状）。 */
	GAMETEST_API TSharedPtr<FJsonObject> CandidateToJson(const FGameTestKeyCandidate& InCandidate);

	//~ 引擎侧

	/** 是不是 ListView 系（ListView / TileView / TreeView）。 */
	GAMETEST_API bool IsListContainer(const UWidget* InContainer);

	/** 收容器的条目（最多 InMax 个）；不是容器返回 false。 */
	GAMETEST_API bool CollectEntries(UWidget* InContainer, TArray<FGameTestKeyEntry>& OutEntries, int32 InMax = MaxEntries);

	/** 条目在容器条目里的下标（按控件或数据项找）；找不到 INDEX_NONE。 */
	GAMETEST_API int32 FindEntry(const TArray<FGameTestKeyEntry>& InEntries, const UWidget* InWidget, const UObject* InItem);

	/** 站点 → hint.key（`from`、`base`、`pos`、`entry`、`count`、`by`、`cond`、`list`、`cands`）。 */
	GAMETEST_API TSharedPtr<FJsonObject> SiteToJson(const FGameTestKeySite& InSite, int32 InMaxCandidates = 8);

	/**
	 * 对象**自己**的字段（项目和蓝图里声明的；UMG、引擎基类的不要），结构体展开成对象（最多 3 层），
	 * 数字、字符串、Name、Text、枚举、布尔是值，对象引用、数组、Map 不要。键用声明时的名字。
	 */
	GAMETEST_API TSharedPtr<FJsonObject> DumpFields(const UObject* InObject, FGameTestKeyCache& InOutCache);

	/** 对象 Lua 实例表上的字段（没绑 Lua、没有 UnLua 时返回空）。 */
	GAMETEST_API TSharedPtr<FJsonObject> DumpLuaFields(UObject* InObject, FGameTestKeyCache& InOutCache);

	/** 读一个条目在某个来源、路径上的值；读不到返回空。 */
	GAMETEST_API TSharedPtr<FJsonValue> ReadValue(const FGameTestKeyEntry& InEntry, bool bInList,
		const FString& InSource, const FString& InPath, FGameTestKeyCache& InOutCache);

	/** 按条件找命中的条目下标（最多找 InMaxHits 个就停）。 */
	GAMETEST_API void FindMatches(const TArray<FGameTestKeyEntry>& InEntries, bool bInList, const TArray<FGameTestKeyCond>& InConds,
		FGameTestKeyCache& InOutCache, int32 InMaxHits, TArray<int32>& OutHits);

	/** 失败消息里的「现有的值」：第一个条件的字段在各条目上是什么（最多 InLimit 个）。 */
	GAMETEST_API FString DescribeValues(const TArray<FGameTestKeyEntry>& InEntries, bool bInList, const FGameTestKeyCond& InCond,
		FGameTestKeyCache& InOutCache, int32 InLimit);

	/**
	 * 给容器里第 InMine 个条目挑候选标识，按分数从高到低排好（最多 InMax 个）。
	 * 单个字段都不唯一时，试一下前几个字段两两组合，组合唯一的也放进来（bCombo）。
	 */
	GAMETEST_API void BuildCandidates(UWidget* InContainer, const TArray<FGameTestKeyEntry>& InEntries, int32 InMine,
		int32 InMax, TArray<FGameTestKeyCandidate>& OutCandidates);

	/**
	 * gt.keys 的「全部字段」：按来源分组列出条目的每个字段和它在容器里命中几个。
	 * 返回 `{groups: [{source, title, class?, note?, fields: [{by, path, v, hits, unique}], more?}], total, count}`。
	 */
	GAMETEST_API TSharedPtr<FJsonObject> ListFields(UWidget* InContainer, const TArray<FGameTestKeyEntry>& InEntries, int32 InMine,
		const FString& InFilter, int32 InOffset, int32 InLimit);

	/**
	 * 录制写选择器时给站点挑标识：收条目、挑候选（最多 12 个），有规则按规则（`by = rule`），
	 * 最高分够准就用它（`auto`），否则 `pending`（分数还行的唯一字段先用上，都不行就按位置）。
	 * 容器里找不到这个条目返回 false。
	 */
	GAMETEST_API bool Decide(FGameTestKeySite& InOutSite);

	/** 只按候选挑（不看规则）：够准 → auto，否则 pending（分数还行的唯一字段先用上）。松开时复查后也用它重挑。 */
	GAMETEST_API void Choose(FGameTestKeySite& InOutSite);

	/** 用某个条件（空 = 按位置）改写 target：把前缀 From 换成 `Base[条件]` 或 Positional，更新 From。前缀对不上返回 false。 */
	GAMETEST_API bool Apply(FGameTestKeySite& InOutSite, const FString& InCond, FString& InOutTarget);

	/** 规则的字段 → 这个条目上的条件（`Data.ItemId=1001`）；有字段读不到或写不进返回空。 */
	GAMETEST_API FString CondFromFields(const FGameTestKeySite& InSite, const TArray<FString>& InBy);

	/** 同一个条目松开时再读一次：值变了的候选标 bChanged 并重新打分、排序。 */
	GAMETEST_API void Recheck(UWidget* InContainer, const FGameTestKeyEntry& InEntry, TArray<FGameTestKeyCandidate>& InOutCandidates);
}
