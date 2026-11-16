// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FGameTestReport;

/**
 * 按场景记的运行历史：每个场景文件最近 10 次的通过 / 失败，存在 `<产物根>/history.json`，跨编辑器会话保留。
 *
 * 和 `GameTestRunStore`（本次会话最近若干次的完整报告，只在内存）是两件事：这里只记摘要，按场景文件分组，
 * 给 GameTest 页列表上的历史条和「这个场景最近是不是一直红」用。临时步骤（没有场景文件）不记。
 * 文件格式：`{ "scenarios": { "<相对场景根的路径>": [最新在前的摘要…] } }`。
 */
class GAMETEST_API GameTestHistory
{
public:
	/** 每个场景留几次。 */
	static constexpr int32 Keep = 10;

	/** 纯逻辑：把一条摘要放到某个场景历史的最前面，超出 Keep 的丢掉（可单测）。 */
	static void Append(FJsonObject& InOutAll, const FString& InScenario, const TSharedPtr<FJsonObject>& InEntry);

	/** 报告 → 一条历史（runId、成败、时间、耗时、步数、第一个失败的步骤和原因、产物目录）。 */
	static TSharedPtr<FJsonObject> MakeEntry(const FGameTestReport& InReport, const FString& InOutputDirRelative);

	/** 一次 run 结束：报告有场景文件时追加进 history.json。写不了只记警告。 */
	static void Record(const FGameTestReport& InReport, const FString& InOutputDirRelative);

	/** 读整份历史；文件不存在或坏了返回空的 `{ scenarios: {} }`。 */
	static TSharedPtr<FJsonObject> Load();

	/** 只要每个场景的通过 / 失败串（最新在前，p 通过、f 失败）：列表上画历史条用，几百个场景也很短。 */
	static TSharedPtr<FJsonObject> Compact(const FJsonObject& InAll);

	static FString FilePath();
};
