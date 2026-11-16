// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameTest.h"

#include "Core/GameTestAction.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestRunner.h"
#include "Input/GameTestHandlerWatch.h"
#include "Integration/GameTestRpc.h"
#include "Observe/GameTestInspect.h"
#include "Record/GameTestRecorder.h"
#include "AgentPanelBridge.h"
#include "GameTestCompat.h"
#include "GameTestSubsystem.h"
#include "GameTestVersion.h"

#include "Engine/Engine.h"

DEFINE_LOG_CATEGORY(LogGameTest);

void FGameTestModule::StartupModule()
{
	// 对外只走 UELink：登记扩展（名片里能看到版本），操作挂到它的 /rpc 上。
	FAgentPanelBridge::Get().RegisterExtension(TEXT("GameTest"), GAMETEST_VERSION);
	// 处理者监听常开：录制、回放核对点没点到、gt.inspect 的「最近一次被谁接住」都要它，平时只记一个弱指针，开销可以忽略。
	GameTestHandlerWatch::Start();
	GameTestRpc::Register();
	RegisterConsoleCommands();
}

void FGameTestModule::ShutdownModule()
{
	UnregisterConsoleCommands();
	FGameTestRecorder::Get().Shutdown();
	GameTestHandlerWatch::Stop();
	GameTestRpc::Unregister();
	FAgentPanelBridge::Get().UnregisterExtension(TEXT("GameTest"));
}

namespace
{
	UGameTestSubsystem* GetTestSubsystem()
	{
		return GEngine != nullptr ? GEngine->GetEngineSubsystem<UGameTestSubsystem>() : nullptr;
	}

	/**
	 * `GameTest.Step <动作> [目标] [参数JSON]`
	 * 不经过 Agent 面板也能驱动一条链路：编辑器控制台、PIE 控制台、`-ExecCmds=` 三条路都能用。
	 */
	void HandleStepCommand(const TArray<FString>& InArgs)
	{
		if (InArgs.Num() == 0)
		{
			UE_LOG(LogGameTest, Display, TEXT("用法：GameTest.Step <动作> [目标] [参数JSON]；动作清单见 GameTest.Actions"));
			return;
		}

		TSharedPtr<FJsonObject> StepJson = MakeShared<FJsonObject>();
		StepJson->SetStringField(TEXT("action"), InArgs[0]);
		if (InArgs.Num() > 1)
		{
			StepJson->SetStringField(TEXT("target"), InArgs[1]);
		}
		if (InArgs.Num() > 2)
		{
			FString ParseError;
			const TSharedPtr<FJsonObject> Params = GameTestJson::ParseObjectFromText(InArgs[2], ParseError);
			if (!Params.IsValid())
			{
				UE_LOG(LogGameTest, Error, TEXT("参数 JSON 解析失败：%s"), *ParseError);
				return;
			}
			for (const auto& Pair : Params->Values)
			{
				const FString PairKey = GAMETEST_JSON_KEY(Pair.Key);
				StepJson->SetField(PairKey, Pair.Value);
			}
		}

		UGameTestSubsystem* Subsystem = GetTestSubsystem();
		if (Subsystem == nullptr)
		{
			UE_LOG(LogGameTest, Error, TEXT("GameTest 子系统不可用（引擎尚未就绪？）"));
			return;
		}

		TSharedPtr<FJsonObject> Result;
		Subsystem->RunStepInline(StepJson, Result);
		UE_LOG(LogGameTest, Display, TEXT("GameTest.Step => %s"),
			*GameTestJson::ToCompactString(Result));
	}

	void HandleHealthCommand(const TArray<FString>& InArgs)
	{
		UGameTestSubsystem* Subsystem = GetTestSubsystem();
		UE_LOG(LogGameTest, Display, TEXT("GameTest.Health => %s"),
			*GameTestJson::ToCompactString(Subsystem != nullptr ? Subsystem->BuildHealthJson() : MakeShared<FJsonObject>()));
	}

	/** `GameTest.Inspect [x y]`：光标（或视口归一化坐标）下是哪个控件、选择器怎么写，打印到日志。 */
	void HandleInspectCommand(const TArray<FString>& InArgs)
	{
		TOptional<FVector2D> Point;
		if (InArgs.Num() >= 2)
		{
			Point = FVector2D(FCString::Atod(*InArgs[0]), FCString::Atod(*InArgs[1]));
		}
		TSharedPtr<FJsonObject> Result;
		FString Error;
		if (!GameTestInspect::Inspect(Point, Result, Error))
		{
			UE_LOG(LogGameTest, Warning, TEXT("GameTest.Inspect：%s"), *Error);
			return;
		}
		UE_LOG(LogGameTest, Display, TEXT("GameTest.Inspect => %s"), *GameTestJson::ToCompactString(Result));
	}

	void HandleActionsCommand(const TArray<FString>& InArgs)
	{
		for (const FGameTestActionInfo& Info : GameTestActions::GetAll())
		{
			UE_LOG(LogGameTest, Display, TEXT("  %s%s%s  —— %s"),
				Info.bEditorOnly ? TEXT("[编辑器] ") : TEXT(""),
				*Info.Name.ToString(),
				Info.bNeedsTarget ? TEXT(" <target>") : TEXT(""),
				Info.Summary);
		}
	}
}

void FGameTestModule::RegisterConsoleCommands()
{
	IConsoleManager& Console = IConsoleManager::Get();

	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("GameTest.Step"),
		TEXT("执行单个测试步骤：GameTest.Step <动作> [目标] [参数JSON]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleStepCommand)));

	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("GameTest.Health"),
		TEXT("打印编辑器/PIE 状态与本地通道信息"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleHealthCommand)));

	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("GameTest.Actions"),
		TEXT("列出全部可用动作"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleActionsCommand)));

	ConsoleCommands.Add(Console.RegisterConsoleCommand(
		TEXT("GameTest.Inspect"),
		TEXT("看光标下是哪个控件、选择器怎么写：GameTest.Inspect [x y]（视口归一化坐标，可省）"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&HandleInspectCommand)));
}

void FGameTestModule::UnregisterConsoleCommands()
{
	for (IConsoleObject* Command : ConsoleCommands)
	{
		if (Command != nullptr)
		{
			IConsoleManager::Get().UnregisterConsoleObject(Command);
		}
	}
	ConsoleCommands.Reset();
}

IMPLEMENT_MODULE(FGameTestModule, GameTest)
