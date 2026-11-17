// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class SWidget;

/**
 * 录制截图的参数（`gt.record.start` 的 `shots`，面板「设置 → 插件 → UE」里改；都是窗口像素）。
 * 截被点控件的框，每边外扩 `clamp(PadRatio × 那一边的长度, PadMin, PadMax)`；不足 Min 补到 Min，
 * 超过 Max 的整张按比例缩小（不裁掉，大列表条目也完整）。
 */
struct GAMETEST_API FGameTestShotOptions
{
	bool bEnabled = true;
	double PadRatio = 0.5;
	int32 PadMin = 24;
	int32 PadMax = 160;
	int32 MinWidth = 200;
	int32 MinHeight = 120;
	int32 MaxWidth = 640;
	int32 MaxHeight = 360;
	/** 界面切换时存的整屏缩略图宽度。 */
	int32 ScreenWidth = 480;
	/** 点在游戏画面上（不是 UI）时，以光标为中心截多大的方块。 */
	int32 GameClickBox = 120;

	/** `shots` 字段：`false` 关掉，对象时逐项覆盖；不认识的值报错（拼错了不悄悄用默认值）。 */
	static bool FromJson(const TSharedPtr<class FJsonValue>& InValue, FGameTestShotOptions& OutOptions, FString& OutError);
};

/**
 * 录制时的截图（GameTest 1.3.1）：每次 UI 点击、拖动起点、点在游戏画面上时截一小块，被点的控件描红框、点击位置画十字，
 * 写进步骤 `hint.shot`；顶层界面变了时存一张整屏缩略图写进 `hint.screen`；点完 2 秒内新打开的界面写进 `hint.opened`。
 * 面板拿截图做 OCR 补步骤名（`hint.ocr`），「AI 整理」时把图给 Agent 看。
 *
 * - 截的时机是**按下**那一刻（录制器处理按下时）：按钮默认松开才触发，这时界面还没变，图里是「点之前」。
 * - 截图走 `FSlateApplication::TakeScreenshot`（同步，每次点击卡一下，只在录制时有）；裁剪、描边、压 PNG、写盘都在后台线程。
 * - 文件在 `Saved/<输出目录>/shots/<开始录制的时间>/`，不放场景根：场景跟着项目文档进版本库，图会把仓库撑大。
 * - 截不了（-nullrhi、窗口没了）只是不写 shot，录制照常。只在游戏线程上用。
 */
class GAMETEST_API FGameTestRecordShots
{
public:
	void Begin(const FGameTestShotOptions& InOptions);
	void End();

	bool IsEnabled() const
	{
		return bActive && Options.bEnabled;
	}

	/** 按在 UI 控件上：截控件那一块（和必要的整屏缩略图）写进 `InHint`，并开始看这一下点完打开了什么界面。 */
	void OnUiPress(const TSharedPtr<SWidget>& InSlate, const FVector2D& InScreenPosition, const TSharedPtr<FJsonObject>& InHint);

	/** 点在游戏画面上：以光标为中心截一块。 */
	void OnGamePress(const FVector2D& InScreenPosition, const TSharedPtr<FJsonObject>& InHint);

	/** 每帧调：点完 0.2–2 秒内出现的新顶层界面写进那一步的 `hint.opened`。 */
	void Tick(double InNow);

	/**
	 * 裁剪框（纯计算，单测用）：`InTarget` 是控件在截图里的像素框，`InImage` 是整张截图的尺寸。
	 * 结果不出图；外扩后不足最小尺寸时以控件中心补齐；超过最大尺寸时原样返回（保存时再整体缩小）。
	 */
	static FIntRect ComputeCrop(const FIntRect& InTarget, const FIntPoint& InImage, const FGameTestShotOptions& InOptions);

	/** 缩到能放进 `InMax` 的尺寸（保持比例，不放大）。 */
	static FIntPoint FitInside(const FIntPoint& InSize, const FIntPoint& InMax);

private:
	/** 截游戏窗口：像素、尺寸，以及屏幕坐标 → 截图像素的换算。 */
	bool Capture(TArray<FColor>& OutPixels, FIntPoint& OutSize, FVector2D& OutWindowPos, double& OutScale) const;

	/** 下一张图的路径：`<目录>/<序号>.png`，序号从 001 起。 */
	FString NextPath(const TCHAR* InPrefix);

	/** 在屏上的顶层界面类名（排序去重）：判断「界面变了」和「点完打开了什么」。 */
	static TArray<FString> TopScreens();

	void MaybeSaveScreen(const TArray<FColor>& InPixels, const FIntPoint& InSize, const FVector2D& InWindowPos, double InScale);

	bool bActive = false;
	FGameTestShotOptions Options;
	FString Dir;
	int32 Counter = 0;

	TArray<FString> LastScreens;
	FString LastScreenPath;

	/** 等着补 opened 的那一步。 */
	struct FPendingOpened
	{
		TSharedPtr<FJsonObject> Hint;
		TArray<FString> Before;
		double PressedAt = 0.0;
		double CheckedAt = 0.0;
	};
	TOptional<FPendingOpened> PendingOpened;
};
