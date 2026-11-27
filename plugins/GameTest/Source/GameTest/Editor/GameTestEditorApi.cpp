// Copyright Epic Games, Inc. All Rights Reserved.

#include "Editor/GameTestEditorApi.h"

#include "Core/GameTestJson.h"

#include "Dom/JsonObject.h"

#if WITH_EDITOR
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Widgets/SWindow.h"
#endif

namespace
{
#if WITH_EDITOR
	FString GetModalWindowTitle()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return FString();
		}

		const TSharedPtr<SWindow> ModalWindow = FSlateApplication::Get().GetActiveModalWindow();
		return ModalWindow.IsValid() ? ModalWindow->GetTitle().ToString() : FString();
	}

	bool AreAssetsLoading()
	{
		const IAssetRegistry* Registry = IAssetRegistry::Get();
		return Registry != nullptr && Registry->IsLoadingAssets();
	}

	FString GetLevelPath(const UWorld* InWorld)
	{
		return InWorld != nullptr ? InWorld->GetOutermost()->GetName() : FString();
	}
#endif
}

bool GameTestEditor::IsAvailable()
{
#if WITH_EDITOR
	return true;
#else
	return false;
#endif
}

FString GameTestEditor::GetEditorLevelPath()
{
#if WITH_EDITOR
	return GEditor != nullptr ? GetLevelPath(GEditor->GetEditorWorldContext().World()) : FString();
#else
	return FString();
#endif
}

TSharedPtr<FJsonObject> GameTestEditor::BuildStatusJson()
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("process"), GIsEditor ? TEXT("editor") : TEXT("game"));

#if WITH_EDITOR
	Json->SetBoolField(TEXT("editorAvailable"), true);
	Json->SetBoolField(TEXT("slateReady"), FSlateApplication::IsInitialized());
	Json->SetBoolField(TEXT("assetsLoading"), AreAssetsLoading());

	const FString ModalTitle = GetModalWindowTitle();
	Json->SetBoolField(TEXT("modal"), !ModalTitle.IsEmpty());
	if (!ModalTitle.IsEmpty())
	{
		// 模态框是这类自动化的经典死法：把它挂在状态里，AI 才能区分"卡在弹窗"与"真死锁"。
		Json->SetStringField(TEXT("modalWindow"), ModalTitle);
	}

	const bool bPlaying = GEditor != nullptr && GEditor->IsPlayingSessionInEditor();
	const bool bQueued = GEditor != nullptr && GEditor->IsPlaySessionRequestQueued();

	Json->SetBoolField(TEXT("pieRunning"), bPlaying);
	Json->SetBoolField(TEXT("pieQueued"), bQueued);
	Json->SetStringField(TEXT("pieState"), bPlaying ? TEXT("running") : (bQueued ? TEXT("queued") : TEXT("stopped")));

	if (GEditor != nullptr && GEditor->PlayWorld != nullptr)
	{
		Json->SetBoolField(TEXT("piePaused"), GEditor->PlayWorld->IsPaused());
		Json->SetStringField(TEXT("pieMap"), GetLevelPath(GEditor->PlayWorld));
	}

	Json->SetStringField(TEXT("level"), GetLevelPath(GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr));
#else
	Json->SetBoolField(TEXT("editorAvailable"), false);
#endif

	return Json;
}

bool GameTestEditor::CheckReady(bool bInRequireAssetsLoaded, bool bInRequireNoModal, FString& OutMessage)
{
#if WITH_EDITOR
	if (GEditor == nullptr)
	{
		OutMessage = TEXT("GEditor 还不存在（编辑器还没起来）");
		return false;
	}

	if (!FSlateApplication::IsInitialized())
	{
		OutMessage = TEXT("Slate 还没初始化");
		return false;
	}

	if (bInRequireAssetsLoaded && AreAssetsLoading())
	{
		OutMessage = TEXT("资产注册表还在加载中");
		return false;
	}

	const FString ModalTitle = GetModalWindowTitle();
	if (bInRequireNoModal && !ModalTitle.IsEmpty())
	{
		OutMessage = FString::Printf(TEXT("编辑器上有一个模态框挡着：'%s'"), *ModalTitle);
		return false;
	}

	OutMessage = TEXT("编辑器就绪");
	return true;
#else
	OutMessage = TEXT("当前进程没有编辑器（独立进程或打包构建）；editor.ensure 不适用");
	return false;
#endif
}

bool GameTestEditor::IsPieRunning()
{
#if WITH_EDITOR
	return GEditor != nullptr && GEditor->IsPlayingSessionInEditor();
#else
	return false;
#endif
}

#if WITH_EDITOR
namespace GameTestEditorDetail
{
	/**
	 * PIE 开进选中的视口：和 Play 菜单勾「选中的视口」一样，把关卡编辑器当前活动的关卡视口给 DestinationSlateViewport。
	 * UELink 2.24 的 pie 操作也是这几行（UELinkPieViewport.cpp），那边是 UELinkEditor 的私有函数、GameTest 只依赖 UELinkCore，
	 * 为这几行让 GameTest 多依赖一个编辑器模块不值。找不到看得见的视口时返回 false，调用方退回独立窗口。
	 */
	bool PieIntoSelectedViewport(FRequestPlaySessionParams& InOutParams)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return false;
		}
		FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
		if (LevelEditor == nullptr)
		{
			return false;
		}
		const TSharedPtr<IAssetViewport> Viewport = LevelEditor->GetFirstActiveViewport();
		if (!Viewport.IsValid() || !FSlateApplication::Get().FindWidgetWindow(Viewport->AsWidget()).IsValid())
		{
			return false;
		}
		InOutParams.DestinationSlateViewport = TWeakPtr<IAssetViewport>(Viewport);
		return true;
	}
}
#endif

bool GameTestEditor::StartPIE(const FGameTestPieRequest& InRequest, UObject*& OutKeepAlive, FString& OutMessage)
{
	OutKeepAlive = nullptr;

#if WITH_EDITOR
	if (GEditor == nullptr)
	{
		OutMessage = TEXT("GEditor 不存在，无法启动 PIE");
		return false;
	}

	if (GEditor->IsPlayingSessionInEditor() || GEditor->IsPlaySessionRequestQueued())
	{
		OutMessage = TEXT("PIE 已经在跑（或启动请求已入队）；先 pie.stop 再启动");
		return false;
	}

	const bool bListenServer = InRequest.NetMode.Equals(TEXT("listenServer"), ESearchCase::IgnoreCase);
	if (!InRequest.NetMode.IsEmpty() && !bListenServer
		&& !InRequest.NetMode.Equals(TEXT("standalone"), ESearchCase::IgnoreCase))
	{
		OutMessage = FString::Printf(TEXT("netMode 只能是 standalone 或 listenServer，收到 '%s'"), *InRequest.NetMode);
		return false;
	}

	if (!bListenServer && InRequest.Players > 1)
	{
		OutMessage = TEXT("standalone 没有第二个玩家窗口；多玩家请用 netMode=listenServer");
		return false;
	}

	if (InRequest.Players < 1)
	{
		OutMessage = TEXT("players 至少为 1");
		return false;
	}

	const bool bWantViewport = InRequest.Window.IsEmpty() || InRequest.Window.Equals(TEXT("viewport"), ESearchCase::IgnoreCase);
	if (!bWantViewport && !InRequest.Window.Equals(TEXT("new"), ESearchCase::IgnoreCase))
	{
		OutMessage = FString::Printf(TEXT("window 只能是 viewport（选中的视口，默认）或 new（独立窗口），收到 '%s'"), *InRequest.Window);
		return false;
	}

	// 不去改 CDO：那会把本次的窗口尺寸写进编辑器配置。复制一份临时的，只影响这一次会话。
	ULevelEditorPlaySettings* PlaySettings = DuplicateObject<ULevelEditorPlaySettings>(
		GetMutableDefault<ULevelEditorPlaySettings>(), GetTransientPackage());

	if (bListenServer)
	{
		// 引擎语义：编辑器自己充当服务器 + 第一个玩家（主机），PlayNumberOfClients 是玩家窗口总数。
		PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_ListenServer);
		PlaySettings->SetPlayNumberOfClients(InRequest.Players);
	}
	else
	{
		PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
	}

	PlaySettings->CenterNewWindow = true;
	if (InRequest.Width > 0)
	{
		PlaySettings->NewWindowWidth = InRequest.Width;
	}
	if (InRequest.Height > 0)
	{
		PlaySettings->NewWindowHeight = InRequest.Height;
	}

	FRequestPlaySessionParams Params;
	Params.SessionDestination = EPlaySessionDestinationType::InProcess;
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;
	Params.EditorPlaySettings = PlaySettings;
	// 自动化不该被登录/在线子系统打断。
	Params.bAllowOnlineSubsystem = false;
	if (!InRequest.Map.IsEmpty())
	{
		Params.GlobalMapOverride = InRequest.Map;
	}

	const bool bInViewport = bWantViewport && GameTestEditorDetail::PieIntoSelectedViewport(Params);

	OutKeepAlive = PlaySettings;
	GEditor->RequestPlaySession(Params);

	const FString Where = bInViewport ? FString(TEXT("选中的视口"))
		: FString::Printf(TEXT("%s独立窗口 %dx%d"), bWantViewport ? TEXT("没找到看得见的关卡视口，开在") : TEXT(""), PlaySettings->NewWindowWidth, PlaySettings->NewWindowHeight);
	OutMessage = FString::Printf(TEXT("已请求启动 PIE（%s%s%s）"),
		*Where,
		bListenServer ? *FString::Printf(TEXT("，监听服务器 %d 名玩家"), InRequest.Players) : TEXT(""),
		InRequest.Map.IsEmpty() ? TEXT("，用当前关卡") : *FString::Printf(TEXT("，地图 %s"), *InRequest.Map));
	return true;
#else
	OutMessage = TEXT("当前进程没有编辑器，不能启动 PIE；独立/打包进程请直接跑游戏本体，用 wait until=pieReady 等世界起来");
	return false;
#endif
}

bool GameTestEditor::StopPIE(FString& OutMessage)
{
#if WITH_EDITOR
	if (GEditor == nullptr)
	{
		OutMessage = TEXT("GEditor 不存在，无法停止 PIE");
		return false;
	}

	if (!GEditor->IsPlayingSessionInEditor() && !GEditor->IsPlaySessionRequestQueued())
	{
		OutMessage = TEXT("PIE 本来就没在跑");
		return false;
	}

	GEditor->RequestEndPlayMap();
	OutMessage = TEXT("已请求退出 PIE（请求入队；用 wait until=pieStopped 等它真的结束）");
	return true;
#else
	OutMessage = TEXT("当前进程没有编辑器，没有 PIE 可停");
	return false;
#endif
}

bool GameTestEditor::SetBackgroundThrottleEnabled(bool bInEnabled, bool& bOutPreviousValue)
{
#if WITH_EDITOR
	UEditorPerformanceSettings* Settings = GetMutableDefault<UEditorPerformanceSettings>();
	if (Settings == nullptr)
	{
		return false;
	}

	bOutPreviousValue = Settings->bThrottleCPUWhenNotForeground;
	Settings->bThrottleCPUWhenNotForeground = bInEnabled;
	return true;
#else
	(void)bInEnabled;
	(void)bOutPreviousValue;
	return false;
#endif
}

bool GameTestEditor::LoadLevel(const FString& InMapPath, FString& OutMessage)
{
#if WITH_EDITOR
	if (InMapPath.IsEmpty())
	{
		OutMessage = TEXT("需要 map 字段（关卡资产路径，例如 /Game/Map/TestMap）");
		return false;
	}

	if (GEditor == nullptr)
	{
		OutMessage = TEXT("GEditor 不存在，无法加载关卡");
		return false;
	}

	if (GEditor->IsPlayingSessionInEditor())
	{
		OutMessage = TEXT("PIE 正在跑，先 pie.stop 再换关卡");
		return false;
	}

	UWorld* LoadedWorld = UEditorLoadingAndSavingUtils::LoadMap(InMapPath);
	if (LoadedWorld == nullptr)
	{
		OutMessage = FString::Printf(TEXT("加载关卡失败：%s（当前关卡：%s）"), *InMapPath, *GetEditorLevelPath());
		return false;
	}

	OutMessage = FString::Printf(TEXT("已加载关卡 %s"), *InMapPath);
	return true;
#else
	OutMessage = TEXT("当前进程没有编辑器，level.load 不适用");
	return false;
#endif
}
