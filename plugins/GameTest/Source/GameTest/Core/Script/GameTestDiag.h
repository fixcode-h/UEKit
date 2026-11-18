// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * 一条诊断（`gt.validate` 的 errors / warnings，加载场景时的拒绝原因）：协议第 10、12 节的形状。
 */
struct GAMETEST_API FGameTestDiag
{
	/** 出问题的步骤 id（有的话）。 */
	FString Id;
	/** 路径：`steps.3.then.0.cond.all.1`。 */
	FString Path;
	/** 错误码（协议第 12 节）。 */
	FName Code;
	/** 中文，写清楚怎么改（原样给用户和 Agent 看）。 */
	FString Message;
	bool bWarning = false;

	TSharedPtr<FJsonObject> ToJson() const;
	/** 「steps.3.then.0：xxx」——拼进旧接口的错误字符串数组。 */
	FString ToLine() const;
};

/**
 * 诊断收集器：校验代码只管「在当前路径下报一条」，路径由调用方压栈，不用每层自己拼字符串。
 *
 * ```cpp
 * FGameTestDiagSink::FScope Scope(Sink, TEXT("cond"));
 * Sink.Error(TEXT("expr_syntax"), TEXT("第 5 个字附近缺右括号"));
 * ```
 */
class GAMETEST_API FGameTestDiagSink
{
public:
	enum class EScopeMode : uint8
	{
		/** 接在当前路径后面（`cond` → `steps.3.cond`）。 */
		Append,
		/** 段是从脚本根算的完整路径（节点路径 `steps.3.then.0`）：换掉基准之后的部分。 */
		FromRoot,
		/** 接在后面并把结果当新的基准（进 call 的子脚本：`<common/x.json>`，里面的节点路径从它算）。 */
		NewRoot,
	};

	/** 压一段路径（离开作用域自动弹出）。 */
	struct GAMETEST_API FScope
	{
		FScope(FGameTestDiagSink& InSink, const FString& InSegment, const FString& InId = FString(), EScopeMode InMode = EScopeMode::Append);
		~FScope();

	private:
		FGameTestDiagSink& Sink;
		FString PrevPath;
		int32 PrevRootLen;
		FString PrevId;
	};

	void Error(FName InCode, const FString& InMessage);
	void Warning(FName InCode, const FString& InMessage);

	/** 当前路径（`steps.3.then.0`）。 */
	const FString& GetPath() const { return Path; }
	const FString& GetId() const { return CurrentId; }

	bool HasErrors() const { return ErrorCount > 0; }
	int32 NumErrors() const { return ErrorCount; }
	const TArray<FGameTestDiag>& GetAll() const { return Items; }

	/** 只要错误，拼成「路径：说明」的行（喂给旧接口的 TArray<FString> OutErrors）。 */
	void AppendErrorLines(TArray<FString>& OutLines) const;

private:
	void Add(FName InCode, const FString& InMessage, bool bWarning);

	TArray<FGameTestDiag> Items;
	FString Path;
	/** 当前脚本根在 Path 里的长度（FromRoot 从这里接）。 */
	int32 RootLen = 0;
	FString CurrentId;
	int32 ErrorCount = 0;
};
