// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameTestCommonUIActions.h"

#include "Core/GameTestAction.h"
#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestJson.h"
#include "Observe/GameTestWidgetNode.h"
#include "Operate/GameTestOps.h"

#include "CommonActivatableWidget.h"
#include "Dom/JsonObject.h"
#include "Widgets/CommonActivatableWidgetContainer.h"

using namespace GameTestHandlers;

namespace
{
	bool PushStackWidget(
		UCommonActivatableWidgetContainerBase* InContainer,
		const FString& InClassPath,
		UCommonActivatableWidget*& OutPushedWidget,
		FString& OutMessage,
		TSharedPtr<FJsonObject>& OutPayload)
	{
		OutPushedWidget = nullptr;

		if (InContainer == nullptr)
		{
			OutMessage = TEXT("目标不是 CommonUI 的层级容器（stack.push 只能用在 UCommonActivatableWidgetContainerBase 的派生控件上）");
			return false;
		}

		UClass* WidgetClass = GameTestOps::ResolveWidgetClass(InClassPath, UCommonActivatableWidget::StaticClass(), OutMessage);
		if (WidgetClass == nullptr)
		{
			return false;
		}

		// 公开的推入接口就是基类的 AddWidget<T>（蓝图里的 Push Widget 是它的 private 包装）。
		OutPushedWidget = InContainer->AddWidget<UCommonActivatableWidget>(WidgetClass);
		if (OutPushedWidget == nullptr)
		{
			OutMessage = FString::Printf(TEXT("推入失败：%s 没能生成实例（抽象类或不是可激活控件？）"), *WidgetClass->GetName());
			return false;
		}

		OutPayload->SetStringField(TEXT("pushed"), OutPushedWidget->GetClass()->GetName());
		OutPayload->SetNumberField(TEXT("depth"), InContainer->GetNumWidgets());
		OutMessage = FString::Printf(TEXT("已推入 %s（容器里现有 %d 个界面）；等待它成为当前界面"),
			*OutPushedWidget->GetClass()->GetName(), InContainer->GetNumWidgets());
		return true;
	}

	bool PopStackWidget(
		UCommonActivatableWidgetContainerBase* InContainer,
		UCommonActivatableWidget*& OutPoppedWidget,
		FString& OutMessage,
		TSharedPtr<FJsonObject>& OutPayload)
	{
		OutPoppedWidget = nullptr;

		if (InContainer == nullptr)
		{
			OutMessage = TEXT("目标不是 CommonUI 的层级容器（stack.pop 只能用在 UCommonActivatableWidgetContainerBase 的派生控件上）");
			return false;
		}

		UCommonActivatableWidget* Active = InContainer->GetActiveWidget();
		if (Active == nullptr)
		{
			OutMessage = TEXT("栈里没有可弹出的界面（当前没有界面处于激活状态）");
			return false;
		}

		OutPoppedWidget = Active;
		OutPayload->SetStringField(TEXT("popped"), Active->GetClass()->GetName());
		InContainer->RemoveWidget(*Active);
		OutMessage = FString::Printf(TEXT("已弹出 %s（容器里还剩 %d 个界面）"),
			*Active->GetClass()->GetName(), InContainer->GetNumWidgets());
		return true;
	}

	EGameTestStepStatus HandleStackPush(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;

		FString ClassPath;
		if (!GameTestJson::TryGetString(Params, TEXT("class"), ClassPath) || ClassPath.IsEmpty())
		{
			InContext.Message = TEXT("stack.push 需要 class（可激活控件的类路径或类名，例如 GameTestProbeScreen）");
			return StepFailed;
		}

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		if (!TryResolveStepTarget(InContext, Node, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		UCommonActivatableWidgetContainerBase* Container = Cast<UCommonActivatableWidgetContainerBase>(Node->Widget.Get());
		UCommonActivatableWidget* Pushed = nullptr;
		if (!PushStackWidget(Container, ClassPath, Pushed, InContext.Message, InContext.Payload))
		{
			return StepFailed;
		}

		// 等它真的显示出来：栈类容器等"成为当前界面"，Queue 之类的入队语义只等"进入控件列表"（见 design D3）。
		const bool bIsStack = Container->IsA<UCommonActivatableWidgetStack>();
		TWeakObjectPtr<UCommonActivatableWidgetContainerBase> WeakContainer(Container);
		TWeakObjectPtr<UCommonActivatableWidget> WeakPushed(Pushed);
		InContext.PendingPredicate = [WeakContainer, WeakPushed, bIsStack](double)
		{
			UCommonActivatableWidgetContainerBase* Live = WeakContainer.Get();
			UCommonActivatableWidget* LivePushed = WeakPushed.Get();
			if (Live == nullptr || LivePushed == nullptr)
			{
				return false;
			}

			return bIsStack
				? Live->GetActiveWidget() == LivePushed
				: Live->GetWidgetList().Contains(LivePushed);
		};

		InContext.Payload->SetStringField(TEXT("path"), Node->Path);
		return StepPending;
	}

	EGameTestStepStatus HandleStackPop(FGameTestStepContext& InContext)
	{
		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		if (!TryResolveStepTarget(InContext, Node, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		UCommonActivatableWidgetContainerBase* Container = Cast<UCommonActivatableWidgetContainerBase>(Node->Widget.Get());
		UCommonActivatableWidget* Popped = nullptr;
		if (!PopStackWidget(Container, Popped, InContext.Message, InContext.Payload))
		{
			return StepFailed;
		}

		// 弹出的清理发生在切换过渡之后（WidgetList 的移除也在那时），所以等"既不在列表里、也不是当前界面"。
		TWeakObjectPtr<UCommonActivatableWidgetContainerBase> WeakContainer(Container);
		TWeakObjectPtr<UCommonActivatableWidget> WeakPopped(Popped);
		InContext.PendingPredicate = [WeakContainer, WeakPopped](double)
		{
			UCommonActivatableWidgetContainerBase* Live = WeakContainer.Get();
			if (Live == nullptr)
			{
				return false;
			}

			UCommonActivatableWidget* PoppedWidget = WeakPopped.Get();
			if (PoppedWidget == nullptr)
			{
				// 控件已回收（过渡结束后的清理）——这就是"回落完成"。
				return true;
			}

			return Live->GetActiveWidget() != PoppedWidget && !Live->GetWidgetList().Contains(PoppedWidget);
		};

		InContext.Payload->SetStringField(TEXT("path"), Node->Path);
		return StepPending;
	}
}

void GameTestCommonUIActions::Register()
{
	GameTestActions::Register({ TEXT("stack.push"), TEXT("class"), true, false,
		TEXT("把可激活控件类推进 CommonUI 层级容器，并等它成为当前界面（Queue 类容器只等进入列表）"),
		&HandleStackPush, TEXT("GameTestCommonUI") });
	GameTestActions::Register({ TEXT("stack.pop"), TEXT(""), true, false,
		TEXT("弹出 CommonUI 层级容器的当前界面，并等它回落到上一个（空栈弹出是失败）"),
		&HandleStackPop, TEXT("GameTestCommonUI") });
}

void GameTestCommonUIActions::Unregister()
{
	GameTestActions::Unregister(TEXT("stack.push"));
	GameTestActions::Unregister(TEXT("stack.pop"));
}
