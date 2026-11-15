// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleInterface.h"

GAMETEST_API DECLARE_LOG_CATEGORY_EXTERN(LogGameTest, Log, All);

/**
 * 插件模块：登记 UELink 扩展和 `gt.*` 操作，注册控制台命令。真正的常驻对象是 `UGameTestSubsystem`
 * （UEngineSubsystem，编辑器与游戏进程都有），执行器、句柄与产物都挂在它下面。
 */
class FGameTestModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface interface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	//~ End IModuleInterface interface

private:
	void RegisterConsoleCommands();
	void UnregisterConsoleCommands();

	TArray<IConsoleObject*> ConsoleCommands;
};
