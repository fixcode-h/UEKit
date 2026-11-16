// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestHandlerShared.h"

#include "Core/GameTestJson.h"
#include "Input/GameTestHandlerWatch.h"
#include "Input/GameTestInput.h"
#include "Observe/GameTestHintPick.h"
#include "Observe/GameTestHitTest.h"
#include "Observe/GameTestSlate.h"
#include "Observe/GameTestWidgetNode.h"
#include "GameTestSettings.h"

#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "InputCoreTypes.h"
#include "Widgets/SWidget.h"

/**
 * 真实指针的 UI 动作：click / drag / wheel / type（按键方式）。
 *
 * 和「代码级点击」（`via: "code"`，按控件类走 OnClicked / 状态回调）不同，这里**不认控件类**：
 * 先等控件可操作（出现、可见、可用、位置稳定、点击点没被挡住），再把光标移过去，发真实的鼠标按下 / 松开，
 * 由控件自己处理——按钮、复选框、BP 或 Lua 里重写了 OnMouseButtonDown 的 WBP、UImage 绑的事件都走同一条路。
 * 点完再核对「接住按下的是不是目标或它里面的控件」，点到别处就判失败，不算通过。
 */
namespace GameTestPointerDetail
{
	/** 自动等待的状态：连续两帧位置不变才算「稳定」（入场动画、滑入结束）。 */
	struct FWaitState
	{
		FVector2D LastMin = FVector2D(-1.0, -1.0);
		FVector2D LastMax = FVector2D(-1.0, -1.0);
		uint64 LastFrame = 0;
		int32 StableFrames = 0;
	};

	/** 一次可操作检查的结果。 */
	struct FActionable
	{
		TSharedPtr<SWidget> Slate;
		FString Path;
		FVector2D Screen = FVector2D::ZeroVector;
		/** 选择器命中多个、按录制线索挑出来的时候写怎么挑的（进步骤结果的 `picked`）。 */
		FString PickNote;
	};

	int32 UiTimeoutMs()
	{
		return GetDefault<UGameTestSettings>()->UiWaitTimeoutMs;
	}

	TSharedPtr<SWidget> SlateOf(const FGameTestWidgetNode& InNode)
	{
		if (const TSharedPtr<SWidget> Slate = InNode.Slate.Pin())
		{
			return Slate;
		}
		const UWidget* Widget = InNode.Widget.Get();
		return Widget != nullptr ? Widget->GetCachedWidget() : nullptr;
	}

	/** 读 `[x, y]`（0–1）；没写时用缺省值。 */
	bool ReadPoint(const FJsonObject& InParams, const TCHAR* InField, FVector2D& InOutPoint, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!InParams.HasField(InField))
		{
			return true;
		}
		if (!InParams.TryGetArrayField(InField, Values) || Values == nullptr || Values->Num() != 2)
		{
			OutError = FString::Printf(TEXT("%s 要写成 [x, y]（控件内归一化坐标，0–1）"), InField);
			return false;
		}
		InOutPoint.X = FMath::Clamp((*Values)[0]->AsNumber(), 0.0, 1.0);
		InOutPoint.Y = FMath::Clamp((*Values)[1]->AsNumber(), 0.0, 1.0);
		return true;
	}

	bool ReadButton(const FJsonObject& InParams, FKey& OutButton, FString& OutError)
	{
		FString ButtonName = TEXT("LeftMouseButton");
		GameTestJson::TryGetString(InParams, TEXT("button"), ButtonName);
		OutButton = FKey(*ButtonName);
		if (!OutButton.IsValid() || !OutButton.IsMouseButton())
		{
			OutError = FString::Printf(TEXT("button 要写鼠标键名（LeftMouseButton / RightMouseButton / MiddleMouseButton），收到 '%s'"), *ButtonName);
			return false;
		}
		return true;
	}

	/**
	 * 一帧的可操作检查：解析 → 可见 → 尺寸 → 可用 → 稳定 →（需要时）点击点没被挡住。
	 * 不满足时 `OutWhy` 写还差什么（超时消息里拼出去，「被谁挡住」「哪一层是 Collapsed」比一句超时有用）。
	 */
	bool CheckActionable(
		const FString& InTarget,
		TMap<FString, TWeakObjectPtr<UWidget>>* InHandles,
		const FJsonObject* InHint,
		const FVector2D& InAt,
		bool bInNeedHit,
		FWaitState& InOutState,
		FActionable& OutTarget,
		FString& OutWhy)
	{
		TSharedPtr<FGameTestWidgetNode> Node;
		FString PickNote;
		if (!GameTestHandlers::TryResolveTarget(InTarget, InHandles, Node, OutWhy, InHint, &PickNote))
		{
			InOutState.StableFrames = 0;
			return false;
		}

		const TSharedPtr<SWidget> Slate = SlateOf(*Node);
		if (!Slate.IsValid())
		{
			OutWhy = FString::Printf(TEXT("%s 还没建出 Slate 控件"), *Node->Path);
			return false;
		}

		for (TSharedPtr<SWidget> Current = Slate; Current.IsValid(); Current = Current->GetParentWidget())
		{
			if (!Current->GetVisibility().IsVisible())
			{
				OutWhy = FString::Printf(TEXT("找到了 %s，但不可见（%s 是 %s）"),
					*Node->Path, *GameTestSlate::Describe(Current), *Current->GetVisibility().ToString());
				InOutState.StableFrames = 0;
				return false;
			}
			if (!Current->IsEnabled())
			{
				OutWhy = FString::Printf(TEXT("找到了 %s，但禁用中（%s）"), *Node->Path, *GameTestSlate::Describe(Current));
				InOutState.StableFrames = 0;
				return false;
			}
		}

		const FGeometry& Geometry = Slate->GetCachedGeometry();
		const FVector2D LocalSize = FVector2D(Geometry.GetLocalSize());
		if (LocalSize.X < 1.0 || LocalSize.Y < 1.0)
		{
			OutWhy = FString::Printf(TEXT("找到了 %s，但尺寸为 0（还没布局）"), *Node->Path);
			InOutState.StableFrames = 0;
			return false;
		}

		const FVector2D Min = FVector2D(Geometry.GetAbsolutePosition());
		const FVector2D Max = Min + FVector2D(Geometry.GetAbsoluteSize());
		if (GFrameCounter != InOutState.LastFrame)
		{
			const bool bSame = Min.Equals(InOutState.LastMin, 0.5) && Max.Equals(InOutState.LastMax, 0.5);
			InOutState.StableFrames = bSame ? InOutState.StableFrames + 1 : 0;
			InOutState.LastMin = Min;
			InOutState.LastMax = Max;
			InOutState.LastFrame = GFrameCounter;
		}
		if (InOutState.StableFrames < 1)
		{
			OutWhy = FString::Printf(TEXT("找到了 %s，还在动（位置没稳定）"), *Node->Path);
			return false;
		}

		if (bInNeedHit)
		{
			GameTestHitTest::FResult Hit;
			FString HitError;
			if (!GameTestHitTest::ProbeSlate(Slate.ToSharedRef(), InAt, Hit, HitError))
			{
				OutWhy = FString::Printf(TEXT("找到了 %s，但点不到：%s"), *Node->Path, *HitError);
				return false;
			}
			if (!Hit.bTargetHit)
			{
				OutWhy = FString::Printf(TEXT("找到了 %s，但点击点被 %s 挡住"), *Node->Path, *Hit.TopDescription);
				return false;
			}
		}

		OutTarget.Slate = Slate;
		OutTarget.Path = Node->Path;
		OutTarget.PickNote = PickNote;
		OutTarget.Screen = FVector2D(Geometry.LocalToAbsolute(LocalSize * InAt));
		return true;
	}

	/**
	 * 接住按下的控件和目标有没有关系：是目标本身或它里面的控件，或者是目标外面、游戏视口里面的控件（点在按钮里的文字上，
	 * 接住的是按钮；点列表条目，接住的是条目外面的表格行）。游戏视口及以上不算：那说明目标没接，点击落到了游戏画面上。
	 * 不要求外面那个控件是 UMG 建的：列表行这类包装控件不一定挂着 UMG 的元数据。
	 */
	bool IsRelatedHandler(const TSharedPtr<SWidget>& InHandler, const TSharedPtr<SWidget>& InTarget)
	{
		if (!InHandler.IsValid() || !InTarget.IsValid())
		{
			return false;
		}
		if (GameTestSlate::IsSelfOrAncestor(InTarget.Get(), InHandler))
		{
			return true;
		}
		const TSharedPtr<SWidget> Viewport = GameTestSlate::GetGameViewportWidget();
		return GameTestSlate::IsSelfOrAncestor(InHandler.Get(), InTarget)
			&& InHandler != Viewport
			&& GameTestSlate::IsSelfOrAncestor(Viewport.Get(), InHandler);
	}

	/** 点击 / 拖动共用的按键核对：这次按下有没有人接、接的是不是目标。 */
	bool CheckDownHandler(
		uint64 InSerialBefore,
		const TSharedPtr<SWidget>& InTarget,
		const FString& InPath,
		FJsonObject& OutPayload,
		FString& OutFailure)
	{
		if (!GameTestHandlerWatch::IsSupported())
		{
			OutPayload.SetStringField(TEXT("handler"), TEXT("（这个进程没编 Slate 调试，核对不了）"));
			return true;
		}

		const GameTestHandlerWatch::FHandled Down = GameTestHandlerWatch::GetLast(GameTestHandlerWatch::EKind::MouseDown);
		if (Down.Serial == InSerialBefore)
		{
			// 没人接住按下：有的控件只在松开时响应，不算失败，如实写进结果。
			OutPayload.SetBoolField(TEXT("handled"), false);
			return true;
		}

		const TSharedPtr<SWidget> Handler = Down.Handler.Pin();
		OutPayload.SetBoolField(TEXT("handled"), true);
		OutPayload.SetStringField(TEXT("handler"), GameTestSlate::Describe(Handler));
		if (!IsRelatedHandler(Handler, InTarget))
		{
			OutFailure = FString::Printf(TEXT("点了 %s，但接住按下的是 %s（目标不响应鼠标，或被它抢走了）"),
				*InPath, *GameTestSlate::Describe(Handler));
			return false;
		}
		return true;
	}

	/** 读步骤通用的 at（控件内的点，默认中心）。 */
	FVector2D ReadAt(const FJsonObject& InParams, FString& OutError, bool& bOutOk)
	{
		FVector2D At(0.5, 0.5);
		bOutOk = ReadPoint(InParams, TEXT("at"), At, OutError);
		return At;
	}
}

namespace GameTestPointerDetail
{

	EGameTestStepStatus RunClickPointer(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		bool bOk = true;
		const FVector2D At = ReadAt(Params, InContext.Message, bOk);
		FKey Button;
		if (!bOk || !ReadButton(Params, Button, InContext.Message))
		{
			return GameTestHandlers::StepFailed;
		}

		// 先解析一次：解析得到就记下来让视口高亮框出它；解析不到不算失败，进入等待。
		TSharedPtr<FGameTestWidgetNode> Initial;
		FString Ignored;
		GameTestHandlers::TryResolveStepTarget(InContext, Initial, Ignored);

		enum class EStage : uint8 { Wait, Down, Up };
		struct FState
		{
			EStage Stage = EStage::Wait;
			FWaitState Wait;
			FActionable Target;
			uint64 MarkFrame = 0;
			uint64 SerialBefore = 0;
			bool bDownOk = true;
			FString DownFailure;
			/** 鼠标键按下了还没松开：步骤中途超时或取消时由收尾松开。 */
			bool bHeld = false;
		};
		const TSharedRef<FState> State = MakeShared<FState>();
		const FString Target = InContext.Step->Target;
		const TSharedPtr<FJsonObject> StepParams = InContext.Step->Params;
		TMap<FString, TWeakObjectPtr<UWidget>>* Handles = InContext.Handles;
		const TSharedPtr<FJsonObject> Payload = InContext.Payload;
		InContext.PendingProgress = MakeShared<FString>();
		InContext.PendingFailure = MakeShared<FString>();
		const TSharedPtr<FString> Progress = InContext.PendingProgress;
		const TSharedPtr<FString> Failure = InContext.PendingFailure;

		InContext.DefaultTimeoutMs = UiTimeoutMs();
		InContext.Message = FString::Printf(TEXT("点击 %s"), *Target);
		// 按下后超时或被取消：执行器不再调谓词，这里把键松开，后面的步骤不会在「按住」下跑。
		InContext.PendingCleanup = [State, Button]()
		{
			if (State->bHeld)
			{
				FString Ignored;
				GameTestInput::SendKey(Button, /*bInDown=*/false, Ignored);
				State->bHeld = false;
			}
		};
		InContext.PendingPredicate = [State, Target, StepParams, Handles, Payload, Progress, Failure, At, Button](double)
		{
			FString Message;
			switch (State->Stage)
			{
			case EStage::Wait:
				if (!CheckActionable(Target, Handles, GameTestHintPick::GetHint(StepParams), At, /*bInNeedHit=*/true, State->Wait, State->Target, *Progress))
				{
					return false;
				}
				GameTestInput::MovePointerTo(State->Target.Screen, Message);
				State->Stage = EStage::Down;
				State->MarkFrame = GFrameCounter;
				return false;

			case EStage::Down:
				// 移过去之后隔一帧再按：让悬停先生效（按钮松开时要还悬停在上面才算点击）。
				if (GFrameCounter <= State->MarkFrame)
				{
					return false;
				}
				State->SerialBefore = GameTestHandlerWatch::GetLast(GameTestHandlerWatch::EKind::MouseDown).Serial;
				GameTestInput::SendKey(Button, /*bInDown=*/true, Message);
				State->bHeld = true;
				State->bDownOk = CheckDownHandler(State->SerialBefore, State->Target.Slate, State->Target.Path, *Payload, State->DownFailure);
				State->Stage = EStage::Up;
				State->MarkFrame = GFrameCounter;
				return false;

			case EStage::Up:
			default:
				if (GFrameCounter <= State->MarkFrame)
				{
					return false;
				}
				// 按下没接对也要松开：不留按住的键。
				GameTestInput::SendKey(Button, /*bInDown=*/false, Message);
				State->bHeld = false;
				Payload->SetStringField(TEXT("path"), State->Target.Path);
				if (!State->Target.PickNote.IsEmpty())
				{
					Payload->SetStringField(TEXT("picked"), State->Target.PickNote);
				}
				Payload->SetNumberField(TEXT("screenX"), State->Target.Screen.X);
				Payload->SetNumberField(TEXT("screenY"), State->Target.Screen.Y);
				if (!State->bDownOk)
				{
					*Failure = State->DownFailure;
				}
				return true;
			}
		};
		return GameTestHandlers::StepPending;
	}

	EGameTestStepStatus RunDrag(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		bool bOk = true;
		const FVector2D At = ReadAt(Params, InContext.Message, bOk);
		FKey Button;
		if (!bOk || !ReadButton(Params, Button, InContext.Message))
		{
			return GameTestHandlers::StepFailed;
		}

		FString ToTarget;
		GameTestJson::TryGetString(Params, TEXT("toTarget"), ToTarget);
		FVector2D ToAt(0.5, 0.5);
		if (!ReadPoint(Params, TEXT("toAt"), ToAt, InContext.Message))
		{
			return GameTestHandlers::StepFailed;
		}
		FVector2D By = FVector2D::ZeroVector;
		const bool bHasBy = Params.HasField(TEXT("by"));
		if (bHasBy)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Params.TryGetArrayField(TEXT("by"), Values) || Values == nullptr || Values->Num() != 2)
			{
				InContext.Message = TEXT("by 要写成 [dx, dy]（按视口尺寸归一化的位移，可以是负数）");
				return GameTestHandlers::StepFailed;
			}
			By = FVector2D((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber());
		}
		if (ToTarget.IsEmpty() == !bHasBy)
		{
			InContext.Message = TEXT("drag 要给 toTarget（拖到哪个控件）或 by（位移），二选一");
			return GameTestHandlers::StepFailed;
		}

		TSharedPtr<FGameTestWidgetNode> Initial;
		FString Ignored;
		GameTestHandlers::TryResolveStepTarget(InContext, Initial, Ignored);

		/** 拖动分几帧移过去：一下跳到终点，有的控件识别不出拖动。 */
		constexpr int32 MoveFrames = 8;

		enum class EStage : uint8 { Wait, Down, Move, Up };
		struct FState
		{
			EStage Stage = EStage::Wait;
			FWaitState Wait;
			FActionable Target;
			FVector2D End = FVector2D::ZeroVector;
			int32 MoveIndex = 0;
			uint64 MarkFrame = 0;
			bool bDownOk = true;
			FString DownFailure;
			/** 鼠标键按下了还没松开：步骤中途超时或取消时由收尾松开。 */
			bool bHeld = false;
		};
		const TSharedRef<FState> State = MakeShared<FState>();
		const FString Target = InContext.Step->Target;
		const TSharedPtr<FJsonObject> StepParams = InContext.Step->Params;
		TMap<FString, TWeakObjectPtr<UWidget>>* Handles = InContext.Handles;
		const TSharedPtr<FJsonObject> Payload = InContext.Payload;
		InContext.PendingProgress = MakeShared<FString>();
		InContext.PendingFailure = MakeShared<FString>();
		const TSharedPtr<FString> Progress = InContext.PendingProgress;
		const TSharedPtr<FString> Failure = InContext.PendingFailure;

		InContext.DefaultTimeoutMs = UiTimeoutMs();
		InContext.Message = ToTarget.IsEmpty()
			? FString::Printf(TEXT("从 %s 拖动 (%.2f, %.2f)"), *Target, By.X, By.Y)
			: FString::Printf(TEXT("把 %s 拖到 %s"), *Target, *ToTarget);
		// 按下后超时或被取消：执行器不再调谓词，这里把键松开，后面的步骤不会在「按住」下跑。
		InContext.PendingCleanup = [State, Button]()
		{
			if (State->bHeld)
			{
				FString Ignored;
				GameTestInput::SendKey(Button, /*bInDown=*/false, Ignored);
				State->bHeld = false;
			}
		};
		InContext.PendingPredicate = [State, Target, StepParams, Handles, Payload, Progress, Failure, At, Button, ToTarget, ToAt, By](double)
		{
			FString Message;
			switch (State->Stage)
			{
			case EStage::Wait:
				if (!CheckActionable(Target, Handles, GameTestHintPick::GetHint(StepParams), At, /*bInNeedHit=*/true, State->Wait, State->Target, *Progress))
				{
					return false;
				}
				GameTestInput::MovePointerTo(State->Target.Screen, Message);
				State->Stage = EStage::Down;
				State->MarkFrame = GFrameCounter;
				return false;

			case EStage::Down:
			{
				if (GFrameCounter <= State->MarkFrame)
				{
					return false;
				}

				// 终点在按下前算好：toTarget 只要求解析得到、有几何，不要求没被挡住（拖过去的东西常常盖在上面）。
				if (!ToTarget.IsEmpty())
				{
					TSharedPtr<FGameTestWidgetNode> EndNode;
					FString EndError;
					const TSharedPtr<SWidget> EndSlate = GameTestHandlers::TryResolveTarget(ToTarget, Handles, EndNode, EndError) ? SlateOf(*EndNode) : nullptr;
					if (!EndSlate.IsValid())
					{
						*Progress = FString::Printf(TEXT("终点 %s 还没出现：%s"), *ToTarget, *EndError);
						return false;
					}
					const FGeometry& EndGeometry = EndSlate->GetCachedGeometry();
					State->End = FVector2D(EndGeometry.LocalToAbsolute(FVector2D(EndGeometry.GetLocalSize()) * ToAt));
				}
				else
				{
					const TSharedPtr<SWidget> Viewport = GameTestSlate::GetGameViewportWidget();
					const FVector2D ViewSize = Viewport.IsValid() ? FVector2D(Viewport->GetCachedGeometry().GetAbsoluteSize()) : FVector2D::ZeroVector;
					State->End = State->Target.Screen + FVector2D(By.X * ViewSize.X, By.Y * ViewSize.Y);
				}

				const uint64 SerialBefore = GameTestHandlerWatch::GetLast(GameTestHandlerWatch::EKind::MouseDown).Serial;
				GameTestInput::SendKey(Button, /*bInDown=*/true, Message);
				State->bHeld = true;
				State->bDownOk = CheckDownHandler(SerialBefore, State->Target.Slate, State->Target.Path, *Payload, State->DownFailure);
				State->Stage = State->bDownOk ? EStage::Move : EStage::Up;
				State->MarkFrame = GFrameCounter;
				return false;
			}

			case EStage::Move:
				if (GFrameCounter <= State->MarkFrame)
				{
					return false;
				}
				++State->MoveIndex;
				GameTestInput::MovePointerTo(
					FMath::Lerp(State->Target.Screen, State->End, static_cast<double>(State->MoveIndex) / MoveFrames), Message);
				State->MarkFrame = GFrameCounter;
				if (State->MoveIndex >= MoveFrames)
				{
					State->Stage = EStage::Up;
				}
				return false;

			case EStage::Up:
			default:
				if (GFrameCounter <= State->MarkFrame)
				{
					return false;
				}
				GameTestInput::SendKey(Button, /*bInDown=*/false, Message);
				State->bHeld = false;
				Payload->SetStringField(TEXT("path"), State->Target.Path);
				if (!State->Target.PickNote.IsEmpty())
				{
					Payload->SetStringField(TEXT("picked"), State->Target.PickNote);
				}
				if (!State->bDownOk)
				{
					*Failure = State->DownFailure;
				}
				return true;
			}
		};
		return GameTestHandlers::StepPending;
	}

	EGameTestStepStatus RunWheel(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		double Delta = 0.0;
		if (!GameTestJson::TryGetNumber(Params, TEXT("delta"), Delta) || FMath::IsNearlyZero(Delta))
		{
			InContext.Message = TEXT("wheel 需要 delta（滚几格，正数向上、负数向下）");
			return GameTestHandlers::StepFailed;
		}

		// 没给 target：在光标当前位置滚（游戏画面上的缩放之类，先用 input.cursor 摆好光标）。
		if (InContext.Step->Target.IsEmpty())
		{
			if (!GameTestInput::SendWheel(static_cast<float>(Delta), InContext.Message))
			{
				return GameTestHandlers::StepFailed;
			}
			return GameTestHandlers::StepSucceeded;
		}

		bool bOk = true;
		const FVector2D At = ReadAt(Params, InContext.Message, bOk);
		if (!bOk)
		{
			return GameTestHandlers::StepFailed;
		}

		TSharedPtr<FGameTestWidgetNode> Initial;
		FString Ignored;
		GameTestHandlers::TryResolveStepTarget(InContext, Initial, Ignored);

		struct FState
		{
			bool bMoved = false;
			FWaitState Wait;
			FActionable Target;
			uint64 MarkFrame = 0;
		};
		const TSharedRef<FState> State = MakeShared<FState>();
		const FString Target = InContext.Step->Target;
		const TSharedPtr<FJsonObject> StepParams = InContext.Step->Params;
		TMap<FString, TWeakObjectPtr<UWidget>>* Handles = InContext.Handles;
		const TSharedPtr<FJsonObject> Payload = InContext.Payload;
		InContext.PendingProgress = MakeShared<FString>();
		const TSharedPtr<FString> Progress = InContext.PendingProgress;

		InContext.DefaultTimeoutMs = UiTimeoutMs();
		InContext.Message = FString::Printf(TEXT("在 %s 上滚动 %.1f"), *Target, Delta);
		InContext.PendingPredicate = [State, Target, StepParams, Handles, Payload, Progress, At, Delta](double)
		{
			FString Message;
			if (!State->bMoved)
			{
				if (!CheckActionable(Target, Handles, GameTestHintPick::GetHint(StepParams), At, /*bInNeedHit=*/true, State->Wait, State->Target, *Progress))
				{
					return false;
				}
				GameTestInput::MovePointerTo(State->Target.Screen, Message);
				State->bMoved = true;
				State->MarkFrame = GFrameCounter;
				return false;
			}
			if (GFrameCounter <= State->MarkFrame)
			{
				return false;
			}
			GameTestInput::SendWheel(static_cast<float>(Delta), Message);
			Payload->SetStringField(TEXT("path"), State->Target.Path);
			if (!State->Target.PickNote.IsEmpty())
			{
				Payload->SetStringField(TEXT("picked"), State->Target.PickNote);
			}
			return true;
		};
		return GameTestHandlers::StepPending;
	}

	EGameTestStepStatus RunTypeKeys(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		FString Text;
		if (!GameTestJson::TryGetString(Params, TEXT("text"), Text))
		{
			InContext.Message = TEXT("type 需要 text");
			return GameTestHandlers::StepFailed;
		}
		bool bCommit = false;
		GameTestJson::TryGetBool(Params, TEXT("commit"), bCommit);
		bool bAppend = false;
		GameTestJson::TryGetBool(Params, TEXT("append"), bAppend);

		TSharedPtr<FGameTestWidgetNode> Initial;
		FString Ignored;
		GameTestHandlers::TryResolveStepTarget(InContext, Initial, Ignored);

		struct FState
		{
			FWaitState Wait;
			FActionable Target;
		};
		const TSharedRef<FState> State = MakeShared<FState>();
		const FString Target = InContext.Step->Target;
		const TSharedPtr<FJsonObject> StepParams = InContext.Step->Params;
		TMap<FString, TWeakObjectPtr<UWidget>>* Handles = InContext.Handles;
		const TSharedPtr<FJsonObject> Payload = InContext.Payload;
		InContext.PendingProgress = MakeShared<FString>();
		InContext.PendingFailure = MakeShared<FString>();
		const TSharedPtr<FString> Progress = InContext.PendingProgress;
		const TSharedPtr<FString> Failure = InContext.PendingFailure;

		InContext.DefaultTimeoutMs = UiTimeoutMs();
		InContext.Message = FString::Printf(TEXT("在 %s 输入「%s」%s"), *Target, *Text, bCommit ? TEXT("并回车") : TEXT(""));
		InContext.PendingPredicate = [State, Target, StepParams, Handles, Payload, Progress, Failure, Text, bCommit, bAppend](double)
		{
			// 打字不要求点击点没被挡住：焦点直接给控件，不经过鼠标。
			if (!CheckActionable(Target, Handles, GameTestHintPick::GetHint(StepParams), FVector2D(0.5, 0.5), /*bInNeedHit=*/false, State->Wait, State->Target, *Progress))
			{
				return false;
			}

			FSlateApplication& App = FSlateApplication::Get();
			App.SetUserFocus(0, State->Target.Slate, EFocusCause::SetDirectly);
			if (!GameTestSlate::IsSelfOrAncestor(State->Target.Slate.Get(), App.GetUserFocusedWidget(0)))
			{
				*Failure = FString::Printf(TEXT("%s 不接受键盘焦点，没法打字（它是输入框吗？）；焦点在 %s"),
					*State->Target.Path, *GameTestSlate::Describe(App.GetUserFocusedWidget(0)));
				return true;
			}

			FString Message;
			if (!bAppend)
			{
				GameTestInput::SendSelectAll(Message);
			}
			GameTestInput::SendCharacters(Text, Message);
			if (bCommit)
			{
				GameTestInput::SendKeyTo(EKeys::Enter, /*bInDown=*/true, State->Target.Slate, Message);
				GameTestInput::SendKeyTo(EKeys::Enter, /*bInDown=*/false, State->Target.Slate, Message);
			}
			Payload->SetStringField(TEXT("path"), State->Target.Path);
			if (!State->Target.PickNote.IsEmpty())
			{
				Payload->SetStringField(TEXT("picked"), State->Target.PickNote);
			}
			Payload->SetStringField(TEXT("text"), Text);
			return true;
		};
		return GameTestHandlers::StepPending;
	}
}

namespace GameTestHandlers
{
	EGameTestStepStatus HandleClickPointer(FGameTestStepContext& InContext)
	{
		return GameTestPointerDetail::RunClickPointer(InContext);
	}

	EGameTestStepStatus HandleDrag(FGameTestStepContext& InContext)
	{
		return GameTestPointerDetail::RunDrag(InContext);
	}

	EGameTestStepStatus HandleWheel(FGameTestStepContext& InContext)
	{
		return GameTestPointerDetail::RunWheel(InContext);
	}

	EGameTestStepStatus HandleTypeKeys(FGameTestStepContext& InContext)
	{
		return GameTestPointerDetail::RunTypeKeys(InContext);
	}
}
