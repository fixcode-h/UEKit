// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "GameTestSettings.generated.h"

/**
 * 插件配置（项目级，进版本管理）。落在 `Config/DefaultGame.ini` 的 `[/Script/GameTest.GameTestSettings]` 段。
 *
 * 只放「这个项目的测试怎么跑」：场景目录、超时、步间隔。没有端口和开关——对外的通道是 UELink，
 * 令牌、只听本机、多实例都由它管；个人偏好（筛选、分组）归 Agent 面板的网页。
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Game Test"))
class GAMETEST_API UGameTestSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UGameTestSettings();

	//~ Begin UDeveloperSettings interface
	virtual FName GetCategoryName() const override;
	//~ End UDeveloperSettings interface

	/**
	 * 场景 JSON 的根目录（相对项目根，也接受绝对路径）；录制生成的场景也存这里。
	 * 留空自动：用 Agent 面板的项目文档根下的 GameTest（面板写在 Saved/AgentPanel/docs-root.txt，P4 / SVN 项目是各分支共用的 ProjectDoc/docs）；
	 * 没有面板时项目外层同级有 ProjectDoc/docs 就用 ProjectDoc/docs/GameTest，否则用项目里的 docs/GameTest。
	 */
	UPROPERTY(EditAnywhere, config, Category = "GameTest", meta = (DisplayName = "场景根目录"))
	FString ScenarioRootDir;

	/** 实际用的场景根目录（绝对路径）：设置了就按设置，没设置按上面的自动规则 */
	static FString ResolveScenarioRootDir();

	/** 截图与报告的根目录（相对项目 Saved 目录）。 */
	UPROPERTY(EditAnywhere, config, Category = "GameTest", meta = (DisplayName = "产物根目录"))
	FString OutputDir = TEXT("GameTest");

	/** 单步默认超时（毫秒）；步骤自己的 timeoutMs 优先。 */
	UPROPERTY(EditAnywhere, config, Category = "GameTest", meta = (DisplayName = "单步默认超时(ms)", ClampMin = "1000"))
	int32 DefaultStepTimeoutMs = 60000;

	/**
	 * UI 动作（click / drag / wheel / type、wait widget）等目标控件出现、可操作的上限（毫秒）。
	 * 0 = 不限：界面没出来就一直等（加载慢、网络慢时不误报），卡住了在面板上停止。
	 * 优先级：步骤的 timeoutMs > 场景的 uiWaitTimeoutMs > 项目运行配置（场景根的 _settings.json）> 这里。
	 */
	UPROPERTY(EditAnywhere, config, Category = "GameTest", meta = (DisplayName = "UI 等待超时(ms，0=不限)", ClampMin = "0"))
	int32 UiWaitTimeoutMs = 0;

	/**
	 * 每步**成功之后**的间隔（毫秒）：让刚被操作的那个控件在游戏窗口里亮够、看得清。
	 * 0 = 不停顿（跑批量回归时设它）；默认 1000 是"看着像演示"的速度。失败不延迟——失败要立刻停在现场。
	 */
	UPROPERTY(EditAnywhere, config, Category = "GameTest", meta = (DisplayName = "步间隔(ms)", ClampMin = "0", ClampMax = "60000"))
	int32 StepDelayMs = 1000;

	/** 快照默认是否带几何信息（窗口坐标 + 尺寸）。关掉可以把返回体压小。 */
	UPROPERTY(EditAnywhere, config, Category = "GameTest", meta = (DisplayName = "快照默认带几何"))
	bool bSnapshotWithGeometry = true;

	/**
	 * 内存里保留多少次 run 的报告（`gt.runs` 能回查的范围）。
	 * 每次收尾的报告同时会落盘到产物目录的 `report.json`，这里只决定"不查磁盘能回看几次"。
	 */
	UPROPERTY(EditAnywhere, config, Category = "GameTest", meta = (DisplayName = "run 历史上限", ClampMin = "1", ClampMax = "1000"))
	int32 MaxRunHistory = 20;

	/**
	 * 一步失败时自动留证据：截图、控件快照（含选择器的候选）、失败前后 50 行日志，写进这次 run 产物目录的 failures/ 下，
	 * 报告里只记路径。一次 run 最多留 5 份。关掉可以省下失败时的一次截图。
	 */
	UPROPERTY(EditAnywhere, config, Category = "GameTest", meta = (DisplayName = "失败时留证据"))
	bool bCaptureFailureEvidence = true;
};
