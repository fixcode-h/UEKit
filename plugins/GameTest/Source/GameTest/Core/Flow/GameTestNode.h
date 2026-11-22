// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/GameTestTypes.h"

class FJsonObject;
class FJsonValue;
class IGameTestBlock;
struct FGameTestGuardDef;
struct FGameTestNode;
struct FGameTestScript;

/** 块里的一串子节点（`then`、`elif.0.then`、`else`、`do`、子脚本的 `steps`）。 */
struct GAMETEST_API FGameTestNodeList
{
	/** 在块里的字段路径（`then`、`elif.1.then`），拼节点路径用。 */
	FString Key;
	TArray<TSharedRef<FGameTestNode>> Nodes;
};

/** 块自己解析出来的东西（条件、循环参数…）：每种块一个子类，放在块的实现文件里。 */
struct GAMETEST_API FGameTestNodeData
{
	virtual ~FGameTestNodeData() = default;
};

/**
 * 脚本里的一个节点（协议第 2 节）：叶子是动作（click、wait、assert、api…），块是容器和控制（if、loop、call…）。
 *
 * 加载时整棵树解析一次（字段白名单、表达式、条件、call 的子脚本），执行时只读。
 * 叶子的 `Step` 就是 1.x 的 FGameTestStep，执行器原样跑；块的 `Step` 只用公共字段（note、timeoutMs、onFail）。
 */
struct GAMETEST_API FGameTestNode
{
	/** 步骤 id（场景里写的；没写为空）。 */
	FString Id;
	/** 路径（`steps.3.then.0`），没 id 时报告、断点按它认。 */
	FString Path;
	FName Action;
	TSharedPtr<FJsonObject> Json;

	/** 块的实现；叶子为空。 */
	const IGameTestBlock* Block = nullptr;
	/** 块解析出来的东西。 */
	TSharedPtr<FGameTestNodeData> Data;
	TArray<FGameTestNodeList> Lists;

	/** 叶子：解析好的步骤；块：公共字段。 */
	FGameTestStep Step;

	/** 这一块 / 这一步里生效的守护。 */
	TArray<TSharedRef<FGameTestGuardDef>> Guards;
	bool bDisabled = false;
	/** 执行期间日志里出现 Error / Ensure / Lua 报错就算失败。 */
	bool bExpectNoErrors = false;
	/** `onFail: retry` 时最多重做几次。 */
	int32 Retries = 0;
	/** 附带 GM（`hooks`）解析时包在步骤外面的那一层：断点、单步不在它上面停，单步的深度也不算它那一层。 */
	bool bHookWrap = false;
	/** 附带 GM 里的步骤（含它们的子步骤）：断点、单步跳过，调试时只停在主步骤上。 */
	bool bInHook = false;
	/** 附带 GM 里 `onError: note`（默认）的项里的节点：失败只记一笔（记录标 warn），不算整次 run 失败。 */
	bool bHookNote = false;

	bool IsBlock() const { return Block != nullptr; }

	/** 报告、断点、推送里认这个节点：有 id 用 id，没有用路径。 */
	const FString& Key() const { return Id.IsEmpty() ? Path : Id; }

	const FGameTestNodeList* FindList(const FString& InKey) const;

	/** 子树里（不含自己）有没有这个 id / 路径的节点。 */
	bool Contains(const FString& InKey) const;

	/** 子树里叶子的个数（进度条估算用，循环只按一轮算）。 */
	int32 CountLeaves() const;

	template <typename T>
	const T* GetData() const
	{
		return static_cast<const T*>(Data.Get());
	}
};

/** 参数声明（场景的 `params`）。 */
struct GAMETEST_API FGameTestParamDef
{
	FString Name;
	TSharedPtr<FJsonValue> Default;
	/** `string` / `int` / `float` / `bool` / `vector` / `object` / `any`。 */
	FString Type = TEXT("any");
	FString Note;
};

/**
 * 一份解析好的脚本（主场景、call 的子脚本、运行配置的 setup 都是它）。
 * 同一个子脚本被 call 多次时共享一份（按路径缓存），节点是只读的。
 */
struct GAMETEST_API FGameTestScript
{
	/** 相对场景根的路径；临时步骤为空。 */
	FString Path;
	FString Name;
	int32 SchemaVersion = 3;
	TArray<FGameTestParamDef> Params;
	TSharedPtr<FJsonObject> Vars;
	TSharedPtr<FJsonObject> Objects;
	TArray<TSharedRef<FGameTestGuardDef>> Guards;
	FGameTestNodeList Setup;
	FGameTestNodeList Steps;

	/** 按 id 或路径找节点（断点、from / to、gt.debug）。 */
	const FGameTestNode* FindNode(const FString& InKey) const;

	/** 主流程叶子数（进度估算）。 */
	int32 CountLeaves() const;

	/**
	 * 1.x 的平铺步骤包成脚本（gt.step、临时步骤、没走解析器的调用方）：每步一个叶子，路径 `steps.N` / `setup.N`。
	 * 已经过 GameTestActions::ParseStep 的步骤，不再校验。
	 */
	static TSharedRef<FGameTestScript> FromSteps(const TArray<FGameTestStep>& InSteps);
};
