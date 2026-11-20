// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameTestClosePop.h"

#include "Operate/Close/GameTestCloser.h"

#include "CommonActivatableWidget.h"
#include "Dom/JsonObject.h"

/**
 * obj.close 的 `by: "pop"`（docs/game-test-ref.md 5.2）：CommonUI 可激活控件调 DeactivateWidget。
 * 在层级容器（Stack / Queue）里的界面被停用后由容器自己移除，等于弹出；不在容器里的只是停用（核对用 verify 看）。
 * 放在这个模块里是因为主模块不 include CommonUI 的头（项目不一定开 CommonUI）。
 */
namespace GameTestClosePopImpl
{
	class FCloser final : public IGameTestCloser
	{
	public:
		virtual FName GetKey() const override { return TEXT("pop"); }
		virtual FString GetSummary() const override { return TEXT("CommonUI：DeactivateWidget，在层级容器里时由容器弹出"); }
		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields;
			return Fields;
		}
		// 推荐顺序在 click 之后、remove 之前（5.2）
		virtual int32 GetRecommendOrder() const override { return 30; }

		virtual bool Close(FGameTestCloseRun& InRun) const override
		{
			UCommonActivatableWidget* Widget = Cast<UCommonActivatableWidget>(InRun.Object);
			if (Widget == nullptr)
			{
				InRun.Message = TEXT("by=pop 只能用在 CommonUI 可激活控件（UCommonActivatableWidget）上");
				return false;
			}
			if (!Widget->IsActivated())
			{
				InRun.Message = FString::Printf(TEXT("%s 没有激活，pop 不了"), *Widget->GetName());
				return false;
			}
			Widget->DeactivateWidget();
			InRun.Message = FString::Printf(TEXT("停用了 %s"), *Widget->GetName());
			return true;
		}

		virtual TSharedPtr<FJsonObject> Recommend(UObject* InObject) const override
		{
			const UCommonActivatableWidget* Widget = Cast<UCommonActivatableWidget>(InObject);
			if (Widget == nullptr || !Widget->IsActivated())
			{
				return nullptr;
			}
			const TSharedRef<FJsonObject> How = MakeShared<FJsonObject>();
			How->SetStringField(TEXT("by"), TEXT("pop"));
			return How;
		}
	};
}

void GameTestClosePop::Register()
{
	GameTestClosers::Get().Register(MakeShared<GameTestClosePopImpl::FCloser>());
}

void GameTestClosePop::Unregister()
{
	GameTestClosers::Get().Unregister(TEXT("pop"));
}
