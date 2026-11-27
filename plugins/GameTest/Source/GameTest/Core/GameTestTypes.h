// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Flow/GameTestDebugOptions.h"
#include "Core/Script/GameTestScriptOptions.h"

class FJsonObject;
class UWidget;
struct FGameTestGuardDef;
struct FGameTestScript;
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
	/**
	 * 已达成，跳过（1.6.0）：步骤的 `skipIf` 条件在回放到它时已经成立（勾已经勾上、已经领过），没有做。
	 * 算通过，和 Skipped 分开：Skipped 是「前一步失败或被取消、没轮到」，会让整次 run 判失败。
	 */
	Already,
	/** 步骤写了 `disabled: true`（1.9.0）：没做，算通过。 */
	Disabled,
};

/** 失败时怎么继续。默认 stop：一步失败就停在原地，让人/AI 看到现场。 */
enum class EGameTestFailMode : uint8
{
	Stop,
	Continue,
	/** 重做（1.9.0）：叶子从头再做这一步，块从块的第一步重来，最多 `retries` 次，还失败按 stop。 */
	Retry,
};

/**
 * 一条步骤。`Params` 保留整份 JSON：动作各自的字段由动作自己读，
 * 词汇表（`GameTestActions`）负责在动资产/动控件**之前**把拼错的字段报出来。
 */
struct FGameTestStep
{
	FName Action;
	FString Target;
	/** 节点名称（2.6.0）：很短的中文，给人看这一步做什么；Note 是详细说明。 */
	FString Title;
	FString Note;
	/** 0 = 没写（用默认）；小于 0 = 写了 0，不限时（用户 2026-11-19：0 表示不限），解析时把写了的 0 换成 -1 */
	int32 TimeoutMs = 0;
	EGameTestFailMode FailMode = EGameTestFailMode::Stop;
	int32 Index = 0;
	/** 运行配置里的前置步骤（`_settings.json` 的 setup）：Index 是它在前置步骤里的序号，不占场景的步骤序号。 */
	bool bSetup = false;
	/** 场景的收尾段（2.7.0，`teardown`）：Index 是它在收尾段里的序号；这里失败只记警告，不改整次 run 的结论。 */
	bool bTeardown = false;
	TSharedPtr<FJsonObject> Params;
	/** 步骤 id（1.9.0，场景里写的；没写为空）。 */
	FString Id;
	/** 在脚本里的路径（1.9.0，`steps.3.then.0`）；块里的步骤和 call 进去的步骤靠它认。 */
	FString Path;
	/** 在块里（不是主流程顶层）：Index 不再是「第几步」，标签改用 id / 路径。 */
	bool bNested = false;

	/** 给人看的「第 N 步」/「前置第 N 步」/「步骤 s3」（日志、失败证据目录名）。 */
	FString Label() const
	{
		if (bSetup)
		{
			return FString::Printf(TEXT("前置第 %d 步"), Index + 1);
		}
		if (bTeardown && !bNested)
		{
			return FString::Printf(TEXT("收尾第 %d 步"), Index + 1);
		}
		if (bNested)
		{
			return FString::Printf(TEXT("步骤 %s"), Id.IsEmpty() ? *Path : *Id);
		}
		return FString::Printf(TEXT("第 %d 步"), Index + 1);
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
	/** 这条是块（if、loop、call…）的记录（1.9.0）：块结束时记一条，块里的步骤各自另有记录，按 path 挂在它下面。 */
	bool bBlock = false;
	/** 在循环的第几轮里执行的（从 0）；不在循环里为 -1。 */
	int32 Iter = -1;
	/** 附带 GM 出错只记一笔（2.3.0）：状态照样是 Failed，但不算整次 run 失败，面板标黄。 */
	bool bWarn = false;

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
	/** 守护触发次数 `{id: 次数}`（1.9.0）；没触发过为空。 */
	TSharedPtr<FJsonObject> Guards;

	int32 CountByStatus(EGameTestStepStatus InStatus) const;

	TSharedPtr<FJsonObject> ToJson() const;
};

/**
 * 「已达成就跳过」（`skipIf`）的跑法（1.6.0）：运行配置的 `skipIf` 段，被场景顶层的 `skipIf` 覆盖，
 * 「全部照做」（`gt.run ignoreSkipIf`）时关掉。步骤里的 settleMs / verify / verifyTimeoutMs 再盖在上面。
 */
struct GAMETEST_API FGameTestSkipOptions
{
	bool bEnabled = true;
	/** 条件成立（或不成立）后再看这么久没变才下结论（毫秒）。 */
	int32 SettleMs = 300;
	/** 真做了以后核对条件成立。 */
	bool bVerify = true;
	/** 核对最多等多久（毫秒）。 */
	int32 VerifyTimeoutMs = 5000;
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

	/** move 步骤的默认设置：场景顶层的 `move` 盖在运行配置的 `move` 上，再由步骤字段覆盖。没有时为空。 */
	TSharedPtr<FJsonObject> MoveDefaults;
	/**
	 * 相邻两个 UI 操作（click、drag、type……）之间至少隔多久（毫秒）：从上一个 UI 操作结束算，
	 * 中间的 wait 等步骤用掉的时间也算进去；0 = 不隔。来自运行配置的 uiStepGapMs。
	 */
	int32 UiStepGapMs = 0;

	/** 已达成就跳过的跑法（运行配置 > 场景 > 这次运行的「全部照做」）。 */
	FGameTestSkipOptions SkipIf;

	//~ 1.9.0 脚本 -----------------------------------------------------------
	/**
	 * 解析好的脚本（v3 的块、守护、变量都在里面）。为空时执行器把 Steps 包成只有主流程的脚本——1.x 的调用方不用改。
	 * 用 TSharedPtr<FGameTestScript> 而不是值：节点树大、同一个子脚本被 call 多次时共享。
	 */
	TSharedPtr<const FGameTestScript> Script;
	/** gt.run 的 params（场景 params 的实参）。 */
	TSharedPtr<FJsonObject> Params;
	/** 场景根（绝对路径）：call、_api.json、_vars.json 相对它。 */
	FString ScenarioRoot;
	/** 运行配置里的脚本开关（allowLuaInvoke、allowLuaInCond、maxCallDepth、稳住默认值）。 */
	FGameTestScriptOptions ScriptOptions;
	/** 全局守护（运行配置的 guards），解析好的。 */
	TArray<TSharedRef<FGameTestGuardDef>> Guards;
	/** 断点、from / to、pauseOnFail。 */
	FGameTestDebugOptions Debug;
	/** 运行配置的 randomSeed：脚本自己的随机（FGameTestFlowContext::Random）也用它，没写就按时间。 */
	TOptional<int32> RandomSeed;
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
