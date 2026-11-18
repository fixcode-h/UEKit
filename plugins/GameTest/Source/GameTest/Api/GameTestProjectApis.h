// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 项目接口（协议第 8.2 节）：把场景根 `_api.json` 里的 C++ / Lua 接口登记进接口注册表。
 *
 * `_api.json` 有两个写入方：编辑器里的 GameTestEditor（C++ 段，写完会通知这里）和面板（Lua 段，进程外改文件）。
 * 所以除了监听通知，每次运行开始和 `gt.api` 时按文件修改时间再看一眼，变了才重读。
 */
class GAMETEST_API GameTestProjectApis
{
public:
	/** 文件变了（或从没读过）就重读并重新登记；没变什么都不做。返回读文件的错误（没有文件不算错）。 */
	static FString Refresh();

	/** 不管时间戳，强制重读（GameTestEditor 写完 cpp 段后）。 */
	static FString Reload();
};
