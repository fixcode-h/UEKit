// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/Close/GameTestCloser.h"

#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"

#include "GameFramework/Actor.h"

namespace GameTestCloseDestroyImpl
{
	class FCloser final : public IGameTestCloser
	{
	public:
		virtual FName GetKey() const override { return TEXT("destroy"); }
		virtual FString GetSummary() const override { return TEXT("AActor::Destroy（运行配置 allowDestroy: true 才可用）"); }
		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields;
			return Fields;
		}

		virtual bool Close(FGameTestCloseRun& InRun) const override
		{
			if (!InRun.Flow.Options.bAllowDestroy)
			{
				InRun.Message = TEXT("运行配置没打开 allowDestroy（_settings.json），不能销毁 Actor");
				return false;
			}
			AActor* Actor = Cast<AActor>(InRun.Object);
			if (Actor == nullptr)
			{
				InRun.Message = TEXT("by=destroy 只能用在 Actor 上（界面用 remove）");
				return false;
			}
			const FString Name = Actor->GetName();
			if (!Actor->Destroy())
			{
				InRun.Message = FString::Printf(TEXT("%s 不让销毁（可能是网络复制的 Actor 在客户端上，或世界正在销毁）"), *Name);
				return false;
			}
			InRun.Message = FString::Printf(TEXT("销毁了 %s"), *Name);
			return true;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCloseDestroy()
	{
		GameTestClosers::Get().Register(MakeShared<GameTestCloseDestroyImpl::FCloser>());
	}
}
