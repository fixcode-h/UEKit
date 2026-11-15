// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class SWidget;

/**
 * 「这次输入是哪个控件接住的」——录制识别控件、回放核对点没点到、`gt.inspect` 都靠它。
 *
 * **怎么知道的**：Slate 把一次输入沿命中路径（或焦点路径）从里往外冒泡，谁回 `FReply::Handled()` 谁就是处理者；
 * 引擎在路由的地方调 `FSlateDebugging::BroadcastInputEvent(类型, 事件, Reply, 处理者)`，
 * 这是引擎公开的调试扩展点（Slate 调试器、Widget Reflector 看事件用的同一条），只在 `WITH_SLATE_DEBUGGING`
 * （非 Shipping 默认打开）时有。这里订阅它，记下每类事件**最近一次被处理时**的处理者。
 *
 * 不按控件类分支：按钮是 `SButton`、自定义 WBP 是 `SObjectWidget`、`UImage` 绑的事件是 `SImage`、
 * 下拉项是 `STableRow`，都是同一条规则。和 UnLua 无关：Lua 怎么绑事件都发生在处理者内部。
 *
 * 没有 `WITH_SLATE_DEBUGGING` 时 `IsSupported()` 为 false，调用方退回命中路径推断。只在游戏线程上用。
 */
class GAMETEST_API GameTestHandlerWatch
{
public:
	/** 关心的事件。 */
	enum class EKind : uint8
	{
		MouseDown,
		MouseUp,
		MouseWheel,
		KeyDown,
		KeyUp,
		KeyChar,
		Num,
	};

	/** 一次被处理的事件。 */
	struct FHandled
	{
		TWeakPtr<SWidget> Handler;
		/** 每记一次 +1：调用方发事件前后比一下，就知道这次事件有没有人接。 */
		uint64 Serial = 0;
		uint64 Frame = 0;
		/** KeyChar 的字符；其他事件为 0。 */
		TCHAR Character = 0;
		/** 键盘事件的键名；其他事件为空。 */
		FName Key;
	};

	/** 这个进程能不能拿到处理者（编了 Slate 调试）。 */
	static bool IsSupported();

	/** 开始 / 停止订阅（模块启动、关闭时调）。 */
	static void Start();
	static void Stop();

	/** 某类事件最近一次被处理的记录。 */
	static FHandled GetLast(EKind InKind);

	/**
	 * 每次有事件被处理时回调（录制器用：按顺序拿到处理者和 KeyChar 字符）。
	 * 回调里不要再发输入事件。
	 */
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnHandled, EKind /*Kind*/, const FHandled& /*Record*/);
	static FOnHandled& OnHandled();
};
