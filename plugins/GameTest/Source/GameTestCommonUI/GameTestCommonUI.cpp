// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameTestCommonUIActions.h"

#include "Observe/GameTestSnapshot.h"

#include "CommonActivatableWidget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"
#include "Widgets/CommonActivatableWidgetContainer.h"

/**
 * GameTest 的 CommonUI 部分：层级容器的推入 / 弹出动作，以及快照里容器和可激活控件的状态。
 *
 * 单独成模块是因为 CommonUI 不是每个项目都开：主模块不 include 它的头，
 * 这块挂在动作登记表和快照的状态读取器上，没有它时只是少了这几个动作。
 */
class FGameTestCommonUIModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		GameTestCommonUIActions::Register();
		StateReader = GameTestSnapshot::AddStateReader(&ReadState);
	}

	virtual void ShutdownModule() override
	{
		GameTestSnapshot::RemoveStateReader(StateReader);
		GameTestCommonUIActions::Unregister();
	}

private:
	static bool ReadState(const UWidget& InWidget, FJsonObject& OutState)
	{
		if (const UCommonActivatableWidgetContainerBase* Container = Cast<UCommonActivatableWidgetContainerBase>(&InWidget))
		{
			// 层级容器：当前显示谁、深度、以及栈的内容——AI 靠它确认 push / pop 生效了没有。
			const UCommonActivatableWidget* Active = Container->GetActiveWidget();
			OutState.SetStringField(TEXT("activeWidget"), Active != nullptr ? GameTestSnapshot::FriendlyClassName(Active->GetClass()) : FString());
			OutState.SetNumberField(TEXT("depth"), Container->GetNumWidgets());

			TArray<TSharedPtr<FJsonValue>> Widgets;
			for (const UCommonActivatableWidget* Widget : Container->GetWidgetList())
			{
				if (Widget != nullptr)
				{
					Widgets.Add(MakeShared<FJsonValueString>(GameTestSnapshot::FriendlyClassName(Widget->GetClass())));
				}
			}
			OutState.SetArrayField(TEXT("widgets"), Widgets);
			return true;
		}

		if (const UCommonActivatableWidget* Activatable = Cast<UCommonActivatableWidget>(&InWidget))
		{
			OutState.SetBoolField(TEXT("activated"), Activatable->IsActivated());
			return true;
		}

		return false;
	}

	FDelegateHandle StateReader;
};

IMPLEMENT_MODULE(FGameTestCommonUIModule, GameTestCommonUI)
