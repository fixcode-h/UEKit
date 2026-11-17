// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UWidget;

/**
 * `gt.keys.preview`：在游戏画面上框出按某个条件命中的条目，几秒后自己消失（1.4.0）。
 * 一个命中画绿框，多个画橙框——挑标识时一眼看出「按它认」会不会认错。
 * 框每帧按控件当前的位置画（列表滚动也跟得住）；HitTestInvisible，不挡游戏输入。只在游戏线程上用。
 */
namespace GameTestKeyPreview
{
	/** 框出这些控件（空列表 = 清掉）；再调一次换成新的、重新计时。 */
	GAMETEST_API void Show(const TArray<UWidget*>& InWidgets, double InSeconds = 3.0);

	/** 模块关闭、PIE 停时清掉。 */
	GAMETEST_API void Clear();
}
