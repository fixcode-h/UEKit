// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * 连着游戏编辑（1.10.0）：从游戏里抓控件（`gt.pick`）、条件和字段此刻的结果实时推送（`gt.watch`）。
 * 面板拿实时数据只收推送、不轮询；两者都是租约，面板关了不续，到期自己停。协议见 docs/game-test-script.md 第 10 节。
 */
namespace GameTestLive
{
	/** `gt.pick`：action = start（开始抓：游戏里 Ctrl+Alt 点、推实时画面）/ stop / at（面板画面上点的那一下）/ frame（马上要一帧）。 */
	bool Pick(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	/** `gt.watch`：登记一组只读查询，每隔 intervalMs 重算，变了才推 `gt.watch` 事件；stop 取消。 */
	bool Watch(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	/** 模块关闭：停定时器、拆输入预处理器、清掉所有会话。 */
	void Shutdown();

	//~ 下面是 Live 目录内部用的（各文件之间）
	/** 共用定时器：有抓取会话或监视时才挂，都没了就摘掉。 */
	void EnsureTicker();
	bool PickTick(double InNow);
	bool WatchTick(double InNow);
	void PickShutdown();
	void WatchShutdown();
}

/** 截游戏视口那一块（含 UMG）压成 JPEG：抓控件的实时画面用。 */
namespace GameTestLiveFrame
{
	/**
	 * 成功时 OutFrame = `{jpg, width, height}`（jpg 是 base64）。OutHash 是像素的校验和：和上一帧一样时调用方可以不推。
	 * InLastHash 和这一帧相同时不压 JPEG，OutFrame 置空并返回 true（省掉没变化时的编码开销）。
	 */
	bool Capture(int32 InMaxWidth, int32 InQuality, uint32 InLastHash, TSharedPtr<FJsonObject>& OutFrame, uint32& OutHash, FString& OutError);
}
