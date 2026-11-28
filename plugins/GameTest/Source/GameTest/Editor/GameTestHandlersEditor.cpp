// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestHandlerShared.h"

#include "Core/GameTestJson.h"
#include "Core/GameTestRunConfig.h"
#include "Core/GameTestWorld.h"
#include "Editor/GameTestEditorApi.h"
#include "AgentPanelBridge.h"

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

		FString Unsaved = TEXT("error");
		GameTestJson::TryGetString(*InContext.Step->Params, TEXT("unsaved"), Unsaved);

		// 2.9.0：走 UELink 的 editor.map（和关卡编辑器「打开关卡」同一个入口，压住加载中的对话框）。
		// 以前直接 LoadMap：当前关卡有没保存的改动时弹「要不要保存」，没人点，游戏线程卡在对话框里，面板看着像断开了
		FAgentPanelBridge& Bridge = FAgentPanelBridge::Get();
		if (!Bridge.HasOp(TEXT("editor.map")))
		{
			// 旧 UELink（2.22 以前）没有 editor.map：照旧直接加载
			return GameTestEditor::LoadLevel(Map, InContext.Message) ? StepSucceeded : StepFailed;
		}
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("action"), TEXT("open"));
		Args->SetStringField(TEXT("map"), Map);
		Args->SetStringField(TEXT("unsaved"), Unsaved == TEXT("discard") ? TEXT("discard") : TEXT("error"));
		TSharedPtr<FJsonObject> Out;
		FString Error;
		if (!Bridge.CallOp(TEXT("editor.map"), Args, Out, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}
		if (!Out.IsValid())
		{
			Out = MakeShared<FJsonObject>();
		}
		FString Package = Map;
		Out->TryGetStringField(TEXT("package"), Package);
		InContext.Payload->SetStringField(TEXT("map"), Package);
		bool bAlready = false;
		if (Out->TryGetBoolField(TEXT("already"), bAlready) && bAlready)
		{
			InContext.Message = FString::Printf(TEXT("编辑器已经是 %s"), *Package);
			return StepSucceeded;
		}
		const TArray<TSharedPtr<FJsonValue>>* Discarded = nullptr;
		if (Out->TryGetArrayField(TEXT("discarded"), Discarded) && Discarded != nullptr)
		{
			InContext.Payload->SetArrayField(TEXT("discarded"), *Discarded);
		}

		// editor.map 下一帧才真正加载（加载本身卡住游戏线程，GameTestClock 把这段算作被打断，不占超时）：之后按 status 的 last 判成败
		InContext.DefaultTimeoutMs = 120000;
		InContext.PendingFailure = MakeShared<FString>();
		const TSharedPtr<FString> Failure = InContext.PendingFailure;
		const TSharedPtr<FJsonObject> Payload = InContext.Payload;
		InContext.Message = FString::Printf(TEXT("正在打开 %s"), *Package);
		InContext.PendingPredicate = [Failure, Payload, Package](double)
		{
			TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
			Status->SetStringField(TEXT("action"), TEXT("status"));
			TSharedPtr<FJsonObject> Now;
			FString StatusError;
			if (!FAgentPanelBridge::Get().CallOp(TEXT("editor.map"), Status, Now, StatusError))
			{
				*Failure = StatusError;
				return true;
			}
			if (!Now.IsValid())
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* Last = nullptr;
			if (!Now->TryGetObjectField(TEXT("last"), Last) || Last == nullptr || !Last->IsValid())
			{
				return false;
			}
			bool bOk = false;
			if (!(*Last)->TryGetBoolField(TEXT("ok"), bOk))
			{
				// 还没加载完
				return false;
			}
			double Ms = 0.0;
			if ((*Last)->TryGetNumberField(TEXT("ms"), Ms))
			{
				Payload->SetNumberField(TEXT("loadMs"), Ms);
			}
			if (!bOk)
			{
				FString Why;
				(*Last)->TryGetStringField(TEXT("error"), Why);
				*Failure = Why.IsEmpty() ? FString::Printf(TEXT("没打开 %s"), *Package) : Why;
			}
			return true;
		};
		return StepPending;
	}
}
