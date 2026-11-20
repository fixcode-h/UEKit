// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/Close/GameTestCloser.h"

#include "Dom/JsonObject.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Operate/GameTestCodeClick.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/Widget.h"
#include "Layout/Geometry.h"

namespace GameTestCloseClickImpl
{
	/** 推荐时认的按钮名（5.2）：BtnClose、CloseBtn、Close…… */
	bool LooksLikeCloseButton(const FString& InName)
	{
		return InName.Contains(TEXT("Close"), ESearchCase::IgnoreCase);
	}

	class FCloser final : public IGameTestCloser
	{
	public:
		virtual FName GetKey() const override { return TEXT("click"); }
		virtual FString GetSummary() const override { return TEXT("点部件（不动真实鼠标，同 click via=code）"); }
		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("part") };
			return Fields;
		}
		virtual int32 GetRecommendOrder() const override { return 20; }

		virtual bool Close(FGameTestCloseRun& InRun) const override
		{
			UWidget* Widget = Cast<UWidget>(InRun.PartOrSelf());
			if (Widget == nullptr)
			{
				if (InRun.Message.IsEmpty())
				{
					InRun.Message = TEXT("by=click 只能点界面控件");
				}
				return false;
			}
			const TSharedPtr<SWidget> Slate = Widget->GetCachedWidget();
			if (!Slate.IsValid() || !Widget->IsVisible() || !Widget->GetIsEnabled())
			{
				InRun.Message = FString::Printf(TEXT("%s 现在点不了（没显示、隐藏或禁用）"), *Widget->GetName());
				return false;
			}
			const FGeometry& Geometry = Widget->GetCachedGeometry();
			const FVector2D Center = Geometry.GetAbsolutePositionAtCoordinates(FVector2D(0.5, 0.5));
			FJsonObject Payload;
			FString Message;
			if (!GameTestCodeClick::Click(Slate, Center, Payload, Message))
			{
				InRun.Message = Message;
				return false;
			}
			InRun.Message = FString::Printf(TEXT("点了 %s"), *Widget->GetName());
			return true;
		}

		virtual TSharedPtr<FJsonObject> Recommend(UObject* InObject) const override
		{
			UUserWidget* User = Cast<UUserWidget>(InObject);
			if (User == nullptr || User->WidgetTree == nullptr)
			{
				return nullptr;
			}
			// 只看这个类自己的树（嵌套 WBP 里的关闭按钮属于那个子界面）
			TArray<UWidget*> Widgets;
			User->WidgetTree->GetAllWidgets(Widgets);
			for (UWidget* Widget : Widgets)
			{
				if (Cast<UButton>(Widget) != nullptr && LooksLikeCloseButton(Widget->GetName()))
				{
					FString Part;
					if (GameTestPropRead::PartPathOf(Widget, User, Part))
					{
						const TSharedRef<FJsonObject> How = MakeShared<FJsonObject>();
						How->SetStringField(TEXT("by"), TEXT("click"));
						How->SetStringField(TEXT("part"), Part);
						return How;
					}
				}
			}
			return nullptr;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCloseClick()
	{
		GameTestClosers::Get().Register(MakeShared<GameTestCloseClickImpl::FCloser>());
	}
}
