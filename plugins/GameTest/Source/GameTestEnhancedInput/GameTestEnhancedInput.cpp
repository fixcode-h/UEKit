// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameTestInjection.h"

#include "Core/GameTestAction.h"
#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestHooks.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestWorld.h"
#include "GameTest.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "InputAction.h"
#include "Modules/ModuleManager.h"

using namespace GameTestHandlers;

namespace
{
	/**
	 * 本次 run 启动过的输入注入。
	 *
	 * 引擎**没有**"批量清除注入"的接口（只有逐个 `StopContinuousInputInjectionForAction`），
	 * 而场景失败 / 取消后残留的注入会让载具一直往前开，污染之后所有观察与下一条场景，
	 * 所以自己记账、run 收尾时逐个停掉。存弱引用：注入本身由 EnhancedInput 子系统按资产持有。
	 */
	TArray<TWeakObjectPtr<const UInputAction>>& GetInjected()
	{
		static TArray<TWeakObjectPtr<const UInputAction>> Injected;
		return Injected;
	}

	int32 CountActiveInjections()
	{
		int32 Count = 0;
		for (const TWeakObjectPtr<const UInputAction>& Action : GetInjected())
		{
			Count += Action.IsValid() ? 1 : 0;
		}
		return Count;
	}

	void ClearInjections()
	{
		UWorld* World = GameTestWorld::FindGameWorld();
		int32 Stopped = 0;
		if (World != nullptr)
		{
			for (const TWeakObjectPtr<const UInputAction>& Action : GetInjected())
			{
				if (Action.IsValid())
				{
					FString Message;
					GameTestInjection::StopInjection(*World, *Action.Get(), Message);
					++Stopped;
				}
			}
		}
		// 世界没了（PIE 退出）时注入随本地玩家一起销毁，账本照样要清，否则下次的活跃计数会带上残留。
		GetInjected().Reset();
		if (Stopped > 0)
		{
			UE_LOG(LogGameTest, Display, TEXT("run 收尾：已停掉 %d 个输入注入"), Stopped);
		}
	}

	EGameTestStepStatus HandleInputStart(FGameTestStepContext& InContext)
	{
		FString ActionPath;
		// 字段名刻意是 asset 不是 action：后者是公共字段里的动作名，同名会把资产路径当成动作名去解析。
		if (!GameTestJson::TryGetString(*InContext.Step->Params, TEXT("asset"), ActionPath) || ActionPath.IsEmpty())
		{
			InContext.Message = TEXT("input.start 需要 asset（输入资产路径，例如 /Game/Input/IA_Throttle）");
			return StepFailed;
		}

		double Value = 1.0;
		GameTestJson::TryGetNumber(*InContext.Step->Params, TEXT("value"), Value);

		FString Error;
		UWorld* World = ResolveStepWorld(InContext, Error);
		if (World == nullptr)
		{
			InContext.Message = Error;
			return StepFailed;
		}

		UInputAction* Action = GameTestInjection::ResolveInputAction(ActionPath, Error);
		if (Action == nullptr)
		{
			InContext.Message = Error;
			return StepFailed;
		}

		if (!GameTestInjection::StartInjection(*World, *Action, Value, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		// 登记进账本：run 收尾时靠它逐个停掉（引擎没有批量清除接口）。
		GetInjected().AddUnique(Action);

		InContext.Payload->SetStringField(TEXT("action"), Action->GetName());
		InContext.Payload->SetStringField(TEXT("path"), Action->GetPathName());
		InContext.Payload->SetStringField(TEXT("world"), GameTestWorld::DescribeWorld(World));
		InContext.Payload->SetNumberField(TEXT("value"), Value);
		InContext.Message = Error;
		return StepSucceeded;
	}

	EGameTestStepStatus HandleInputStop(FGameTestStepContext& InContext)
	{
		FString ActionPath;
		if (!GameTestJson::TryGetString(*InContext.Step->Params, TEXT("asset"), ActionPath) || ActionPath.IsEmpty())
		{
			InContext.Message = TEXT("input.stop 需要 asset（输入资产路径，例如 /Game/Input/IA_Throttle）");
			return StepFailed;
		}

		FString Error;
		UWorld* World = ResolveStepWorld(InContext, Error);
		if (World == nullptr)
		{
			InContext.Message = Error;
			return StepFailed;
		}

		UInputAction* Action = GameTestInjection::ResolveInputAction(ActionPath, Error);
		if (Action == nullptr)
		{
			InContext.Message = Error;
			return StepFailed;
		}

		if (!GameTestInjection::StopInjection(*World, *Action, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		GetInjected().Remove(Action);

		InContext.Payload->SetStringField(TEXT("action"), Action->GetName());
		InContext.Payload->SetStringField(TEXT("world"), GameTestWorld::DescribeWorld(World));
		InContext.Message = Error;
		return StepSucceeded;
	}
}

/**
 * GameTest 的 EnhancedInput 部分：`input.start` / `input.stop`（持续注入输入资产）。
 *
 * 单独成模块是因为 EnhancedInput 不一定开：主模块不依赖它。run 收尾时清掉本次的注入，
 * health 里报活跃注入数，让「清空」这件事可被验证。
 */
class FGameTestEnhancedInputModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		GameTestActions::Register({ TEXT("input.start"), TEXT("asset,value,client"), false, false,
			TEXT("按输入资产开始持续注入（每 tick 重发、走完整 modifier/trigger 链，与真实按键同一条路）；重复注入同一资产以最后一次的值为准，run 结束或取消时自动清空。字段名是 asset 不是 action——action 是公共字段里的动作名；多客户端会话用 client 选注入到哪一端（缺省 server）"),
			&HandleInputStart, TEXT("GameTestEnhancedInput") });
		GameTestActions::Register({ TEXT("input.stop"), TEXT("asset,client"), false, false,
			TEXT("停止某个输入资产的持续注入；对没在注入的资产会失败并说明（字段名是 asset，理由同 input.start；client 须与注入时一致）"),
			&HandleInputStop, TEXT("GameTestEnhancedInput") });

		RunFinishedHandle = GameTestHooks::OnRunFinished().AddStatic(&ClearInjections);
		HealthHandle = GameTestHooks::OnHealth().AddLambda([](FJsonObject& OutHealth)
		{
			OutHealth.SetNumberField(TEXT("activeInjections"), CountActiveInjections());
		});
	}

	virtual void ShutdownModule() override
	{
		GameTestHooks::OnRunFinished().Remove(RunFinishedHandle);
		GameTestHooks::OnHealth().Remove(HealthHandle);
		GameTestActions::Unregister(TEXT("input.start"));
		GameTestActions::Unregister(TEXT("input.stop"));
		GetInjected().Reset();
	}

private:
	FDelegateHandle RunFinishedHandle;
	FDelegateHandle HealthHandle;
};

IMPLEMENT_MODULE(FGameTestEnhancedInputModule, GameTestEnhancedInput)
