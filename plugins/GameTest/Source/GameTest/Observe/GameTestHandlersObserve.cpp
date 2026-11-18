// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestHandlerShared.h"

#include "Core/GameTestJson.h"
#include "Core/GameTestWorld.h"
#include "Editor/GameTestEditorApi.h"
#include "Observe/GameTestHitTest.h"
#include "Observe/GameTestSelector.h"
#include "Observe/GameTestSnapshot.h"
#include "Observe/GameTestWidgetJson.h"
#include "Observe/GameTestWidgetNode.h"
#include "Shot/GameTestShot.h"
#include "GameTestSettings.h"

#include "Widgets/SWidget.h"

#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/Paths.h"
#include "GameFramework/Pawn.h"
#include "GameTestCompat.h" // FStringOutputDevice 的头在 5.4 和之后的版本里位置不同

#if WITH_EDITOR
#include "Editor.h"
#endif

namespace
{
	bool TryCaptureScreens(UWorld* InWorld, TArray<TSharedPtr<FGameTestWidgetNode>>& OutScreens, FString& OutMessage)
	{
		if (InWorld == nullptr)
		{
			OutMessage = TEXT("当前没有正在跑的游戏世界（先 pie.start，或 wait until=pieReady）");
			return false;
		}

		GameTestSnapshot::CaptureScreens(InWorld, OutScreens);
		if (OutScreens.Num() == 0)
		{
			OutMessage = TEXT("世界里没有挂在视口上的 UMG（用 widget.add 造一个靶子，或确认界面真的被打开了）");
			return false;
		}

		return true;
	}
}

namespace GameTestHandlers
{
	EGameTestStepStatus HandlePing(FGameTestStepContext& InContext)
	{
		UWorld* World = GameTestWorld::FindGameWorld();
		InContext.Payload->SetBoolField(TEXT("worldPresent"), World != nullptr);
		InContext.Payload->SetBoolField(TEXT("playable"), GameTestWorld::IsPlayable(World));
		InContext.Payload->SetObjectField(TEXT("editor"), GameTestEditor::BuildStatusJson());
		if (World != nullptr)
		{
			InContext.Payload->SetStringField(TEXT("map"), World->GetOutermost()->GetName());
		}

		// 世界清单：多客户端会话里，这是"有哪些世界、各自跑没跑起来"的唯一直接观察。
		TArray<UWorld*> Worlds;
		GameTestWorld::FindGameWorlds(Worlds);
		TArray<TSharedPtr<FJsonValue>> WorldValues;
		WorldValues.Reserve(Worlds.Num());
		for (UWorld* EachWorld : Worlds)
		{
			const TSharedPtr<FJsonObject> WorldJson = MakeShared<FJsonObject>();
			WorldJson->SetStringField(TEXT("label"), GameTestWorld::DescribeWorld(EachWorld));
			WorldJson->SetBoolField(TEXT("playable"), GameTestWorld::IsPlayable(EachWorld));
			const APlayerController* Controller = EachWorld->GetFirstPlayerController();
			WorldJson->SetBoolField(TEXT("hasLocalPlayer"), Controller != nullptr && Controller->GetPawn() != nullptr);
			WorldValues.Add(MakeShared<FJsonValueObject>(WorldJson));
		}
		InContext.Payload->SetArrayField(TEXT("worlds"), WorldValues);

		InContext.Message = TEXT("pong");
		return StepSucceeded;
	}

	EGameTestStepStatus HandleMark(FGameTestStepContext& InContext)
	{
		FString Text;
		if (!GameTestJson::TryGetString(*InContext.Step->Params, TEXT("text"), Text))
		{
			InContext.Message = TEXT("mark 需要 text（要记进报告的那句话）");
			return StepFailed;
		}

		InContext.Message = Text;
		return StepSucceeded;
	}

	EGameTestStepStatus HandleFind(FGameTestStepContext& InContext)
	{
		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		if (!TryResolveStepTarget(InContext, Node, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		FString HandleName;
		if (!GameTestJson::TryGetString(*InContext.Step->Params, TEXT("into"), HandleName) || HandleName.IsEmpty())
		{
			// 缺省用路径末段当句柄名：调用顺序读起来最自然，而且名字一定写在报告里。
			HandleName = Node->Name;
		}

		if (InContext.Handles == nullptr)
		{
			InContext.Message = TEXT("句柄表不可用");
			return StepFailed;
		}

		(*InContext.Handles).Add(HandleName, Node->Widget);

		InContext.Payload->SetStringField(TEXT("handle"), HandleName);
		InContext.Payload->SetStringField(TEXT("path"), Node->Path);
		InContext.Payload->SetStringField(TEXT("class"), Node->ClassName);
		InContext.Payload->SetStringField(TEXT("visibility"), Node->Visibility);
		InContext.Message = FString::Printf(TEXT("已捕获句柄 @%s → %s（%s）"), *HandleName, *Node->Path, *Node->ClassName);
		return StepSucceeded;
	}

	EGameTestStepStatus HandleSnapshot(FGameTestStepContext& InContext)
	{
		bool bGeometry = true;
		if (const UGameTestSettings* Settings = GetDefault<UGameTestSettings>())
		{
			bGeometry = Settings->bSnapshotWithGeometry;
		}
		GameTestJson::TryGetBool(*InContext.Step->Params, TEXT("geometry"), bGeometry);

		bool bProps = false;
		GameTestJson::TryGetBool(*InContext.Step->Params, TEXT("props"), bProps);

		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;

		if (!InContext.Step->Target.IsEmpty())
		{
			TSharedPtr<FGameTestWidgetNode> Node;
			FString Error;
			if (!TryResolveStepTarget(InContext, Node, Error))
			{
				// 失败也把当前屏清单带回去：AI 看得到"现在有哪些界面"，下一步就能改对。
				TArray<TSharedPtr<FGameTestWidgetNode>> AllScreens;
				FString Ignored;
				if (TryCaptureScreens(InContext.World, AllScreens, Ignored))
				{
					InContext.Payload->SetObjectField(TEXT("screens"),
						GameTestSnapshot::ScreensToJson(AllScreens, /*bInIncludeGeometry=*/false, /*bInIncludeProps=*/false, MaxSnapshotNodes));
				}
				InContext.Message = Error;
				return StepFailed;
			}

			Screens.Add(Node);
		}
		else
		{
			FString Error;
			if (!TryCaptureScreens(InContext.World, Screens, Error))
			{
				InContext.Message = Error;
				return StepFailed;
			}
		}

		int32 NodeCount = 0;
		const TSharedPtr<FJsonObject> Snapshot = GameTestSnapshot::ScreensToJson(Screens, bGeometry, bProps, MaxSnapshotNodes);
		for (const TSharedPtr<FGameTestWidgetNode>& Screen : Screens)
		{
			int32 Count = 0;
			GameTestWidgetJson::ToJson(*Screen, false, false, MaxSnapshotNodes, Count);
			NodeCount += Count;
		}

		InContext.Payload->SetObjectField(TEXT("snapshot"), Snapshot);
		InContext.Message = FString::Printf(TEXT("快照：%d 个屏、约 %d 个控件"), Screens.Num(), NodeCount);
		return StepSucceeded;
	}

	EGameTestStepStatus HandleScreenshot(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;

		FString ShotName;
		if (!GameTestJson::TryGetString(Params, TEXT("name"), ShotName) || ShotName.IsEmpty())
		{
			ShotName = InContext.Step->Action.ToString();
		}

		bool bAnnotate = false;
		GameTestJson::TryGetBool(Params, TEXT("annotate"), bAnnotate);

		// 标注与截图用同一份树：图例里的路径就是快照里的路径。
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		if (bAnnotate || !InContext.Step->Target.IsEmpty())
		{
			if (!InContext.Step->Target.IsEmpty())
			{
				TSharedPtr<FGameTestWidgetNode> Node;
				FString Error;
				if (!TryResolveStepTarget(InContext, Node, Error))
				{
					InContext.Message = Error;
					return StepFailed;
				}
				Screens.Add(Node);
			}
			else
			{
				FString Error;
				if (!TryCaptureScreens(InContext.World, Screens, Error))
				{
					InContext.Message = Error;
					return StepFailed;
				}
			}
		}

		const FString FileName = FString::Printf(TEXT("%02d-%s.png"), InContext.Step->Index, *ShotName);
		const FString AbsolutePath = FPaths::Combine(InContext.OutputDir, FileName);

		FString Message;
		if (!GameTestShot::CaptureGameWindow(InContext.World, AbsolutePath,
				bAnnotate ? &Screens : nullptr, Message, InContext.Payload))
		{
			InContext.Message = Message;
			return StepFailed;
		}

		InContext.Message = Message;
		return StepSucceeded;
	}

	EGameTestStepStatus HandleConsole(FGameTestStepContext& InContext)
	{
		FString Command;
		if (!GameTestJson::TryGetString(*InContext.Step->Params, TEXT("command"), Command) || Command.IsEmpty())
		{
			InContext.Message = TEXT("console 需要 command");
			return StepFailed;
		}

		FStringOutputDevice Output;
		bool bHandled = false;

#if WITH_EDITOR
		if (GEditor != nullptr)
		{
			bHandled = GEditor->Exec(InContext.World, *Command, Output);
		}
		else
#endif
		if (GEngine != nullptr)
		{
			bHandled = GEngine->Exec(InContext.World, *Command, Output);
		}

		const FString Captured = FString(*Output);
		InContext.Payload->SetBoolField(TEXT("handled"), bHandled);
		InContext.Payload->SetStringField(TEXT("output"), Captured.Left(4000));

		// 可选断言：输出必须包含指定子串，否则本步失败——把"世界状态"变成场景可判定的观察
		// （例：查询命令输出的 "Wrecked=true"）。
		FString ExpectContains;
		if (GameTestJson::TryGetString(*InContext.Step->Params, TEXT("expectContains"), ExpectContains)
			&& !ExpectContains.IsEmpty() && !Captured.Contains(ExpectContains))
		{
			InContext.Message = FString::Printf(TEXT("执行 '%s' 的输出不含 '%s'：%s"),
				*Command, *ExpectContains, *Captured.Left(400));
			return StepFailed;
		}

		InContext.Message = Captured.IsEmpty()
			? FString::Printf(TEXT("已执行 '%s'（%s，没有文本输出）"), *Command, bHandled ? TEXT("命令被处理") : TEXT("没有命令认领"))
			: FString::Printf(TEXT("已执行 '%s'：%s"), *Command, *Captured.Left(400));
		return StepSucceeded;
	}

	/**
	 * 只读命中测试：这个点上最终会命中谁。
	 *
	 * 操作路径走代码接口（不经过命中测试）是刻意的取舍，代价是遮挡类缺陷检不出来——
	 * 这个动作就是补那一块的：它用引擎的 hittest grid 复现"真实鼠标会点到谁"。
	 */
	EGameTestStepStatus HandleProbe(FGameTestStepContext& InContext)
	{
		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		if (!TryResolveStepTarget(InContext, Node, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		UWidget* Target = Node->Widget.Get();
		if (Target == nullptr)
		{
			InContext.Message = TEXT("目标控件已失效（句柄过期？重新 find 一次）");
			return StepFailed;
		}

		const FJsonObject& Params = *InContext.Step->Params;
		double PointX = 0.5;
		double PointY = 0.5;
		GameTestJson::TryGetNumber(Params, TEXT("pointX"), PointX);
		GameTestJson::TryGetNumber(Params, TEXT("pointY"), PointY);

		GameTestHitTest::FResult Result;
		if (!GameTestHitTest::Probe(*Target, FVector2D(PointX, PointY), Result, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		TArray<TSharedPtr<FJsonValue>> PathValues;
		PathValues.Reserve(Result.HitPath.Num());
		for (const FString& Item : Result.HitPath)
		{
			PathValues.Add(MakeShared<FJsonValueString>(Item));
		}

		TArray<TSharedPtr<FJsonValue>> PointValues;
		PointValues.Add(MakeShared<FJsonValueNumber>(Result.Point.X));
		PointValues.Add(MakeShared<FJsonValueNumber>(Result.Point.Y));

		InContext.Payload->SetArrayField(TEXT("hitPath"), PathValues);
		InContext.Payload->SetStringField(TEXT("topWidget"), Result.TopDescription);
		InContext.Payload->SetArrayField(TEXT("point"), PointValues);

		// expect（可选）：期望命中的是**另一个**控件——"验证被遮挡"因此是一条正例，
		// 不需要给这个 driver 加一个"期望失败"机制。
		FString ExpectText;
		GameTestJson::TryGetString(Params, TEXT("expect"), ExpectText);

		bool bPassed = Result.bTargetHit;
		FString FailReason;
		if (!ExpectText.IsEmpty())
		{
			TSharedPtr<FGameTestWidgetNode> ExpectNode;
			TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
			FString ExpectError;
			if (!TryCaptureScreens(InContext.World, Screens, ExpectError)
				|| !GameTestSelector::ResolveText(Screens, ExpectText, ExpectNode, ExpectError))
			{
				InContext.Message = FString::Printf(TEXT("expect 解析失败：%s"), *ExpectError);
				return StepFailed;
			}

			const TSharedPtr<SWidget> ExpectSlate = ExpectNode->Widget.IsValid()
				? ExpectNode->Widget->GetCachedWidget()
				: nullptr;
			if (!ExpectSlate.IsValid())
			{
				InContext.Message = FString::Printf(TEXT("expect 指向的 %s 没有 Slate 控件"), *ExpectNode->Path);
				return StepFailed;
			}

			bPassed = Result.TopSlateWidget.IsValid() && Result.TopSlateWidget == ExpectSlate;
			if (!bPassed)
			{
				FailReason = FString::Printf(TEXT("期望命中 %s，实际命中 %s"),
					*ExpectNode->Path, *Result.TopDescription);
			}
		}
		else if (!bPassed)
		{
			FailReason = FString::Printf(TEXT("%s 在这个点上没有被命中（被遮挡，或该处不属于它）"), *Node->Path);
		}

		if (bPassed)
		{
			InContext.Message = FString::Printf(TEXT("命中 %s（点 %.2f, %.2f）"),
				*Result.TopDescription, Result.Point.X, Result.Point.Y);
			return StepSucceeded;
		}

		InContext.Payload->SetStringField(TEXT("occludedBy"), Result.TopDescription);
		InContext.Message = FString::Printf(TEXT("%s；实际命中的最顶层控件：%s；命中路径：%s"),
			*FailReason, *Result.TopDescription, *FString::Join(Result.HitPath, TEXT(" > ")));
		return StepFailed;
	}
}
