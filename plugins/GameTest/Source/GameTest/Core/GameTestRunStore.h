// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * 本次会话的 run 历史（定长环形）。
 *
 * 存在的理由：`gt.job` 只有"当前这一次"的一份快照，下一次 run 一来就把它覆盖掉——
 * 于是"上次为什么红"在轮询超时、换 run 之后全部丢失。这里把每次收尾的报告留若干份，
 * 让摘要清单与单份回查都成立。
 *
 * **只存内存，不扫盘**：磁盘上的 `report.json` 是留给人与后续工具的证物，不是查询后端——
 * 每次请求去遍历 `Saved/GameTest/**` 会把成本与 IO 绑到历史长度上，还会把上一个编辑器会话
 * 的产物混进来（语义从"本次会话的历史"漂成"磁盘上所有报告"）。
 *
 * 纯逻辑（只操作 JSON 对象），可脱离引擎单测。
 */
class GAMETEST_API GameTestRunStore
{
public:
	/** 上限 ≤ 0 时按 1 处理，避免"什么都不留"这种静默失效。 */
	explicit GameTestRunStore(int32 InCapacity = 20);

	void SetCapacity(int32 InCapacity);

	/** 记一份完整报告；超出上限时从最旧的一端淘汰。 */
	void Add(const TSharedPtr<FJsonObject>& InReportJson);

	/** 摘要清单，最近一次在最前。 */
	TArray<TSharedPtr<FJsonObject>> ListSummaries() const;

	/** 按 `runId` 找完整报告；没有则返回无效指针。 */
	TSharedPtr<FJsonObject> Find(const FString& InRunId) const;

	int32 Num() const
	{
		return Reports.Num();
	}

	void Reset();

	/** 报告 → 摘要（`runId`/`name`/`revision`/起止时间/成败/步数/耗时）。 */
	static TSharedPtr<FJsonObject> MakeSummary(const FJsonObject& InReportJson);

private:
	TArray<TSharedPtr<FJsonObject>> Reports;
	int32 Capacity = 20;
};
