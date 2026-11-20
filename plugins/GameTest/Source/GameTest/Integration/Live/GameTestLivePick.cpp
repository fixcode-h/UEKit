// Copyright Epic Games, Inc. All Rights Reserved.

#include "Integration/Live/GameTestLive.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Ref/GameTestRefFor.h"

#include "Core/GameTestWorld.h"
#include "Input/GameTestInput.h"
#include "Observe/GameTestInspect.h"
#include "Observe/GameTestSlate.h"
#include "Record/GameTestRecorder.h"
#include "AgentPanelBridge.h"

#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"
#include "Widgets/SWidget.h"

/**
 * 从游戏里抓控件（定稿 9.3、效果图 3 / 10 / 11）：
 * - 面板在实时画面上点一下 → `gt.pick action=at`，同步返回点到的控件；
 * - 游戏里按住 Ctrl+Alt 点一下 → 这一下被吃掉（不传给游戏），推 `gt.pick` 事件 `{kind: "picked", via: "game"}`；
 * - 抓取期间按 fps 截视口推 `{kind: "frame", jpg, width, height}`，画面没变不推。
 * 会话是租约：面板每隔一会儿再 start 一次续上，关了页面不续，ttl 到了自己停，不会一直截图。
 */
namespace GameTestLivePickDetail
{
	struct FSession
	{
		bool bActive = false;
		bool bFrames = true;
		double FrameInterval = 0.5;
		int32 MaxWidth = 640;
		int32 Quality = 70;
		double ExpireAt = 0.0;
		double NextFrameAt = 0.0;
		uint32 LastHash = 0;
		/** 刚吃掉了 Ctrl+Alt 的按下：对应的松开也吃掉，免得游戏收到一个没头的松开 */
		bool bSwallowUp = false;
		TSharedPtr<IInputProcessor> Input;
	};

	FSession& Session()
	{
		static FSession Instance;
		return Instance;
	}

	constexpr double DefaultTtlSeconds = 60.0;
	constexpr double MaxTtlSeconds = 600.0;

	/** 屏幕坐标 → 视口归一化坐标；不在视口里返回 false。 */
	bool ToNormalized(const FVector2D& InScreen, FVector2D& OutNormalized)
	{
		const TSharedPtr<SWidget> Viewport = GameTestSlate::GetGameViewportWidget();
		if (!Viewport.IsValid())
		{
			return false;
		}
		const FGeometry& Geometry = Viewport->GetCachedGeometry();
		const FVector2D Size = FVector2D(Geometry.GetLocalSize());
		if (Size.X <= 0.0 || Size.Y <= 0.0)
		{
			return false;
		}
		const FVector2D Local = FVector2D(Geometry.AbsoluteToLocal(InScreen));
		OutNormalized = FVector2D(Local.X / Size.X, Local.Y / Size.Y);
		return OutNormalized.X >= 0.0 && OutNormalized.X <= 1.0 && OutNormalized.Y >= 0.0 && OutNormalized.Y <= 1.0;
	}

	/** 点到的地方没有 UMG 控件（游戏画面）：从玩家视角打一条射线，抓 Actor（效果图 10 抓对象选标识用）。 */
	void AddActorUnder(const FVector2D& InNormalized, FJsonObject& OutResult)
	{
		UWorld* World = GameTestWorld::FindGameWorld();
		if (World == nullptr)
		{
			return;
		}
		APlayerController* Controller = World->GetFirstPlayerController();
		UGameViewportClient* ViewportClient = World->GetGameViewport();
		if (Controller == nullptr || ViewportClient == nullptr)
		{
			return;
		}
		FVector2D ViewportSize = FVector2D::ZeroVector;
		ViewportClient->GetViewportSize(ViewportSize);
		FHitResult Hit;
		if (ViewportSize.X <= 0.0 || !Controller->GetHitResultAtScreenPosition(FVector2D(InNormalized.X * ViewportSize.X, InNormalized.Y * ViewportSize.Y), ECC_Visibility, false, Hit))
		{
			return;
		}
		const AActor* Actor = Hit.GetActor();
		if (Actor == nullptr)
		{
			return;
		}
		const TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		Info->SetStringField(TEXT("name"), Actor->GetName());
#if WITH_EDITOR
		Info->SetStringField(TEXT("label"), Actor->GetActorLabel());
#endif
		if (const UClass* Native = Actor->GetClass())
		{
			const UClass* NativeParent = Native;
			while (NativeParent != nullptr && !NativeParent->HasAnyClassFlags(CLASS_Native))
			{
				NativeParent = NativeParent->GetSuperClass();
			}
			if (NativeParent != nullptr && NativeParent != Native)
			{
				Info->SetStringField(TEXT("nativeClass"), NativeParent->GetName());
			}
		}
		OutResult.SetObjectField(TEXT("actor"), Info);
		// 2.0.0：Actor 也出引用（world 里按类，ident 按 identRules / 打分），chain 只有这一层
		FGameTestFlowContext Flow;
		Flow.World = World;
		FGameTestRefForResult Ref;
		if (GameTestRefFor::ForObject(const_cast<AActor*>(Actor), Flow, Ref) && Ref.Ref.IsValid())
		{
			const TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
			Link->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
			Link->SetStringField(TEXT("kind"), TEXT("actor"));
			Link->SetStringField(TEXT("note"), TEXT("场景里的 Actor"));
			TArray<TSharedPtr<FJsonValue>> Chain;
			Chain.Add(MakeShared<FJsonValueObject>(Link));
			OutResult.SetArrayField(TEXT("chain"), Chain);
			OutResult.SetObjectField(TEXT("ref"), Ref.Ref);
			OutResult.SetNumberField(TEXT("matches"), Ref.Matches);
			OutResult.SetBoolField(TEXT("ask"), Ref.bAsk);
		}
	}

	/** 点一下抓到的东西：控件（和 gt.inspect 一样，偏向按钮 / 勾选框）；没有控件时补上射线打到的 Actor。 */
	bool BuildPick(const FVector2D& InNormalized, const TCHAR* InVia, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		if (!GameTestInspect::Inspect(TOptional<FVector2D>(InNormalized), OutResult, OutError, /*bInPreferInteractive=*/true))
		{
			return false;
		}
		if (!OutResult->HasField(TEXT("ref")))
		{
			AddActorUnder(InNormalized, *OutResult);
		}
		OutResult->SetStringField(TEXT("via"), InVia);
		return true;
	}

	void PublishPicked(const TSharedPtr<FJsonObject>& InPicked)
	{
		const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
		Event->Values = InPicked->Values;
		Event->SetStringField(TEXT("kind"), TEXT("picked"));
		FAgentPanelBridge::Get().PublishEvent(TEXT("gt.pick"), Event);
	}

	/** 游戏里 Ctrl+Alt 点：录制中让给录制器（录制时 Ctrl+Alt 点是录断言）。 */
	class FPickInput : public IInputProcessor
	{
	public:
		virtual void Tick(const float InDeltaTime, FSlateApplication& InSlateApp, TSharedRef<ICursor> InCursor) override
		{
		}

		virtual bool HandleMouseButtonDownEvent(FSlateApplication& InSlateApp, const FPointerEvent& InMouseEvent) override
		{
			FSession& State = Session();
			if (!State.bActive || GameTestInput::IsSendingSynthetic() || FGameTestRecorder::Get().IsRecording())
			{
				return false;
			}
			if (InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton || !InMouseEvent.IsControlDown() || !InMouseEvent.IsAltDown())
			{
				return false;
			}
			FVector2D Normalized;
			if (!ToNormalized(FVector2D(InMouseEvent.GetScreenSpacePosition()), Normalized))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Picked;
			FString Error;
			if (BuildPick(Normalized, TEXT("game"), Picked, Error))
			{
				PublishPicked(Picked);
			}
			else
			{
				const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
				Event->SetStringField(TEXT("kind"), TEXT("picked"));
				Event->SetStringField(TEXT("via"), TEXT("game"));
				Event->SetStringField(TEXT("error"), Error);
				FAgentPanelBridge::Get().PublishEvent(TEXT("gt.pick"), Event);
			}
			State.bSwallowUp = true;
			return true;
		}

		virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& InSlateApp, const FPointerEvent& InMouseEvent) override
		{
			return HandleMouseButtonDownEvent(InSlateApp, InMouseEvent);
		}

		virtual bool HandleMouseButtonUpEvent(FSlateApplication& InSlateApp, const FPointerEvent& InMouseEvent) override
		{
			FSession& State = Session();
			if (State.bSwallowUp && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
			{
				State.bSwallowUp = false;
				return true;
			}
			return false;
		}

		virtual const TCHAR* GetDebugName() const override
		{
			return TEXT("GameTestPick");
		}
	};

	void Stop()
	{
		FSession& State = Session();
		if (State.Input.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(State.Input);
		}
		State = FSession();
	}

	bool FrameNow(TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		FSession& State = Session();
		TSharedPtr<FJsonObject> Frame;
		uint32 Hash = 0;
		if (!GameTestLiveFrame::Capture(State.MaxWidth, State.Quality, 0, Frame, Hash, OutError) || !Frame.IsValid())
		{
			return false;
		}
		State.LastHash = Hash;
		OutResult = Frame;
		return true;
	}
}

bool GameTestLive::Pick(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
{
	using namespace GameTestLivePickDetail;
	FString Action = TEXT("start");
	if (InArgs.IsValid())
	{
		InArgs->TryGetStringField(TEXT("action"), Action);
	}
	OutResult = MakeShared<FJsonObject>();
	FSession& State = Session();

	if (Action == TEXT("stop"))
	{
		Stop();
		OutResult->SetBoolField(TEXT("active"), false);
		return true;
	}
	if (Action == TEXT("at"))
	{
		double X = -1.0;
		double Y = -1.0;
		if (!InArgs.IsValid() || !InArgs->TryGetNumberField(TEXT("x"), X) || !InArgs->TryGetNumberField(TEXT("y"), Y) || X < 0.0 || X > 1.0 || Y < 0.0 || Y > 1.0)
		{
			OutError = TEXT("gt.pick action=at 要 x、y（视口归一化坐标 0–1）");
			return false;
		}
		return BuildPick(FVector2D(X, Y), TEXT("panel"), OutResult, OutError);
	}
	if (Action == TEXT("frame"))
	{
		return FrameNow(OutResult, OutError);
	}
	if (Action != TEXT("start"))
	{
		OutError = FString::Printf(TEXT("gt.pick 不认识 action=%s：可用 start / stop / at / frame"), *Action);
		return false;
	}

	if (!FSlateApplication::IsInitialized() || !GameTestSlate::GetGameViewportWidget().IsValid())
	{
		OutError = TEXT("没有游戏视口：先启动 PIE 或游戏，再从游戏里抓");
		return false;
	}

	double Fps = 2.0;
	double TtlMs = DefaultTtlSeconds * 1000.0;
	bool bFrames = true;
	int32 MaxWidth = 640;
	int32 Quality = 70;
	InArgs->TryGetBoolField(TEXT("frames"), bFrames);
	InArgs->TryGetNumberField(TEXT("fps"), Fps);
	InArgs->TryGetNumberField(TEXT("ttlMs"), TtlMs);
	InArgs->TryGetNumberField(TEXT("maxWidth"), MaxWidth);
	InArgs->TryGetNumberField(TEXT("quality"), Quality);

	const double Now = FPlatformTime::Seconds();
	State.bFrames = bFrames;
	State.FrameInterval = 1.0 / FMath::Clamp(Fps, 0.5, 10.0);
	State.MaxWidth = FMath::Clamp(MaxWidth, 160, 1920);
	State.Quality = FMath::Clamp(Quality, 30, 95);
	State.ExpireAt = Now + FMath::Clamp(TtlMs / 1000.0, 5.0, MaxTtlSeconds);
	if (!State.bActive)
	{
		State.bActive = true;
		State.LastHash = 0;
		State.NextFrameAt = Now;
		State.Input = MakeShared<FPickInput>();
		FSlateApplication::Get().RegisterInputPreProcessor(State.Input);
	}
	EnsureTicker();

	OutResult->SetBoolField(TEXT("active"), true);
	OutResult->SetBoolField(TEXT("recording"), FGameTestRecorder::Get().IsRecording());
	// 续租时不带画面；第一次 start 带一帧，面板打开就有图，不用等下一次推送
	bool bWithFrame = false;
	InArgs->TryGetBoolField(TEXT("withFrame"), bWithFrame);
	if (bWithFrame && State.bFrames)
	{
		TSharedPtr<FJsonObject> Frame;
		FString FrameError;
		if (FrameNow(Frame, FrameError))
		{
			OutResult->SetObjectField(TEXT("frame"), Frame);
		}
		else
		{
			OutResult->SetStringField(TEXT("frameError"), FrameError);
		}
	}
	return true;
}

bool GameTestLive::PickTick(double InNow)
{
	using namespace GameTestLivePickDetail;
	FSession& State = Session();
	if (!State.bActive)
	{
		return false;
	}
	if (InNow >= State.ExpireAt)
	{
		Stop();
		return false;
	}
	if (!State.bFrames || InNow < State.NextFrameAt)
	{
		return true;
	}
	State.NextFrameAt = InNow + State.FrameInterval;
	TSharedPtr<FJsonObject> Frame;
	uint32 Hash = 0;
	FString Error;
	if (!GameTestLiveFrame::Capture(State.MaxWidth, State.Quality, State.LastHash, Frame, Hash, Error))
	{
		// 截不到（PIE 停了、窗口最小化）：不推错误刷屏，下一轮再试；PIE 停了面板会从连接状态看到
		return true;
	}
	State.LastHash = Hash;
	if (Frame.IsValid())
	{
		const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
		Event->Values = Frame->Values;
		Event->SetStringField(TEXT("kind"), TEXT("frame"));
		FAgentPanelBridge::Get().PublishEvent(TEXT("gt.pick"), Event);
	}
	return true;
}

void GameTestLive::PickShutdown()
{
	GameTestLivePickDetail::Stop();
}
