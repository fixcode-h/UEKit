// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestHandlerShared.h"

#include "Core/GameTestJson.h"
#include "Core/GameTestWorld.h"
#include "Input/GameTestInput.h"
#include "Observe/GameTestActor.h"
#include "Observe/GameTestWidgetNode.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Components/Widget.h"
#include "Widgets/SWidget.h"

namespace GameTestHandlers
{
	EGameTestStepStatus HandleInputCursor(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		double X = 0.5;
		double Y = 0.5;
		if (!GameTestJson::TryGetNumber(Params, TEXT("x"), X) || !GameTestJson::TryGetNumber(Params, TEXT("y"), Y))
		{
			InContext.Message = TEXT("input.cursor 需要 x 与 y（视口内归一化坐标，0–1）");
			return StepFailed;
		}

		FString Error;
		UWorld* World = ResolveStepWorld(InContext, Error);
		if (World == nullptr)
		{
			InContext.Message = Error;
			return StepFailed;
		}

		FVector2D ScreenPosition = FVector2D::ZeroVector;
		if (!GameTestInput::MoveCursor(*World, FVector2D(X, Y), ScreenPosition, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		InContext.Payload->SetNumberField(TEXT("x"), X);
		InContext.Payload->SetNumberField(TEXT("y"), Y);
		InContext.Payload->SetNumberField(TEXT("screenX"), ScreenPosition.X);
		InContext.Payload->SetNumberField(TEXT("screenY"), ScreenPosition.Y);
		InContext.Message = Error;
		return StepSucceeded;
	}

	EGameTestStepStatus HandleKey(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		FString KeyName;
		if (!GameTestJson::TryGetString(Params, TEXT("key"), KeyName) || KeyName.IsEmpty())
		{
			InContext.Message = TEXT("key 需要 key（UE 的键名，例如 W、SpaceBar、LeftMouseButton）");
			return StepFailed;
		}

		FString Phase = TEXT("tap");
		GameTestJson::TryGetString(Params, TEXT("phase"), Phase);
		const bool bTap = Phase.Equals(TEXT("tap"), ESearchCase::IgnoreCase);
		const bool bPress = Phase.Equals(TEXT("press"), ESearchCase::IgnoreCase);
		const bool bRelease = Phase.Equals(TEXT("release"), ESearchCase::IgnoreCase);
		if (!bTap && !bPress && !bRelease)
		{
			InContext.Message = FString::Printf(TEXT("phase 只能是 tap / press / release，收到 '%s'"), *Phase);
			return StepFailed;
		}

		// 给了 target：键盘事件发给这个控件（输入框里的回车、退格、方向键），否则发给游戏视口。
		TSharedPtr<SWidget> Focus;
		if (!InContext.Step->Target.IsEmpty())
		{
			TSharedPtr<FGameTestWidgetNode> Node;
			FString ResolveError;
			if (!TryResolveStepTarget(InContext, Node, ResolveError))
			{
				InContext.Message = ResolveError;
				return StepFailed;
			}
			Focus = Node->Slate.Pin();
			if (!Focus.IsValid() && Node->Widget.IsValid())
			{
				Focus = Node->Widget->GetCachedWidget();
			}
			if (!Focus.IsValid())
			{
				InContext.Message = FString::Printf(TEXT("%s 还没建出 Slate 控件，按键没处发"), *Node->Path);
				return StepFailed;
			}
		}

		const FKey Key(*KeyName);
		FString Error;
		if (!GameTestInput::SendKeyTo(Key, /*bInDown=*/!bRelease, Focus, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		InContext.Payload->SetStringField(TEXT("key"), Key.GetFName().ToString());
		InContext.Payload->SetStringField(TEXT("phase"), Phase.ToLower());

		if (!bTap)
		{
			InContext.Message = Error;
			return StepSucceeded;
		}

		// tap：按下后隔一帧再松开——同一帧里按下又松开，有的 trigger（Pressed / Tap）看不到这次按键。
		// 给了 holdMs（录制的长按）就按住这么久再松开：长按和单击在游戏里常常是两个操作（Hold / Tap trigger）。
		int32 HoldMs = 0;
		GameTestJson::TryGetInt(Params, TEXT("holdMs"), HoldMs);
		HoldMs = FMath::Max(HoldMs, 0);
		const FKey HeldKey = Key;
		const uint64 PressedFrame = GFrameCounter;
		const double ReleaseAt = FPlatformTime::Seconds() + HoldMs / 1000.0;
		const TWeakPtr<SWidget> FocusWeak = Focus;
		InContext.Payload->SetNumberField(TEXT("holdMs"), HoldMs);
		InContext.Message = HoldMs > 0
			? FString::Printf(TEXT("已按住 %s %d 毫秒后松开"), *Key.GetFName().ToString(), HoldMs)
			: FString::Printf(TEXT("已按下并松开 %s"), *Key.GetFName().ToString());
		// 按住的时间可能比单步默认超时还长：超时至少留出按住的时间再加 5 秒。
		InContext.DefaultTimeoutMs = HoldMs + 5000;
		// 超时或取消时执行器不再调谓词：由收尾把键松开，后面的步骤不会在「按住」下跑。
		const TSharedRef<bool> bReleased = MakeShared<bool>(false);
		InContext.PendingCleanup = [HeldKey, FocusWeak, bReleased]()
		{
			if (!*bReleased)
			{
				FString Ignored;
				GameTestInput::SendKeyTo(HeldKey, /*bInDown=*/false, FocusWeak.Pin(), Ignored);
				*bReleased = true;
			}
		};
		InContext.PendingPredicate = [HeldKey, PressedFrame, ReleaseAt, FocusWeak, bReleased](double)
		{
			if (GFrameCounter <= PressedFrame + 1 || FPlatformTime::Seconds() < ReleaseAt)
			{
				return false;
			}
			FString Ignored;
			GameTestInput::SendKeyTo(HeldKey, /*bInDown=*/false, FocusWeak.Pin(), Ignored);
			*bReleased = true;
			return true;
		};
		return StepPending;
	}

	EGameTestStepStatus HandleActorGet(FGameTestStepContext& InContext)
	{
		FString ClassName;
		FString Tag;
		GameTestJson::TryGetString(*InContext.Step->Params, TEXT("class"), ClassName);
		GameTestJson::TryGetString(*InContext.Step->Params, TEXT("tag"), Tag);

		// locallyControlled：多客户端会话里把"本端玩家那台"与"远端那台"分开的稳定判据。
		TOptional<bool> LocallyControlled;
		bool bLocallyControlled = false;
		if (GameTestJson::TryGetBool(*InContext.Step->Params, TEXT("locallyControlled"), bLocallyControlled))
		{
			LocallyControlled = bLocallyControlled;
		}

		FString Error;
		UWorld* World = ResolveStepWorld(InContext, Error);
		if (World == nullptr)
		{
			InContext.Message = Error;
			return StepFailed;
		}

		AActor* Actor = GameTestActor::ResolveOne(*World, ClassName, Tag, LocallyControlled, Error);
		if (Actor == nullptr)
		{
			InContext.Message = Error;
			return StepFailed;
		}

		const TSharedPtr<FJsonObject> ActorJson = GameTestActor::ToJson(*Actor);
		InContext.Payload->SetObjectField(TEXT("actor"), ActorJson);
		// 关键字段也摊到顶层：AI 读位置时不必先钻进 actor 对象。
		InContext.Payload->SetStringField(TEXT("path"), Actor->GetPathName());
		InContext.Payload->SetStringField(TEXT("world"), GameTestWorld::DescribeWorld(World));
		if (const TSharedPtr<FJsonValue> Location = ActorJson->TryGetField(TEXT("location")))
		{
			InContext.Payload->SetField(TEXT("location"), Location);
		}
		if (const TSharedPtr<FJsonValue> Velocity = ActorJson->TryGetField(TEXT("velocity")))
		{
			InContext.Payload->SetField(TEXT("velocity"), Velocity);
		}

		InContext.Message = FString::Printf(TEXT("[%s] 找到 %s，位置 %s，速度 %.1f"),
			*GameTestWorld::DescribeWorld(World), *Actor->GetClass()->GetName(),
			*Actor->GetActorLocation().ToCompactString(), Actor->GetVelocity().Size());
		return StepSucceeded;
	}
}
