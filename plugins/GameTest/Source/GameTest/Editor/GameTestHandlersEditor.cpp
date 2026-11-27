// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestHandlerShared.h"

#include "Core/GameTestJson.h"
#include "Core/GameTestRunConfig.h"
#include "Core/GameTestWorld.h"
#include "Editor/GameTestEditorApi.h"

#include "Dom/JsonObject.h"

namespace GameTestHandlers
{
	EGameTestStepStatus HandleEditorEnsure(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;

		bool bAssetsLoaded = true;
		bool bNoModal = true;
		GameTestJson::TryGetBool(Params, TEXT("assetsLoaded"), bAssetsLoaded);
		GameTestJson::TryGetBool(Params, TEXT("noModal"), bNoModal);

		if (!GameTestEditor::IsAvailable())
		{
			InContext.Message = TEXT("当前进程没有编辑器（独立进程或打包构建）；editor.ensure 不适用");
			return StepFailed;
		}

		FString ReadyMessage;
		if (GameTestEditor::CheckReady(bAssetsLoaded, bNoModal, ReadyMessage))
		{
			InContext.Message = ReadyMessage;
			return StepSucceeded;
		}

		// 没就绪不是错误：资产在加载、模态框会自己消失，都是"等一会儿就好"。
		InContext.PendingPredicate = [bAssetsLoaded, bNoModal](double)
		{
			FString Ignored;
			return GameTestEditor::CheckReady(bAssetsLoaded, bNoModal, Ignored);
		};
		InContext.Message = FString::Printf(TEXT("等待编辑器就绪：%s"), *ReadyMessage);
		return StepPending;
	}

	EGameTestStepStatus HandlePieStart(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;

		FGameTestPieRequest Request;
		GameTestJson::TryGetString(Params, TEXT("map"), Request.Map);
		GameTestJson::TryGetInt(Params, TEXT("width"), Request.Width);
		GameTestJson::TryGetInt(Params, TEXT("height"), Request.Height);
		GameTestJson::TryGetString(Params, TEXT("netMode"), Request.NetMode);
		GameTestJson::TryGetInt(Params, TEXT("players"), Request.Players);
		// 开在哪：步骤的 window > 运行配置的 pieWindow > 选中的视口（2.5.0，用户 2026-11-27 要默认不弹独立窗口）
		if (!GameTestJson::TryGetString(Params, TEXT("window"), Request.Window) || Request.Window.IsEmpty())
		{
			FGameTestRunConfig RunConfig;
			FString ConfigError;
			if (GameTestRunConfig::Load(RunConfig, ConfigError))
			{
				Request.Window = RunConfig.PieWindow;
			}
		}
		if (Request.Players < 1)
		{
			InContext.Message = TEXT("players 至少为 1");
			return StepFailed;
		}

		// PIE 已经在跑同一张地图（单人）时沿用它：录制时 PIE 开着，录出来的场景第一步就是 pie.start，
		// 用户常常是在 PIE 还开着时直接点回放，这时报「先 pie.stop」会让整场在第一步就失败。
		// 换地图、开监听服务器这类和当前会话对不上的请求照旧报错，不悄悄用错的会话跑。
		const bool bPlainRequest = Request.Players <= 1 && !Request.NetMode.Equals(TEXT("listenServer"), ESearchCase::IgnoreCase);
		if (bPlainRequest && GameTestEditor::IsPieRunning()
			&& (Request.Map.IsEmpty() || Request.Map.Equals(GameTestEditor::GetEditorLevelPath(), ESearchCase::IgnoreCase)))
		{
			InContext.PendingPredicate = [](double)
			{
				return GameTestWorld::AreAllGameWorldsPlayable();
			};
			InContext.Message = TEXT("PIE 已经在跑同一张地图，沿用当前会话（要从头开始请先 pie.stop）");
			return StepPending;
		}

		UObject* KeepAlive = nullptr;
		FString Message;
		if (!GameTestEditor::StartPIE(Request, KeepAlive, Message))
		{
			InContext.Message = Message;
			return StepFailed;
		}

		if (KeepAlive != nullptr && InContext.RootedObjects != nullptr)
		{
			// 临时 play settings 必须活到会话启动，否则被 GC 掉会让请求失效。
			InContext.RootedObjects->Empty();
			InContext.RootedObjects->Emplace(KeepAlive);
		}

		// 启动是"请求入队"：这里等到**全部**世界（服务器 + 各客户端）都跑起来，省掉一条惯例性的 wait。
		InContext.PendingPredicate = [](double)
		{
			return GameTestWorld::AreAllGameWorldsPlayable();
		};
		InContext.Message = Message + TEXT("；等待全部世界跑起来");
		return StepPending;
	}

	EGameTestStepStatus HandlePieStop(FGameTestStepContext& InContext)
	{
		FString Message;
		if (!GameTestEditor::StopPIE(Message))
		{
			InContext.Message = Message;
			return StepFailed;
		}

		InContext.PendingPredicate = [](double)
		{
			return !GameTestEditor::IsPieRunning();
		};
		InContext.Message = Message;
		return StepPending;
	}

	EGameTestStepStatus HandleLevelLoad(FGameTestStepContext& InContext)
	{
		FString Map;
		if (!GameTestJson::TryGetString(*InContext.Step->Params, TEXT("map"), Map) || Map.IsEmpty())
		{
			InContext.Message = TEXT("level.load 需要 map（关卡资产路径，例如 /Game/Map/TestMap）");
			return StepFailed;
		}

		if (!GameTestEditor::LoadLevel(Map, InContext.Message))
		{
			return StepFailed;
		}

		return StepSucceeded;
	}
}
