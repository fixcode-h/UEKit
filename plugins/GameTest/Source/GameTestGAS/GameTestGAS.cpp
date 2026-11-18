// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestAction.h"
#include "GameTestGASApi.h"
#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestJson.h"
#include "Observe/GameTestActor.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameplayTagContainer.h"
#include "Modules/ModuleManager.h"

using namespace GameTestHandlers;

namespace
{
	/**
	 * 按类名在游戏世界里找 actor，**必须唯一命中**（与控件选择器同一条约定，不"取第一个"）。
	 *
	 * 类名沿继承链匹配：写 `ATank` 能命中蓝图子类 `BP_Tank_C`——场景作者脑子里想的是
	 * C++ 类型，世界里站着的却是蓝图实例。
	 */
	AActor* FindSingleActorByClass(UWorld& InWorld, const FString& InClassName, FString& OutError)
	{
		TArray<AActor*> Matches;
		for (TActorIterator<AActor> It(&InWorld); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor == nullptr)
			{
				continue;
			}

			// 与 actor.get / net.compare 走同一份类名判据（含 A/U 前缀兼容），不在这里重复实现。
			if (GameTestActor::MatchesClass(*Actor, InClassName))
			{
				Matches.Add(Actor);
			}
		}

		if (Matches.Num() == 0)
		{
			OutError = FString::Printf(TEXT("游戏世界里没有 `%s` 的实例"), *InClassName);
			return nullptr;
		}

		if (Matches.Num() > 1)
		{
			FString Candidates;
			for (const AActor* Actor : Matches)
			{
				Candidates += FString::Printf(TEXT("%s；"), *Actor->GetPathName());
			}

			OutError = FString::Printf(
				TEXT("`%s` 命中 %d 个实例（必须唯一）：%s"), *InClassName, Matches.Num(), *Candidates);
			return nullptr;
		}

		return Matches[0];
	}

	/** 本地玩家当前接管的 Pawn——`class` 缺省时的目标。 */
	APawn* FindLocalPawn(UWorld& InWorld, FString& OutError)
	{
		const APlayerController* Controller = InWorld.GetFirstPlayerController();
		APawn* Pawn = Controller != nullptr ? Controller->GetPawn() : nullptr;
		if (Pawn == nullptr)
		{
			OutError = TEXT("本地没有可控 Pawn——PIE 起了吗？玩家接管了吗？");
		}
		return Pawn;
	}

	/** 取 Actor 的技能系统组件：走引擎接口，所以不认识 游戏模块 的类型也能拿到。 */
	UAbilitySystemComponent* FindAbilitySystemComponent(AActor& InActor)
	{
		if (IAbilitySystemInterface* Interface = Cast<IAbilitySystemInterface>(&InActor))
		{
			return Interface->GetAbilitySystemComponent();
		}
		return nullptr;
	}

	/**
	 * 按名字调项目的输入标签入口（`AbilityInputTagPressed` / `AbilityInputTagReleased`）。
	 *
	 * **为什么是反射**：那两个方法属于 游戏模块 模块，而本插件刻意不依赖它（见 Build.cs）。
	 * 它们标了 `BlueprintCallable`，所以按 `UFunction` 调用与蓝图调用走的是同一条路——不是
	 * 绕过封装，而是站在项目给蓝图留的那个口子上。
	 */
	bool InvokeAbilityInputTag(
		UAbilitySystemComponent& InASC,
		const TCHAR* InFunctionName,
		const FGameplayTag& InTag,
		FString& OutError)
	{
		UFunction* Function = InASC.FindFunction(FName(InFunctionName));
		if (Function == nullptr)
		{
			OutError = FString::Printf(
				TEXT("目标 ASC 上没有 `%s`——本动作按名字调项目的输入标签入口"), InFunctionName);
			return false;
		}

		// 参数布局照 `UFUNCTION` 的签名：单个 `FGameplayTag` 入参。
		struct FAbilityInputTagParams
		{
			FGameplayTag InputTag;
		};

		FAbilityInputTagParams Params;
		Params.InputTag = InTag;
		InASC.ProcessEvent(Function, &Params);
		return true;
	}
}

namespace
{
	EGameTestStepStatus HandleAbilityInput(FGameTestStepContext& InContext)
	{
		FString TagName;
		if (!GameTestJson::TryGetString(*InContext.Step->Params, TEXT("tag"), TagName) || TagName.IsEmpty())
		{
			InContext.Message = TEXT("ability.input 需要 tag（输入标签的完整名，例如 InputTag.Fire）");
			return StepFailed;
		}

		FString Phase;
		GameTestJson::TryGetString(*InContext.Step->Params, TEXT("phase"), Phase);
		const bool bRelease = Phase.Equals(TEXT("release"), ESearchCase::IgnoreCase);

		// 第二个参数是"找不到就报错"；这里自己报，好把名字该长什么样写进消息。
		const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound=*/false);
		if (!Tag.IsValid())
		{
			InContext.Message = FString::Printf(
				TEXT("输入标签 `%s` 没有注册（要用完整名，例如 InputTag.Fire）"), *TagName);
			return StepFailed;
		}

		if (InContext.World == nullptr)
		{
			InContext.Message = TEXT("当前没有正在跑的游戏世界——先 pie.start 并等它就绪");
			return StepFailed;
		}

		FString Error;
		AActor* Target = nullptr;

		FString ClassName;
		if (GameTestJson::TryGetString(*InContext.Step->Params, TEXT("class"), ClassName) && !ClassName.IsEmpty())
		{
			Target = FindSingleActorByClass(*InContext.World, ClassName, Error);
		}
		else
		{
			Target = FindLocalPawn(*InContext.World, Error);
		}

		if (Target == nullptr)
		{
			InContext.Message = Error;
			return StepFailed;
		}

		UAbilitySystemComponent* ASC = FindAbilitySystemComponent(*Target);
		if (ASC == nullptr)
		{
			InContext.Message = FString::Printf(
				TEXT("`%s` 上没有技能系统组件（还没被接管或没初始化）"), *Target->GetName());
			return StepFailed;
		}

		const TCHAR* FunctionName = bRelease ? TEXT("AbilityInputTagReleased") : TEXT("AbilityInputTagPressed");
		if (!InvokeAbilityInputTag(*ASC, FunctionName, Tag, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		InContext.Payload->SetStringField(TEXT("actor"), Target->GetPathName());
		InContext.Payload->SetStringField(TEXT("tag"), Tag.ToString());
		InContext.Payload->SetStringField(TEXT("phase"), bRelease ? TEXT("release") : TEXT("press"));
		InContext.Message = FString::Printf(TEXT("已对 %s 发出 `%s`（%s）"),
			*Target->GetName(), *Tag.ToString(), bRelease ? TEXT("松开") : TEXT("按下"));
		return StepSucceeded;
	}
}

/**
 * GameTest 的 GAS 部分：`ability.input`（对目标的技能系统组件发输入标签）。
 *
 * 单独成模块是因为 GameplayAbilities 不是每个项目都开：主模块不依赖它，没有它时只少这一个动作。
 */
class FGameTestGASModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		GameTestGASApi::Register();
		GameTestActions::Register({ TEXT("ability.input"), TEXT("tag,phase,class"), false, false,
			TEXT("对游戏世界里的目标发出一次技能输入标签：默认本地玩家 Pawn，可用 class 指定（沿继承链匹配、必须唯一）；缺省按下，phase=release 发松开"),
			&HandleAbilityInput, TEXT("GameTestGAS") });
	}

	virtual void ShutdownModule() override
	{
		GameTestGASApi::Unregister();
		GameTestActions::Unregister(TEXT("ability.input"));
	}
};

IMPLEMENT_MODULE(FGameTestGASModule, GameTestGAS)
