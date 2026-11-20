// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

namespace GameTestStateHiddenInGameImpl
{
	/** `:hiddenInGame`：Actor / 场景组件是否隐藏。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("hiddenInGame");
		}

		virtual EGameTestStateScope GetScope() const override
		{
			return EGameTestStateScope::Actor;
		}

		virtual EGameTestKind GetKind() const override
		{
			return EGameTestKind::Bool;
		}

		virtual FString GetSummary() const override
		{
			return TEXT("Actor / 场景组件是否隐藏");
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			if (const AActor* Actor = Cast<AActor>(InObject))
			{
				return GameTestStateUtil::Of(FGameTestValue::FromBool(Actor->IsHidden()));
			}
			if (const USceneComponent* Component = Cast<USceneComponent>(InObject))
			{
				// bHiddenInGame 在各版本里访问方式不同（位域 / 私有），按反射读，不碰成员
				if (const FBoolProperty* Prop = CastField<FBoolProperty>(Component->GetClass()->FindPropertyByName(TEXT("bHiddenInGame"))))
				{
					return GameTestStateUtil::Of(FGameTestValue::FromBool(Prop->GetPropertyValue_InContainer(Component)));
				}
			}
			return GameTestStateUtil::NotApplicable(InObject, TEXT("hiddenInGame"));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateHiddenInGame()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateHiddenInGameImpl::FState>());
	}
}
