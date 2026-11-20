// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestRefFor.h"

#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Ref/GameTestIdentSuggest.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRefResolve.h"
#include "Observe/Ref/GameTestState.h"

#include "Blueprint/UserWidget.h"
#include "Components/ListView.h"
#include "Components/Widget.h"
#include "GameFramework/Actor.h"

namespace GameTestRefForImpl
{
	/** 条目套条目（列表里的条目里又有列表）最多往外这么多层。 */
	constexpr int32 MaxDepth = 4;

	FString ClassNameOf(const UObject* InObject)
	{
		FString Name = InObject->GetClass()->GetName();
		Name.RemoveFromEnd(TEXT("_C"));
		return Name;
	}

	bool Build(UObject* InObject, FGameTestFlowContext& InContext, FGameTestRefForResult& OutResult, int32 InDepth)
	{
		if (InObject == nullptr || InDepth > MaxDepth)
		{
			return false;
		}
		FGameTestRef Ref;
		Ref.Class = ClassNameOf(InObject);
		// 列表条目：in = {obj: 列表所在界面的引用, part: 列表}
		UObject* Item = nullptr;
		if (UListView* List = GameTestStateUtil::OwningList(InObject, Item))
		{
			UUserWidget* ListOwner = GameTestRefFor::OwnerOf(List);
			FString ListPart;
			FGameTestRefForResult Outer;
			if (ListOwner != nullptr && GameTestPropRead::PartPathOf(List, ListOwner, ListPart) && Build(ListOwner, InContext, Outer, InDepth + 1) && Outer.Ref.IsValid())
			{
				Ref.In = EGameTestRefIn::Container;
				Ref.InObj = MakeShared<FJsonValueObject>(Outer.Ref);
				Ref.InPart = ListPart;
			}
		}
		TArray<FGameTestRefCandidate> Candidates;
		FString Error;
		FName Code;
		GameTestRefResolve::Collect(Ref, InContext, Candidates, Error, Code);
		FGameTestRefCandidate Target;
		Target.Object = InObject;
		Target.Item = Item != nullptr ? Item : GameTestPropRead::ListItemOf(InObject);
		for (const FGameTestRefCandidate& Candidate : Candidates)
		{
			if (Candidate.Object.Get() == InObject)
			{
				Target = Candidate;
				break;
			}
		}
		OutResult.Siblings = Candidates.Num();
		bool bPicked = true;
		if (Candidates.Num() > 1 && !GameTestIdentSuggest::FromRule(Target, InContext, Ref.Ident))
		{
			bPicked = GameTestIdentSuggest::Pick(Target, Candidates, InContext, Ref.Ident);
		}
		OutResult.Matches = 0;
		for (const FGameTestRefCandidate& Candidate : Candidates)
		{
			if (Ref.Ident.Num() == 0 || GameTestRefResolve::MatchesIdent(Candidate, Ref.Ident, InContext))
			{
				++OutResult.Matches;
			}
		}
		if (Candidates.Num() == 0)
		{
			// 收不到（不在屏上的界面、in 推不出来）：只按类写，面板再改
			OutResult.Matches = 1;
		}
		OutResult.bAsk = !bPicked || OutResult.Matches > 1;
		OutResult.Object = InObject;
		OutResult.Ident = Ref.Ident;
		OutResult.Ref = Ref.ToJson();
		return true;
	}

	FString KindOf(const UWidget* InWidget)
	{
		UObject* Item = nullptr;
		if (GameTestStateUtil::OwningList(const_cast<UWidget*>(InWidget), Item) != nullptr)
		{
			return TEXT("entry");
		}
		return TEXT("widget");
	}
}

UUserWidget* GameTestRefFor::OwnerOf(const UWidget* InWidget)
{
	return InWidget != nullptr ? InWidget->GetTypedOuter<UUserWidget>() : nullptr;
}

bool GameTestRefFor::ForObject(UObject* InObject, FGameTestFlowContext& InContext, FGameTestRefForResult& OutResult)
{
	OutResult = FGameTestRefForResult();
	return GameTestRefForImpl::Build(InObject, InContext, OutResult, 0);
}

bool GameTestRefFor::ForWidget(UWidget* InWidget, FGameTestFlowContext& InContext, FGameTestRefForResult& OutResult, int32 InLevel)
{
	OutResult = FGameTestRefForResult();
	if (InWidget == nullptr)
	{
		return false;
	}
	// 对象：自己是 UUserWidget 就是自己，否则最近的那个；InLevel 再往外几层
	UUserWidget* Owner = Cast<UUserWidget>(InWidget);
	if (Owner == nullptr)
	{
		Owner = OwnerOf(InWidget);
	}
	for (int32 Level = 1; Level < InLevel && Owner != nullptr; ++Level)
	{
		UUserWidget* Outer = OwnerOf(Owner);
		if (Outer == nullptr)
		{
			break;
		}
		Owner = Outer;
	}
	if (Owner == nullptr)
	{
		return false;
	}
	if (!GameTestRefForImpl::Build(Owner, InContext, OutResult, 0))
	{
		return false;
	}
	// part：点到的控件在对象类树里的写法；运行时生成的（写不出名字）往外找能写的
	for (const UWidget* Current = InWidget; Current != nullptr && Current != Owner; Current = GameTestStateUtil::ParentOf(Current))
	{
		FString Part;
		if (GameTestPropRead::PartPathOf(Current, Owner, Part) && !Part.IsEmpty())
		{
			OutResult.Part = Part;
			const TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
			Target->SetObjectField(TEXT("obj"), OutResult.Ref);
			Target->SetStringField(TEXT("part"), Part);
			OutResult.Target = Target;
			break;
		}
	}
	return true;
}

void GameTestRefFor::Chain(UWidget* InWidget, TArray<TSharedPtr<FJsonObject>>& OutChain, TArray<UWidget*>* OutWidgets)
{
	UWidget* Current = InWidget;
	for (int32 Guard = 0; Current != nullptr && Guard < 8; ++Guard)
	{
		UUserWidget* Owner = Cast<UUserWidget>(Current) != nullptr ? Cast<UUserWidget>(Current) : OwnerOf(Current);
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("class"), GameTestRefForImpl::ClassNameOf(Current));
		if (Current != Owner && Owner != nullptr)
		{
			FString Part;
			Entry->SetStringField(TEXT("kind"), TEXT("widget"));
			if (GameTestPropRead::PartPathOf(Current, Owner, Part))
			{
				Entry->SetStringField(TEXT("part"), Part);
			}
			Entry->SetStringField(TEXT("note"), FString::Printf(TEXT("%s 里的控件"), *GameTestRefForImpl::ClassNameOf(Owner)));
		}
		else
		{
			const FString Kind = GameTestRefForImpl::KindOf(Current);
			Entry->SetStringField(TEXT("kind"), Kind);
			Entry->SetStringField(TEXT("note"), Kind == TEXT("entry") ? TEXT("列表条目") : TEXT("界面"));
		}
		OutChain.Add(Entry);
		if (OutWidgets != nullptr)
		{
			OutWidgets->Add(Current);
		}
		// 下一层：控件 → 它所在的界面；界面 → 外层界面
		Current = Current != Owner ? static_cast<UWidget*>(Owner) : static_cast<UWidget*>(OwnerOf(Owner));
	}
}

TSharedRef<FJsonObject> GameTestRefFor::ToJson(UWidget* InWidget, FGameTestFlowContext& InContext, int32 InLevel, TFunction<FString(const UWidget*)> InNodeId)
{
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonObject>> Chain;
	TArray<UWidget*> Widgets;
	GameTestRefFor::Chain(InWidget, Chain, &Widgets);
	TArray<TSharedPtr<FJsonValue>> ChainJson;
	for (int32 Index = 0; Index < Chain.Num(); ++Index)
	{
		const FString Id = InNodeId ? InNodeId(Widgets[Index]) : FString();
		if (!Id.IsEmpty())
		{
			Chain[Index]->SetStringField(TEXT("node"), Id);
		}
		ChainJson.Add(MakeShared<FJsonValueObject>(Chain[Index]));
	}
	Out->SetArrayField(TEXT("chain"), ChainJson);
	FGameTestRefForResult Result;
	if (ForWidget(InWidget, InContext, Result, InLevel) && Result.Ref.IsValid())
	{
		Out->SetObjectField(TEXT("ref"), Result.Ref);
		if (Result.Target.IsValid())
		{
			Out->SetObjectField(TEXT("target"), Result.Target);
		}
		Out->SetNumberField(TEXT("matches"), Result.Matches);
		Out->SetBoolField(TEXT("ask"), Result.bAsk);
	}
	return Out;
}
