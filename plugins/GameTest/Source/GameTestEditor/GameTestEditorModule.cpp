// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameTestEditorOps.h"
#include "Deep/GameTestDeepRefCheck.h"
#include "EditorAct/GameTestEditorActions.h"
#include "EditorRec/GameTestEditorRecorder.h"

#include "Modules/ModuleManager.h"

/**
 * GameTest 的编辑器模块：编辑器里的操作（gt.*）、深度检查、编辑器操作录制。
 * 2.9.0 起不再在启动、编译后自动扫 C++ 项目接口（不许为测试专门写接口，用户 2026-11-28 定）；gt.api.scan 只留给面板迁移时列出旧接口。
 */
class FGameTestEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		GameTestEditorOps::Register();
		GameTestDeepRefCheck::Register();
		GameTestEditorActions::Register();
	}

	virtual void ShutdownModule() override
	{
		FGameTestEditorRecorder::Get().Shutdown();
		GameTestEditorActions::Unregister();
		GameTestDeepRefCheck::Unregister();
		GameTestEditorOps::Unregister();
	}
};

IMPLEMENT_MODULE(FGameTestEditorModule, GameTestEditor)
