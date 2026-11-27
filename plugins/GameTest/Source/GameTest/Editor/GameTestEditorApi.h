// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UObject;

/**
 * 启动 PIE 的请求参数。
 *
 * 收成结构体是因为字段已到 5 个（地图、分辨率、网络模式、玩家数），裸参数会越过调用预算
 * （ue-architecture-standards R2）。
 */
struct FGameTestPieRequest
{
	/** 空 = 用当前关卡。 */
	FString Map;
	int32 Width = 1280;
	int32 Height = 720;

	/** `standalone`（缺省）或 `listenServer`。 */
	FString NetMode;

	/** 玩家窗口数；listenServer 下至少 1（映射到引擎的 `PlayNumberOfClients`，1 = 只有主机）。 */
	int32 Players = 1;

	/**
	 * 开在哪（2.5.0）：`viewport`（缺省，选中的视口 = 关卡编辑器当前活动的关卡视口）或 `new`（独立窗口，Width / Height 只对它有效）。
	 * 找不到看得见的关卡视口时退回独立窗口。
	 */
	FString Window;
};

/**
 * 编辑器专属能力：PIE 启停、编辑器就绪门禁、关卡加载。
 *
 * 这些函数在**非编辑器构建里也存在**（返回明确的失败信息），而不是让动作消失——
 * 这样 `gt.actions` 的词汇表在编辑器与独立进程里是同一份，
 * AI 不会因为"这个动作突然不存在"而困惑，只会收到一句"当前进程没有编辑器"。
 */
class GAMETEST_API GameTestEditor
{
public:
	/** 编译期能力（不是运行期状态）：本次构建里有没有编辑器。 */
	static bool IsAvailable();

	/** 编辑器 + PIE 状态快照；health、步骤失败信息、快照头部都用它。 */
	static TSharedPtr<FJsonObject> BuildStatusJson();

	/** 就绪门禁：按需求检查，不满足时 OutMessage 说明差在哪一项。 */
	static bool CheckReady(bool bInRequireAssetsLoaded, bool bInRequireNoModal, FString& OutMessage);

	static bool IsPieRunning();

	/**
	 * 请求启动 PIE（默认浮动新窗口——它有自己的 SWindow，截图干净、不掺编辑器界面）。
	 * 请求只是入队，真正跑起来靠调用方的等待谓词（见 `HandlePieStart`）。
	 * @param OutKeepAlive 临时 play settings 对象；调用方必须持有强引用直到会话启动，否则会被 GC。
	 */
	static bool StartPIE(const FGameTestPieRequest& InRequest, UObject*& OutKeepAlive, FString& OutMessage);

	static bool StopPIE(FString& OutMessage);

	static bool LoadLevel(const FString& InMapPath, FString& OutMessage);

	/** 当前编辑器世界的关卡路径（用于 level.load 的默认与报错）。 */
	static FString GetEditorLevelPath();

	/**
	 * 开关"编辑器不在前台就节流"（`UEditorPerformanceSettings::bThrottleCPUWhenNotForeground`）。
	 *
	 * 这个设置默认开着，副作用是**编辑器失去焦点后几乎不 Tick、渲染也被关掉**
	 * （`EditorEngine.cpp:1798`）——PIE 世界随之停摆、截图也可能拿到旧帧。
	 * 引擎自己的编辑器自动化也是把它关掉的（`AutomationEditorCommon.cpp:1459`），这里照做。
	 *
	 * @param bOutPreviousValue 改动前的值，调用方负责在 run 结束时恢复。
	 */
	static bool SetBackgroundThrottleEnabled(bool bInEnabled, bool& bOutPreviousValue);
};
