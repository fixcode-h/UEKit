// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestActor.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"

namespace
{
	bool MatchesTag(const AActor& InActor, const FString& InTag)
	{
		for (const FName& Tag : InActor.Tags)
		{
			if (Tag.ToString().Equals(InTag, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	/** `locallyControlled` 只对 Pawn 有意义（"这台是不是本端玩家自己开的那台"）。 */
	bool MatchesLocallyControlled(const AActor& InActor, const TOptional<bool>& InLocallyControlled)
	{
		if (!InLocallyControlled.IsSet())
		{
			return true;
		}

		const APawn* Pawn = Cast<APawn>(&InActor);
		return Pawn != nullptr && Pawn->IsLocallyControlled() == InLocallyControlled.GetValue();
	}

	FString DescribeLocallyControlled(const TOptional<bool>& InLocallyControlled)
	{
		return InLocallyControlled.IsSet()
			? FString::Printf(TEXT("、locallyControlled=%s"), InLocallyControlled.GetValue() ? TEXT("true") : TEXT("false"))
			: FString();
	}

	FString DescribeActor(const AActor& InActor)
	{
		return FString::Printf(TEXT("%s(%s)"), *InActor.GetClass()->GetName(), *InActor.GetName());
	}

	void CollectCandidates(const TArray<AActor*>& InActors, FString& OutText, int32 InLimit = 12)
	{
		TArray<FString> Names;
		for (int32 Index = 0; Index < InActors.Num() && Index < InLimit; ++Index)
		{
			if (InActors[Index] != nullptr)
			{
				Names.Add(DescribeActor(*InActors[Index]));
			}
		}

		const FString More = InActors.Num() > InLimit
			? FString::Printf(TEXT(" …（共 %d 个）"), InActors.Num())
			: FString();
		OutText = FString::Printf(TEXT("%s%s"), *FString::Join(Names, TEXT("、")), *More);
	}
}

bool GameTestActor::MatchesClass(const AActor& InActor, const FString& InClassName)
{
	for (const UClass* Class = InActor.GetClass(); Class != nullptr; Class = Class->GetSuperClass())
	{
		const FString ClassName = Class->GetName();
		if (ClassName.Equals(InClassName, ESearchCase::IgnoreCase))
		{
			return true;
		}

		// 反射名剥前缀：ATank 的类名是 Tank——两种写法都认（2026-10-07 探针实测）。
		if (InClassName.Len() == ClassName.Len() + 1
			&& InClassName.RightChop(1).Equals(ClassName, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

AActor* GameTestActor::ResolveOne(
	UWorld& InWorld,
	const FString& InClassName,
	const FString& InTag,
	const TOptional<bool>& InLocallyControlled,
	FString& OutError)
{
	if (InClassName.IsEmpty() && InTag.IsEmpty())
	{
		OutError = TEXT("需要 class 或 tag 之一来选 actor");
		return nullptr;
	}

	TArray<AActor*> Matches;
	for (TActorIterator<AActor> It(&InWorld); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor == nullptr)
		{
			continue;
		}

		const bool bClassHit = InClassName.IsEmpty() || MatchesClass(*Actor, InClassName);
		const bool bTagHit = InTag.IsEmpty() || MatchesTag(*Actor, InTag);
		if (bClassHit && bTagHit && MatchesLocallyControlled(*Actor, InLocallyControlled))
		{
			Matches.Add(Actor);
		}
	}

	if (Matches.Num() == 0)
	{
		OutError = FString::Printf(TEXT("游戏世界里没有匹配的 actor（class=%s、tag=%s%s）"),
			InClassName.IsEmpty() ? TEXT("（未指定）") : *InClassName,
			InTag.IsEmpty() ? TEXT("（未指定）") : *InTag,
			*DescribeLocallyControlled(InLocallyControlled));
		return nullptr;
	}

	if (Matches.Num() > 1)
	{
		FString Candidates;
		CollectCandidates(Matches, Candidates);
		OutError = FString::Printf(
			TEXT("命中 %d 个 actor，写法不够确定（用 tag 或 locallyControlled 收窄，或先清理场景）；候选：%s"),
			Matches.Num(), *Candidates);
		return nullptr;
	}

	return Matches[0];
}

TSharedPtr<FJsonObject> GameTestActor::ToJson(const AActor& InActor)
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	Json->SetStringField(TEXT("path"), InActor.GetPathName());
	Json->SetStringField(TEXT("name"), InActor.GetName());
	Json->SetStringField(TEXT("className"), InActor.GetClass()->GetName());

	const FVector Location = InActor.GetActorLocation();
	const FRotator Rotation = InActor.GetActorRotation();
	const FVector Velocity = InActor.GetVelocity();

	const TSharedPtr<FJsonObject> LocationJson = MakeShared<FJsonObject>();
	LocationJson->SetNumberField(TEXT("x"), Location.X);
	LocationJson->SetNumberField(TEXT("y"), Location.Y);
	LocationJson->SetNumberField(TEXT("z"), Location.Z);
	Json->SetObjectField(TEXT("location"), LocationJson);

	const TSharedPtr<FJsonObject> RotationJson = MakeShared<FJsonObject>();
	RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
	RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
	RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
	Json->SetObjectField(TEXT("rotation"), RotationJson);

	// Yaw 单独摊一份：玩法上"朝哪"绝大多数时候只看它，省掉调用方从 rotator 里挑字段。
	Json->SetNumberField(TEXT("yaw"), Rotation.Yaw);

	const TSharedPtr<FJsonObject> VelocityJson = MakeShared<FJsonObject>();
	VelocityJson->SetNumberField(TEXT("x"), Velocity.X);
	VelocityJson->SetNumberField(TEXT("y"), Velocity.Y);
	VelocityJson->SetNumberField(TEXT("z"), Velocity.Z);
	Json->SetObjectField(TEXT("velocity"), VelocityJson);
	Json->SetNumberField(TEXT("speed"), Velocity.Size());

	return Json;
}
