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
	 * 留空自动：项目外层同级有 ProjectDoc/docs 时用 ProjectDoc/docs/GameTest（P4 / SVN 项目的文档在项目外），否则用项目里的 docs/GameTest。
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
	 * UI 动作（click / drag / wheel / type）自动等待控件可操作的默认超时（毫秒）；步骤自己的 timeoutMs 优先。
	 * 比单步默认超时短：界面一般几秒内就出来，等满一分钟才报「没找到」太慢。
	 */
	UPROPERTY(EditAnywhere, config, Category = "GameTest", meta = (DisplayName = "UI 等待超时(ms)", ClampMin = "500"))
	int32 UiWaitTimeoutMs = 10000;

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
};
