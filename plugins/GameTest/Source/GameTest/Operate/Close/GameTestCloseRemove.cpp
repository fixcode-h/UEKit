// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/Close/GameTestCloser.h"

#include "Dom/JsonObject.h"

#include "Components/Widget.h"

namespace GameTestCloseRemoveImpl
{
	class FCloser final : public IGameTestCloser
	{
	public:
		virtual FName GetKey() const override { return TEXT("remove"); }
		virtual FString GetSummary() const override { return TEXT("RemoveFromParent（界面从视口或父级移除）"); }
		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields;
			return Fields;
		}
		// 推荐里排最后：直接移除不走界面自己的关闭逻辑（音效、状态清理），能调函数、点按钮时先用那些
		virtual int32 GetRecommendOrder() const override { return 40; }

		virtual bool Close(FGameTestCloseRun& InRun) const override
		{
			UWidget* Widget = Cast<UWidget>(InRun.Object);
			if (Widget == nullptr)
			{
				InRun.Message = TEXT("by=remove 只能用在界面上（Actor 用 destroy）");
				return false;
			}
			Widget->RemoveFromParent();
			InRun.Message = FString::Printf(TEXT("把 %s 移除了"), *Widget->GetName());
			return true;
		}

		virtual TSharedPtr<FJsonObject> Recommend(UObject* InObject) const override
		{
			if (Cast<UWidget>(InObject) == nullptr)
			{
				return nullptr;
			}
			const TSharedRef<FJsonObject> How = MakeShared<FJsonObject>();
			How->SetStringField(TEXT("by"), TEXT("remove"));
			return How;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCloseRemove()
	{
		GameTestClosers::Get().Register(MakeShared<GameTestCloseRemoveImpl::FCloser>());
	}
}
