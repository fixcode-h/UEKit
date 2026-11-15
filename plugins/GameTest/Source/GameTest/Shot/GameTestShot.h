// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UWorld;
struct FGameTestWidgetNode;

/**
 * 截图。
 *
 * 走 `FSlateApplication::TakeScreenshot(游戏窗口)`——同步拿到像素、文件落在我们指定的绝对路径、
 * 还能把控件框画上去。引擎自带的 `FScreenshotRequest` 是异步的、路径由引擎决定
 * （`Saved/Screenshots/<平台>/`），而且拿不到像素就没法标注；不过它的 "含 UI" 分支
 * 本身就是调同一个 Slate 接口（`UGameViewportClient::ProcessScreenShots`），所以两者画面一致。
 *
 * 前提：**必须有真实 RHI**。`-nullrhi` 下 Slate 用空渲染器，截图会是全 0 像素。
 */
class GAMETEST_API GameTestShot
{
public:
	/** 产物根目录（项目 Saved 下），绝对路径。 */
	static FString GetOutputRootDir();

	/** 相对项目根的路径，给 AI 读图用。 */
	static FString ToProjectRelative(const FString& InAbsolutePath);

	/**
	 * 截当前游戏窗口。
	 * @param InAnnotateNodes 非空时按控件的窗口坐标画编号框，图例写进 OutPayload 的 boxes。
	 */
	static bool CaptureGameWindow(
		UWorld* InWorld,
		const FString& InAbsolutePath,
		const TArray<TSharedPtr<FGameTestWidgetNode>>* InAnnotateNodes,
		FString& OutMessage,
		TSharedPtr<FJsonObject>& OutPayload);
};
