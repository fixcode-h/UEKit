// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestRegistry.h"

class FGameTestFlowContext;
class FGameTestScriptParser;
class FJsonObject;
struct FGameTestGuardDef;
struct FGameTestNode;
struct FGameTestNodeList;

/** 块走一步的结果。 */
enum class EGameTestBlockStep : uint8
{
	/** 还在等（if 在稳住条件、loop 在等 gapMs）：执行器下一帧再问，期间照常查守护。 */
	Pending,
	/** 跑一串子节点（`Out.List` 或 `Out.ExternalList`），跑完再调 Tick。 */
	RunList,
	/** 这一块做完了。 */
	Done,
	/** 这一块失败（`Out.Message` 是原因），按块的 onFail 处理。 */
	Failed,
	/** 发控制信号（break / continue / return），执行器往外找接住它的块。 */
	Signal,
};

enum class EGameTestFlowSignal : uint8
{
	None,
	Break,
	Continue,
	Return,
};

/** 每个块执行时自己的状态（第几轮、选了哪个分支、稳住到哪了）：每种块一个子类。 */
struct GAMETEST_API FGameTestBlockState
{
	virtual ~FGameTestBlockState() = default;
};

/** Tick 的输出。 */
struct GAMETEST_API FGameTestBlockTick
{
	/** RunList：跑节点自己的第几串（FGameTestNode::Lists 的下标）。 */
	int32 List = INDEX_NONE;
	/** RunList：跑别处的一串（call 的子脚本）；给了它就不看 List。 */
	const FGameTestNodeList* ExternalList = nullptr;
	/** 结果说明（Done / Failed 进报告；Pending 时是此刻在等什么，调试面板和超时消息用）。 */
	FString Message;
	EGameTestFlowSignal Signal = EGameTestFlowSignal::None;
	/** 循环换轮时填轮次（从 0），执行器推 `gt.flow phase=iter`。 */
	int32 Iter = INDEX_NONE;
	/** 块的结果附加信息（if 走了哪个分支、循环跑了几轮），进这个块的报告记录。 */
	TSharedPtr<FJsonObject> Payload;
};

/**
 * 块（协议第 2 节、第 14 节）：`group`、`if`、`loop`、`call`、`set`、`break`、`continue`、`return`、`fail`……
 * 一种一个实现、一个文件（`Core/Flow/Blocks/GameTestBlock<名字>.cpp`），登记进 `GameTestBlocks`。
 *
 * 块只管「下一步跑哪串子节点 / 什么时候结束」。等待、超时、守护、onFail / retry、报告、推送、断点
 * 都是执行器（FGameTestFlow）的公共逻辑，块不自己处理——加一种块不用碰执行器。
 */
class GAMETEST_API IGameTestBlock
{
public:
	virtual ~IGameTestBlock() = default;

	/** 动作名（`if`、`loop`）；`set` 变量块的键是 `set.var`，靠 Claims 认。 */
	virtual FName GetKey() const = 0;

	virtual FString GetSummary() const = 0;

	/** 这个步骤归不归它。默认看 action 等于键；`set` 有 `var` 才是变量块（没有时是 1.x 的控件赋值动作）。 */
	virtual bool Claims(const FJsonObject& InJson, FName InAction) const { return InAction == GetKey(); }

	/** 块自己的字段（公共字段 id、note、onFail… 由解析器加）。不在里面的报 unknown_field。 */
	virtual const TArray<FString>& GetFields() const = 0;

	/** 解析：读字段、校验、用 `InParser.ParseList` 解析子节点、把解析结果放进 `InOutNode.Data`。错误报给 `InParser.Sink()`。 */
	virtual void Parse(FGameTestNode& InOutNode, FGameTestScriptParser& InParser) const = 0;

	/** 进块：建状态。 */
	virtual TSharedRef<FGameTestBlockState> Enter(const FGameTestNode& InNode, FGameTestFlowContext& InContext) const;

	/** 走一步：刚进块时调一次，之后每跑完一串子节点、或上次返回 Pending 的下一帧再调。 */
	virtual EGameTestBlockStep Tick(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext,
		double InNowSeconds, FGameTestBlockTick& Out) const = 0;

	/** 子节点里发出的信号：接住返回 true（loop 接 break / continue，call 接 return），之后照常调 Tick。 */
	virtual bool CatchSignal(EGameTestFlowSignal InSignal, const FGameTestNode& InNode, FGameTestBlockState& InState,
		FGameTestFlowContext& InContext) const
	{
		return false;
	}

	/** 出块（做完、失败、被信号穿过、取消都调）：收拾作用域（call 弹出子脚本作用域、loop 弹出局部名字）。 */
	virtual void Leave(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext) const
	{
	}

	/** 块里额外生效的守护（call 进去的子脚本自己的 guards）。节点自己的 `guards` 执行器已经加了。 */
	virtual void CollectGuards(const FGameTestNode& InNode, const FGameTestBlockState& InState, TArray<TSharedRef<FGameTestGuardDef>>& OutGuards) const
	{
	}

	/** 单步调试时「块算一个节点」：false 的块（set、break…）单步时和叶子一样一下走完。 */
	virtual bool IsContainer() const { return true; }
};

/** 块注册表。 */
class GAMETEST_API GameTestBlocks
{
public:
	static TGameTestRegistry<IGameTestBlock>& Get();

	/** 认领这个步骤的块；不是块（是叶子动作）返回 nullptr。 */
	static const IGameTestBlock* FindFor(const FJsonObject& InJson, FName InAction);
};
