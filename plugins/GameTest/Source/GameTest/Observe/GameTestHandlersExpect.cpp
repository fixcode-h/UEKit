// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestHandlerShared.h"
#include "GameTestCompat.h"

#include "Core/GameTestJson.h"
#include "Core/GameTestWorld.h"
#include "Editor/GameTestEditorApi.h"
#include "Observe/GameTestActor.h"
#include "Observe/GameTestCondition.h"
#include "Observe/GameTestItemMatch.h"
#include "Observe/GameTestSelector.h"
#include "Observe/GameTestWidgetNode.h"

#include "Components/ListView.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace
{
	/** net.compare 的一端：在哪个世界、选谁。 */
	struct FNetCompareSide
	{
		FString ClientText;
		bool bClientSpecified = false;
		TOptional<bool> LocallyControlled;
	};

	/**
	 * 读 `from` / `to` 对象。对象缺省时按缺省值构造：from = 服务器世界（交给 ResolveWorld 决定），
	 * to = client 0。
	 */
	bool ParseNetCompareSide(const TSharedPtr<FJsonObject>& InObject, bool bInDefaultToFirstClient, const TCHAR* InSideName, FNetCompareSide& OutSide, FString& OutError)
	{
		OutSide = FNetCompareSide();

		if (!InObject.IsValid())
		{
			if (bInDefaultToFirstClient)
			{
				OutSide.bClientSpecified = true;
				OutSide.ClientText = TEXT("0");
			}
			return true;
		}

		for (const auto& Pair : InObject->Values)
		{
			const FString PairKey = GAMETEST_JSON_KEY(Pair.Key);
			if (PairKey != TEXT("client") && PairKey != TEXT("locallyControlled"))
			{
				OutError = FString::Printf(TEXT("%s 里不认识字段 '%s'（可用：client, locallyControlled）"), InSideName, *PairKey);
				return false;
			}
		}

		if (!GameTestHandlers::TryReadClientField(*InObject, OutSide.ClientText, OutSide.bClientSpecified, OutError))
		{
			OutError = FString::Printf(TEXT("%s：%s"), InSideName, *OutError);
			return false;
		}

		bool bLocallyControlled = false;
		if (GameTestJson::TryGetBool(*InObject, TEXT("locallyControlled"), bLocallyControlled))
		{
			OutSide.LocallyControlled = bLocallyControlled;
		}
		return true;
	}

	enum class ENetCompareOutcome : uint8
	{
		Match,
		Mismatch,
		Unresolved,
	};

	TSharedPtr<FJsonObject> MakeLocationJson(const FVector& InLocation)
	{
		const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("x"), InLocation.X);
		Json->SetNumberField(TEXT("y"), InLocation.Y);
		Json->SetNumberField(TEXT("z"), InLocation.Z);
		return Json;
	}

	/** 比较同一玩家对应 actor 在两端世界的位姿；结果同时写进 payload 与消息。 */
	ENetCompareOutcome EvaluateNetCompare(const FString& InClassName, const FString& InTag, const FNetCompareSide& InFrom, const FNetCompareSide& InTo, double InTolerance, const TSharedPtr<FJsonObject>& InOutPayload, FString& OutMessage)
	{
		InOutPayload->SetNumberField(TEXT("tolerance"), InTolerance);

		FString SideError;
		UWorld* FromWorld = GameTestWorld::ResolveWorld(InFrom.ClientText, InFrom.bClientSpecified, SideError);
		if (FromWorld == nullptr)
		{
			OutMessage = FString::Printf(TEXT("from 端：%s"), *SideError);
			return ENetCompareOutcome::Unresolved;
		}

		AActor* FromActor = GameTestActor::ResolveOne(*FromWorld, InClassName, InTag, InFrom.LocallyControlled, SideError);
		if (FromActor == nullptr)
		{
			OutMessage = FString::Printf(TEXT("from（%s）：%s"), *GameTestWorld::DescribeWorld(FromWorld), *SideError);
			return ENetCompareOutcome::Unresolved;
		}

		SideError.Reset();
		UWorld* ToWorld = GameTestWorld::ResolveWorld(InTo.ClientText, InTo.bClientSpecified, SideError);
		if (ToWorld == nullptr)
		{
			OutMessage = FString::Printf(TEXT("to 端：%s"), *SideError);
			return ENetCompareOutcome::Unresolved;
		}

		AActor* ToActor = GameTestActor::ResolveOne(*ToWorld, InClassName, InTag, InTo.LocallyControlled, SideError);
		if (ToActor == nullptr)
		{
			OutMessage = FString::Printf(TEXT("to（%s）：%s"), *GameTestWorld::DescribeWorld(ToWorld), *SideError);
			return ENetCompareOutcome::Unresolved;
		}

		const FVector FromLocation = FromActor->GetActorLocation();
		const FVector ToLocation = ToActor->GetActorLocation();
		const double Distance = FVector::Dist(FromLocation, ToLocation);
		const double YawDelta = FMath::FindDeltaAngleDegrees(FromActor->GetActorRotation().Yaw, ToActor->GetActorRotation().Yaw);
		const double SpeedDelta = FMath::Abs(FromActor->GetVelocity().Size() - ToActor->GetVelocity().Size());

		const TSharedPtr<FJsonObject> FromJson = MakeShared<FJsonObject>();
		FromJson->SetStringField(TEXT("world"), GameTestWorld::DescribeWorld(FromWorld));
		FromJson->SetStringField(TEXT("path"), FromActor->GetPathName());
		FromJson->SetObjectField(TEXT("location"), MakeLocationJson(FromLocation));
		InOutPayload->SetObjectField(TEXT("from"), FromJson);

		const TSharedPtr<FJsonObject> ToJson = MakeShared<FJsonObject>();
		ToJson->SetStringField(TEXT("world"), GameTestWorld::DescribeWorld(ToWorld));
		ToJson->SetStringField(TEXT("path"), ToActor->GetPathName());
		ToJson->SetObjectField(TEXT("location"), MakeLocationJson(ToLocation));
		InOutPayload->SetObjectField(TEXT("to"), ToJson);

		InOutPayload->SetNumberField(TEXT("distance"), Distance);
		InOutPayload->SetNumberField(TEXT("yawDelta"), YawDelta);
		InOutPayload->SetNumberField(TEXT("speedDelta"), SpeedDelta);

		if (Distance <= InTolerance)
		{
			OutMessage = FString::Printf(TEXT("两端一致：[%s] 与 [%s] 偏差 %.1f cm（阈值 %.0f）"),
				*GameTestWorld::DescribeWorld(FromWorld), *GameTestWorld::DescribeWorld(ToWorld), Distance, InTolerance);
			return ENetCompareOutcome::Match;
		}

		OutMessage = FString::Printf(TEXT("两端偏差 %.1f cm 超过阈值 %.0f：[%s] %s ↔ [%s] %s"),
			Distance, InTolerance,
			*GameTestWorld::DescribeWorld(FromWorld), *FromLocation.ToCompactString(),
			*GameTestWorld::DescribeWorld(ToWorld), *ToLocation.ToCompactString());
		return ENetCompareOutcome::Mismatch;
	}
}

namespace GameTestHandlers
{
	EGameTestStepStatus HandleWait(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;

		FString Until;
		if (!GameTestJson::TryGetString(Params, TEXT("until"), Until))
		{
			InContext.Message = TEXT("wait 需要 until（pieReady / pieStopped / widget / widgetGone / seconds / actorNear / cond）");
			return StepFailed;
		}

		if (Until == TEXT("cond"))
		{
			return StartWaitCond(InContext);
		}

		if (Until == TEXT("seconds"))
		{
			double Seconds = 0.0;
			if (!GameTestJson::TryGetNumber(Params, TEXT("seconds"), Seconds) || Seconds <= 0.0)
			{
				InContext.Message = TEXT("wait until=seconds 需要 seconds（正数）");
				return StepFailed;
			}

			const double StartTime = FPlatformTime::Seconds();
			InContext.PendingPredicate = [StartTime, Seconds](double)
			{
				return FPlatformTime::Seconds() - StartTime >= Seconds;
			};
			InContext.Message = FString::Printf(TEXT("等待 %.2f 秒"), Seconds);
			return StepPending;
		}

		if (Until == TEXT("pieReady"))
		{
			InContext.PendingPredicate = [](double)
			{
				return GameTestWorld::IsPlayable(GameTestWorld::FindGameWorld());
			};
			InContext.Message = TEXT("等待 PIE 世界跑起来");
			return StepPending;
		}

		if (Until == TEXT("pieStopped"))
		{
			if (!GameTestEditor::IsAvailable())
			{
				InContext.Message = TEXT("当前进程没有编辑器，没有 PIE 可等（独立进程请直接等进程退出）");
				return StepFailed;
			}

			InContext.PendingPredicate = [](double)
			{
				return !GameTestEditor::IsPieRunning();
			};
			InContext.Message = TEXT("等待 PIE 结束");
			return StepPending;
		}

		if (Until == TEXT("widget") || Until == TEXT("widgetGone"))
		{
			if (InContext.Step->Target.IsEmpty())
			{
				InContext.Message = FString::Printf(TEXT("wait until=%s 需要 target"), *Until);
				return StepFailed;
			}

			const FString Target = InContext.Step->Target;
			TMap<FString, TWeakObjectPtr<UWidget>>* Handles = InContext.Handles;
			const bool bExpectPresent = (Until == TEXT("widget"));
			// 等界面出现 / 消失和点击的自动等待同一个上限（默认不限）。
			InContext.DefaultTimeoutMs = InContext.UiWaitDefault();

			InContext.PendingPredicate = [Target, Handles, bExpectPresent](double)
			{
				TSharedPtr<FGameTestWidgetNode> Node;
				FString Ignored;
				return TryResolveTarget(Target, Handles, Node, Ignored) == bExpectPresent;
			};
			InContext.Message = FString::Printf(TEXT("等待 %s %s"), *Target, bExpectPresent ? TEXT("出现") : TEXT("消失"));
			return StepPending;
		}

		if (Until == TEXT("actorNear"))
		{
			FString ClassName;
			FString Tag;
			GameTestJson::TryGetString(Params, TEXT("actor"), ClassName);
			GameTestJson::TryGetString(Params, TEXT("tag"), Tag);
			if (ClassName.IsEmpty() && Tag.IsEmpty())
			{
				InContext.Message = TEXT("wait until=actorNear 需要 actor（类名）或 tag");
				return StepFailed;
			}

			const TArray<TSharedPtr<FJsonValue>>* PointValues = GameTestJson::TryGetArray(Params, TEXT("point"));
			if (PointValues == nullptr || PointValues->Num() < 3)
			{
				InContext.Message = TEXT("wait until=actorNear 需要 point（[x,y,z] 世界坐标）");
				return StepFailed;
			}

			// JSON 数字取成 double 再收进 FVector：不做"读不到就当 0"，少一个分量就是写错了。
			double Components[3] = { 0.0, 0.0, 0.0 };
			for (int32 Index = 0; Index < 3; ++Index)
			{
				if (!(*PointValues)[Index].IsValid() || !(*PointValues)[Index]->TryGetNumber(Components[Index]))
				{
					InContext.Message = FString::Printf(TEXT("wait until=actorNear 的 point 第 %d 个分量不是数字"), Index + 1);
					return StepFailed;
				}
			}

			double Radius = 0.0;
			if (!GameTestJson::TryGetNumber(Params, TEXT("radius"), Radius) || Radius <= 0.0)
			{
				InContext.Message = TEXT("wait until=actorNear 需要 radius（正数，世界单位）");
				return StepFailed;
			}

			const FVector Point(Components[0], Components[1], Components[2]);

			// 进度描述每帧刷新，超时时由执行器拼进失败消息（"还差多远"）。
			InContext.PendingProgress = MakeShared<FString>();
			TSharedPtr<FString> Progress = InContext.PendingProgress;
			InContext.PendingPredicate = [ClassName, Tag, Point, Radius, Progress](double)
			{
				UWorld* World = GameTestWorld::FindGameWorld();
				if (World == nullptr)
				{
					*Progress = TEXT("当前没有正在跑的游戏世界");
					return false;
				}

				FString Error;
				AActor* Actor = GameTestActor::ResolveOne(*World, ClassName, Tag, TOptional<bool>(), Error);
				if (Actor == nullptr)
				{
					// 找不唯一/找不到都写进进度：超时时这条消息比"没等到"有用得多。
					*Progress = Error;
					return false;
				}

				const double Distance = FVector::Dist(Actor->GetActorLocation(), Point);
				*Progress = FString::Printf(TEXT("%s 距目标点 %.1f（半径 %.1f）"), *Actor->GetName(), Distance, Radius);
				return Distance <= Radius;
			};

			InContext.Message = FString::Printf(TEXT("等待 %s 进入 (%.0f, %.0f, %.0f) 的 %.0f 半径内"),
				ClassName.IsEmpty() ? *Tag : *ClassName, Point.X, Point.Y, Point.Z, Radius);
			return StepPending;
		}

		InContext.Message = FString::Printf(
			TEXT("wait 不认识 until='%s'（可用：pieReady / pieStopped / widget / widgetGone / seconds / actorNear / cond）"), *Until);
		return StepFailed;
	}

	EGameTestStepStatus HandleAssert(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;

		// 1.9 的 cond / expr：先判它们；没有原有的条件字段时就是全部结论
		bool bScriptHolds = true;
		const bool bHasScript = CheckScriptAssert(InContext, bScriptHolds);
		if (bHasScript && !bScriptHolds)
		{
			return StepFailed;
		}
		if (bHasScript && InContext.Step->Target.IsEmpty())
		{
			return StepSucceeded;
		}
		const FString ScriptMessage = bHasScript ? InContext.Message : FString();

		TSharedPtr<FGameTestWidgetNode> Node;
		FString ResolveError;
		const bool bResolved = TryResolveTarget(InContext.Step->Target, InContext.Handles, Node, ResolveError);
		if (bResolved)
		{
			// 断言不拿"解析失败"当步骤失败（exists=false 就是靠它判的），但碰过谁照样要记。
			InContext.TouchedWidget = Node->Widget;
			InContext.bTouchedHasRect = Node->bHasGeometry && Node->Size.X >= 1.0 && Node->Size.Y >= 1.0;
			InContext.TouchedMin = Node->Position;
			InContext.TouchedMax = Node->Position + Node->Size;
		}

		// 条件判断和「已达成就跳过」共用一份（GameTestCondition），断言只管把结果写成通过 / 失败。
		FGameTestConditionCheck Check;
		GameTestCondition::Check(Params, bResolved ? Node.Get() : nullptr, ResolveError, Check);

		if (Check.Count == 0 && bHasScript)
		{
			// 写了 target 只是让判断里省掉 target：结论就是 cond / expr 的
			InContext.Message = ScriptMessage;
			return StepSucceeded;
		}
		if (Check.Count == 0)
		{
			InContext.Message = TEXT("assert 至少要给一个条件（exists / visible / enabled / text / textContains / textLike / checked / count / selected / selectedProperty+equals / stateKey+equals / field+equals / image / any）");
			return StepFailed;
		}

		if (!Check.Holds())
		{
			InContext.Message = FString::Printf(TEXT("断言失败：%s；实际：%s"),
				*FString::Join(Check.Mismatches, TEXT("；")),
				Check.Actuals.Num() > 0 ? *FString::Join(Check.Actuals, TEXT("、")) : TEXT("（无）"));
			return StepFailed;
		}

		if (!bResolved)
		{
			InContext.Message = TEXT("断言通过：目标确实不存在");
			return StepSucceeded;
		}
		InContext.Message = FString::Printf(TEXT("断言通过：%s（%s）"),
			*Node->Path, Check.Actuals.Num() > 0 ? *FString::Join(Check.Actuals, TEXT("、")) : TEXT("存在"));
		return StepSucceeded;
	}

	EGameTestStepStatus HandleNetCompare(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;

		FString ClassName;
		FString Tag;
		GameTestJson::TryGetString(Params, TEXT("class"), ClassName);
		GameTestJson::TryGetString(Params, TEXT("tag"), Tag);
		if (ClassName.IsEmpty() && Tag.IsEmpty())
		{
			InContext.Message = TEXT("net.compare 需要 class 或 tag 之一来选 actor");
			return StepFailed;
		}

		double Tolerance = 0.0;
		if (!GameTestJson::TryGetNumber(Params, TEXT("tolerance"), Tolerance) || Tolerance < 0.0)
		{
			InContext.Message = TEXT("net.compare 需要 tolerance（位置偏差上限，厘米，≥0）");
			return StepFailed;
		}

		// 5.8 的 TryGetObjectField 是「bool + 出参」签名；出参为 nullptr 表示字段缺失或不是对象。
		const TSharedPtr<FJsonObject>* FromObjectPtr = nullptr;
		const TSharedPtr<FJsonObject>* ToObjectPtr = nullptr;
		Params.TryGetObjectField(TEXT("from"), FromObjectPtr);
		Params.TryGetObjectField(TEXT("to"), ToObjectPtr);
		const TSharedPtr<FJsonObject> FromObject = FromObjectPtr != nullptr ? *FromObjectPtr : nullptr;
		const TSharedPtr<FJsonObject> ToObject = ToObjectPtr != nullptr ? *ToObjectPtr : nullptr;
		if (Params.Values.Contains(TEXT("from")) && !FromObject.IsValid())
		{
			InContext.Message = TEXT("from 必须是对象（{ \"client\": \"server\" | 序号, \"locallyControlled\": true|false }）");
			return StepFailed;
		}
		if (Params.Values.Contains(TEXT("to")) && !ToObject.IsValid())
		{
			InContext.Message = TEXT("to 必须是对象（{ \"client\": \"server\" | 序号, \"locallyControlled\": true|false }）");
			return StepFailed;
		}

		FNetCompareSide FromSide;
		FNetCompareSide ToSide;
		FString Error;
		if (!ParseNetCompareSide(FromObject, /*bInDefaultToFirstClient=*/false, TEXT("from"), FromSide, Error)
			|| !ParseNetCompareSide(ToObject, /*bInDefaultToFirstClient=*/true, TEXT("to"), ToSide, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		// 判据是"作者有没有写 timeoutMs"（看 `Params` 原文），不是 `Step->TimeoutMs`——
		// 执行器会把缺省超时（DefaultStepTimeoutMs）覆写进 `Step->TimeoutMs`（GameTestRunner.cpp:124-127），
		// 用它判断会让每一步都走跨帧分支（2026-10-07 实测踩过）。
		if (InContext.Step->Params->Values.Contains(TEXT("timeoutMs")))
		{
			InContext.PendingProgress = MakeShared<FString>();
			TSharedPtr<FString> Progress = InContext.PendingProgress;
			const TSharedPtr<FJsonObject> Payload = InContext.Payload;
			InContext.PendingPredicate = [ClassName, Tag, FromSide, ToSide, Tolerance, Progress, Payload](double)
			{
				FString Message;
				const ENetCompareOutcome Outcome = EvaluateNetCompare(ClassName, Tag, FromSide, ToSide, Tolerance, Payload, Message);
				*Progress = Message;
				return Outcome == ENetCompareOutcome::Match;
			};
			InContext.Message = TEXT("等待两端一致");
			return StepPending;
		}

		FString Message;
		const ENetCompareOutcome Outcome = EvaluateNetCompare(ClassName, Tag, FromSide, ToSide, Tolerance, InContext.Payload, Message);
		InContext.Message = Message;
		return Outcome == ENetCompareOutcome::Match ? StepSucceeded : StepFailed;
	}
}
