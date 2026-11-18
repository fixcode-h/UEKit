// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameTestEditorOps.h"
#include "Api/GameTestCppApiScanner.h"

#include "Containers/Ticker.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameTestEditor, Log, All);

/**
 * 编辑器启动、编译（热重载）、Live Coding 之后重扫 C++ 项目接口（协议第 8.2 节）。
 * 内容没变时不写文件；扫描出错只记日志，不打断编辑器。
 */
class FGameTestEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		GameTestEditorOps::Register();
		// 模块在 PostEngineInit 加载，这时项目模块的类都注册完了；放到下一帧做，不拖慢编辑器启动
		StartupTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			ScanQuietly(TEXT("编辑器启动"));
			return false;
		}));
		// Live Coding 打完补丁、热重载编完都走这个委托
		ReloadHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddLambda([](EReloadCompleteReason)
		{
			ScanQuietly(TEXT("编译完成"));
		});
	}

	virtual void ShutdownModule() override
	{
		FTSTicker::GetCoreTicker().RemoveTicker(StartupTick);
		FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(ReloadHandle);
		GameTestEditorOps::Unregister();
	}

private:
	static void ScanQuietly(const TCHAR* InWhy)
	{
		FGameTestCppApiScanResult Result;
		FString Error;
		if (!GameTestCppApiScanner::ScanAndWrite(/*bDryRun=*/false, Result, Error))
		{
			UE_LOG(LogGameTestEditor, Warning, TEXT("GameTest 项目接口扫描失败（%s）：%s"), InWhy, *Error);
			return;
		}
		if (Result.bChanged)
		{
			UE_LOG(LogGameTestEditor, Display, TEXT("GameTest 项目接口已更新（%s）：%d 个接口，%d 个不能用，写入 %s"),
				InWhy, Result.Items, Result.Rejected, *Result.Path);
		}
	}

	FTSTicker::FDelegateHandle StartupTick;
	FDelegateHandle ReloadHandle;
};

IMPLEMENT_MODULE(FGameTestEditorModule, GameTestEditor)
