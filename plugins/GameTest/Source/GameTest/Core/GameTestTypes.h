// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UWidget;
class UWorld;

/**
 * 单步执行结果。Pending 只出现在跨帧动作（`wait`）上：执行器每帧问一次谓词，
 * 满足才落 Succeeded/Failed —— 不允许用 sleep 阻塞游戏线程。
 */
enum class EGameTestStepStatus : uint8
{
	Succeeded,
	Failed,
	Skipped,
	Pending,
};

/** 失败时怎么继续。默认 stop：一步失败就停在原地，让人/AI 看到现场。 */
enum class EGameTestFailMode : uint8
{
	Stop,
	Continue,
};

/**
 * 一条步骤。`Params` 保留整份 JSON：动作各自的字段由动作自己读，
 * 词汇表（`GameTestActions`）负责在动资产/动控件**之前**把拼错的字段报出来。
 */
struct FGameTestStep
{
	FName Action;
	FString Target;
	FString Note;
	int32 TimeoutMs = 0;
	EGameTestFailMode FailMode = EGameTestFailMode::Stop;
	int32 Index = 0;
	/** 运行配置里的前置步骤（`_settings.json` 的 setup）：Index 是它在前置步骤里的序号，不占场景的步骤序号。 */
	bool bSetup = false;
	TSharedPtr<FJsonObject> Params;

	/** 给人看的「第 N 步」/「前置第 N 步」（日志、失败证据目录名）。 */
	FString Label() const
	{
		return bSetup ? FString::Printf(TEXT("前置第 %d 步"), Index + 1) : FString::Printf(TEXT("第 %d 步"), Index + 1);
	}
};

/** 一步的记录：进报告、也进 `job` 查询结果。 */
struct GAMETEST_API FGameTestStepRecord
{
	FGameTestStep Step;
	EGameTestStepStatus Status = EGameTestStepStatus::Skipped;
	FString Message;
	/** 动作自己的结果（快照、截图路径、命中列表……）。 */
	TSharedPtr<FJsonObject> Payload;
	double StartedAt = 0.0;
	double FinishedAt = 0.0;

	TSharedPtr<FJsonObject> ToJson() const;
};

/** 一次 run（单步或整场景）的报告。 */
struct GAMETEST_API FGameTestReport
{
	/** 本次 run 的标识，同时是 `gt.runs/<runId>` 的路径段。 */
	FString RunId;

	FString Name;

	/**
	 * 场景文件相对场景根的路径（`smoke/main-menu.json`）；临时步骤和单步 run 为空。
	 * 场景名可以重复、可以改，按场景记历史和认「这次跑的是哪个文件」只能靠它。
	 */
	FString Scenario;

	/** 场景文件里的 `revision`；单步 run 与没写该字段的场景为 0。用来判断"这次红的是不是改过的那份配置"。 */
	int32 Revision = 0;

	FString StartedAtUtc;
	FString FinishedAtUtc;

	/** 墙钟耗时（毫秒）。逐步耗时之和会漏掉步间隔，历史摘要要的是整次 run 的时长。 */
	double DurationMs = 0.0;

	bool bSucceeded = false;
	TArray<FGameTestStepRecord> Steps;

	int32 CountByStatus(EGameTestStepStatus InStatus) const;

	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * 起一次 run 的完整请求。
 *
 * 收成结构体是因为 `FGameTestRunner::StartRun` 的裸参数已经到 7 个（名字、步骤、产物目录、
 * 句柄表、保活槽、完成回调、错误输出），再叠 runId 与 revision 只会更难读——这几个字段本就是
 * 同一件事（"这次要跑的是什么"）。解析留在调用方（子系统）：那里本来就要把第几步错在哪
 * 拼成给调用方的消息，搬进来只是把同一段代码挪个地方。
 */
struct GAMETEST_API FGameTestRunRequest
{
	/** 进报告的场景名（人读的那个）。 */
	FString Name;

	/** 场景文件相对场景根的路径；临时步骤为空（见 FGameTestReport::Scenario）。 */
	FString Scenario;

	/** 场景文件里的 `revision`；单步 run 为 0。 */
	int32 Revision = 0;

	/** URL 安全标识（`GameTestRun::MakeRunId` 生成），进报告与 `gt.runs/<runId>`。 */
	FString RunId;

	/** 本次 run 的产物目录（绝对路径）。 */
	FString OutputDir;

	/** 已解析的步骤队列（解析与拒绝都在调用方完成）。 */
	TArray<FGameTestStep> Steps;

	/**
	 * 单步默认超时（毫秒）；步骤自己的 `timeoutMs` 优先。
	 *
	 * 由子系统从设置里读出后注入，而不是执行器自己去 `GetDefault<UGameTestSettings>()`：
	 * 状态机读全局既让它不可单测（测试得去改 CDO），也让依赖藏起来；顺带把"每步都读一次设置"
	 * 变成"每次 run 读一次"。
	 */
	int32 DefaultStepTimeoutMs = 60000;

	/** 每步**成功之后**的间隔（毫秒）；0 = 不停顿。来源同 `DefaultStepTimeoutMs`。 */
	int32 StepDelayMs = 0;

	/**
	 * UI 动作等目标控件出现、可操作的上限（毫秒）；0 = 不限，一直等到控件出来或有人 stop。
	 * 子系统按「场景 > 项目运行配置 > 项目设置」的顺序定下来再注入。
	 */
	int32 UiWaitTimeoutMs = 0;

	/** click 没写 via 时用的点击方式：code（默认，调控件委托，不动鼠标）或 pointer（真实鼠标）。来源同上。 */
	FString ClickVia = TEXT("code");

	/**
	 * 相邻两个 UI 操作（click、drag、type……）之间至少隔多久（毫秒）：从上一个 UI 操作结束算，
	 * 中间的 wait 等步骤用掉的时间也算进去；0 = 不隔。来自运行配置的 uiStepGapMs。
	 */
	int32 UiStepGapMs = 0;
};

/** 状态名（进 JSON 与日志，保持英文以便机器读）。 */
class GAMETEST_API GameTestStatus
{
public:
	static const TCHAR* ToString(EGameTestStepStatus InStatus);
};

/**
 * run 标识的生成（纯逻辑，可单测）。
 *
 * 场景名是作者写的自由文本（可以含中文、空格与标点），而 runId 要进 `gt.runs/<runId>`
 * 的路径段——直接拿场景名拼 URL 就得在每个调用点各做一次转义，还得指望它们都做对。
 * 这里把它折成只含 `[a-z0-9-]` 的 ASCII，调用方拿到的就是 URL 安全的值。
 */
class GAMETEST_API GameTestRun
{
public:
	/** `MakeRunId("showcase-smoke", 3)` → `showcase-smoke-3`；名字折不出字符时退回 `run`。 */
	static FString MakeRunId(const FString& InName, int32 InSequence);
};
