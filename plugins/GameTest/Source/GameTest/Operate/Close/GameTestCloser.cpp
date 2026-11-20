// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/Close/GameTestCloser.h"

#include "Core/Script/GameTestBuiltins.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"
#include "Observe/Ref/GameTestIdentSuggest.h"
#include "Observe/Ref/GameTestPropPath.h"

UObject* FGameTestCloseRun::PartOrSelf()
{
	FString Part;
	How.TryGetStringField(TEXT("part"), Part);
	FString Error;
	UObject* Found = GameTestPropRead::FindPart(Object, Part, Error);
	if (Found == nullptr)
	{
		Message = FString::Printf(TEXT("part_missing：%s"), *Error);
	}
	return Found;
}

TGameTestRegistry<IGameTestCloser>& GameTestClosers::Get()
{
	static TGameTestRegistry<IGameTestCloser> Registry;
	GameTestBuiltins::EnsureRegistered();
	return Registry;
}

void GameTestClosers::ValidateHow(const TSharedPtr<FJsonValue>& InHow, FGameTestDiagSink& InSink)
{
	const TSharedPtr<FJsonObject>* How = nullptr;
	if (!InHow.IsValid() || !InHow->TryGetObject(How) || How == nullptr || !How->IsValid())
	{
		InSink.Error(TEXT("bad_type"), TEXT("关闭方式要写对象：{\"by\": \"call\", \"function\": \"Close\"}"));
		return;
	}
	FString By;
	if (!(*How)->TryGetStringField(TEXT("by"), By) || By.IsEmpty())
	{
		InSink.Error(TEXT("arg_missing"), TEXT("关闭方式要写 by（call / lua / click / visibility / remove / pop / key / destroy）"));
		return;
	}
	const IGameTestCloser* Closer = Get().Find(FName(*By));
	if (Closer == nullptr)
	{
		if (By == TEXT("pop"))
		{
			InSink.Error(TEXT("close_unavailable"), TEXT("by=pop 由 GameTestCommonUI 模块提供，它没有加载：项目里启用 CommonUI 插件后重启编辑器"));
			return;
		}
		TArray<FString> Suggestions;
		Get().Suggest(By, Suggestions);
		InSink.Error(TEXT("close_unavailable"), FString::Printf(TEXT("不认识关闭方式 %s%s"), *By,
			Suggestions.Num() > 0 ? *FString::Printf(TEXT("（是否想写 %s）"), *FString::Join(Suggestions, TEXT(" / "))) : TEXT("")));
		return;
	}
	for (const auto& Pair : (*How)->Values)
	{
		const FString Key = GAMETEST_JSON_KEY(Pair.Key);
		if (Key != TEXT("by") && !Closer->GetFields().Contains(Key))
		{
			InSink.Error(TEXT("unknown_field"), FString::Printf(TEXT("by=%s 不认识字段 %s（可用：%s）"), *By, *Key,
				Closer->GetFields().Num() > 0 ? *FString::Join(Closer->GetFields(), TEXT(", ")) : TEXT("没有")));
		}
	}
	Closer->Validate(**How, InSink);
}

TArray<TSharedPtr<FJsonObject>> GameTestClosers::DefaultHows(UObject* InObject, FGameTestFlowContext& InFlow, FString& OutVerify, FString& OutFrom)
{
	TArray<TSharedPtr<FJsonObject>> Out;
	OutVerify.Reset();
	if (InObject == nullptr)
	{
		return Out;
	}
	TSharedPtr<FJsonObject> Rule;
	const TArray<TSharedPtr<FJsonValue>>* RuleHows = nullptr;
	if (GameTestIdentSuggest::RuleFor(InObject->GetClass(), InFlow.Options.CloseRules, Rule) && Rule.IsValid()
		&& Rule->TryGetArrayField(TEXT("how"), RuleHows) && RuleHows != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Value : *RuleHows)
		{
			const TSharedPtr<FJsonObject>* How = nullptr;
			if (Value.IsValid() && Value->TryGetObject(How) && How != nullptr && How->IsValid())
			{
				Out.Add(*How);
			}
		}
		Rule->TryGetStringField(TEXT("verify"), OutVerify);
		OutFrom = TEXT("closeRules");
		return Out;
	}
	TArray<TPair<int32, TSharedPtr<FJsonObject>>> Ranked;
	for (const TSharedRef<IGameTestCloser>& Closer : Get().GetAll())
	{
		const int32 Order = Closer->GetRecommendOrder();
		if (Order == INDEX_NONE)
		{
			continue;
		}
		if (const TSharedPtr<FJsonObject> How = Closer->Recommend(InObject))
		{
			Ranked.Add(TPair<int32, TSharedPtr<FJsonObject>>(Order, How));
		}
	}
	Ranked.StableSort([](const TPair<int32, TSharedPtr<FJsonObject>>& A, const TPair<int32, TSharedPtr<FJsonObject>>& B) { return A.Key < B.Key; });
	for (const TPair<int32, TSharedPtr<FJsonObject>>& Pair : Ranked)
	{
		Out.Add(Pair.Value);
	}
	OutFrom = TEXT("recommend");
	return Out;
}
