// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Expr/GameTestExpr.h"
#include "Core/Flow/GameTestNode.h"
#include "Core/Script/GameTestFlowContext.h"

class FGameTestDiagSink;
class FJsonObject;
class FJsonValue;

/**
 * 场景 v3 的读取和校验（协议第 1、2 节）：一次把整棵树解析好，执行时不再看 JSON 写法。
 *
 * 加载场景（gt.run）、`gt.validate`、`gt.step` 的块都走这里，错误统一进 FGameTestDiagSink（带路径、id、错误码）。
 * 1 / 2 版的平铺步骤就是只有主流程、没有块的 v3，照读，不补 id、不改文件。
 *
 * 块的解析由块自己做（IGameTestBlock::Parse），它们通过这里提供的方法解析子节点、检查条件和表达式、
 * 登记变量、加载 call 的子脚本——解析器不知道有哪些块。
 */
class GAMETEST_API FGameTestScriptParser
{
public:
	/** `InScenarioRoot` 为空时 call 不能用（临时步骤没有场景根就报 call_missing）。 */
	FGameTestScriptParser(const FString& InScenarioRoot, FGameTestDiagSink& InSink, const FGameTestScriptOptions& InOptions);

	/** 解析一份场景。出错也返回能解析的部分；有没有错看 Sink。 */
	TSharedRef<FGameTestScript> ParseScript(const TSharedPtr<FJsonObject>& InJson, const FString& InPath);

	/** 临时步骤（gt.run steps、gt.step、运行配置的 setup）：没有场景头，只有步骤。 */
	TSharedRef<FGameTestScript> ParseSteps(const TArray<TSharedPtr<FJsonObject>>& InSteps, const FString& InName, const FString& InListKey = TEXT("steps"));

	//~ 给块用 -----------------------------------------------------------------
	FGameTestDiagSink& Sink() { return DiagSink; }

	/** 把 `InValue`（步骤数组）解析成节点的第 N 串子节点，返回下标。`bRequired` 时缺了报错。 */
	int32 ParseList(FGameTestNode& InOutOwner, const FString& InKey, const TSharedPtr<FJsonValue>& InValue, bool bRequired);

	/** 检查统一条件（第 4 节）。`InField` 是字段名（拼路径用）。 */
	void CheckCond(const TSharedPtr<FJsonValue>& InCond, const FString& InField);

	/** 检查填值（`$var`、`=表达式`）。`bReadOnly` = false 时表达式里也只许只读（表达式永远只读），这里只是让 Lua 调用按步骤规则放行。 */
	void CheckValue(const TSharedPtr<FJsonValue>& InValue, const FString& InField);

	/** 登记变量（set、saveAs、循环的 as / index），之后的 `$名字` 不再报 var_undefined。 */
	void DeclareVar(const FString& InName);
	/** 循环的局部名字只在循环里看得到。 */
	void PushVarScope();
	void PopVarScope();

	/**
	 * 加载 call 的子脚本（相对场景根，`.json` 可省）：同一个路径只解析一次；成环报 call_cycle，超过 maxCallDepth 报 call_depth。
	 * 找不到报 call_missing 并返回空。
	 */
	TSharedPtr<FGameTestScript> LoadCall(const FString& InScript);

	const FGameTestScriptOptions& GetOptions() const { return Options; }

	/** 当前脚本里已知的变量（表达式检查用）。 */
	FGameTestExprScope MakeExprScope() const;

	/** 解析一个步骤（块或叶子）。路径 = InPath。守护的 `do` 也用它。 */
	TSharedRef<FGameTestNode> ParseNode(const TSharedPtr<FJsonValue>& InJson, const FString& InPath);

	/** 解析一串步骤到 OutList（路径前缀 InPath，如 `steps`、`steps.3.then`）。 */
	void ParseNodes(const TSharedPtr<FJsonValue>& InValue, const FString& InPath, FGameTestNodeList& OutList);

private:
	void ParseHeader(const TSharedPtr<FJsonObject>& InJson, FGameTestScript& OutScript);
	void ParseCommon(const FJsonObject& InJson, FGameTestNode& OutNode);
	void ParseBlock(const FJsonObject& InJson, FGameTestNode& OutNode);
	void ParseLeaf(const TSharedPtr<FJsonObject>& InJson, FGameTestNode& OutNode);
	void CheckLeafRefs(const FJsonObject& InJson);

	/** 一份脚本的解析状态（解析 call 的子脚本时压栈）。 */
	struct FScriptState
	{
		FString Path;
		TSet<FString> Ids;
		TArray<TSet<FString>> Vars;
		int32 TopLevelLeaf = 0;
	};

	FScriptState& State() { return States.Last(); }

	FString ScenarioRoot;
	FGameTestDiagSink& DiagSink;
	FGameTestScriptOptions Options;
	TArray<FScriptState> States;
	TMap<FString, TSharedPtr<FGameTestScript>> CallCache;
	TArray<FString> Loading;
};

/** 场景文件相关的小工具。 */
namespace GameTestScriptFiles
{
	/** call 的 script 字段 → 相对场景根的规范路径（`common/翻完引导` → `common/翻完引导.json`，`/` 分隔）。 */
	GAMETEST_API FString NormalizeCallPath(const FString& InScript);

	/** 读场景文件（UTF-8 JSON）。 */
	GAMETEST_API TSharedPtr<FJsonObject> LoadJson(const FString& InAbsolutePath, FString& OutError);
}
