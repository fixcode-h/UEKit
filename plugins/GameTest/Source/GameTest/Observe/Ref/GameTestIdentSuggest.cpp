// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestIdentSuggest.h"

#include "Core/Script/GameTestFlowContext.h"
#include "Observe/GameTestItemKey.h"
#include "Observe/Ref/GameTestRefResolve.h"
#include "Observe/Sources/GameTestObjectRoots.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/RichTextBlock.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace GameTestIdentSuggestImpl
{
	/** 一个来源的字段铺平成叶子，逐个按属性路径读（kind / 类型 / 来源以读到的为准）。 */
	void AddLeaves(const TSharedPtr<FJsonObject>& InFields, const FString& InPrefix, const FString& InGroup, const FString& InSource, UObject* InObject, UObject* InItem, FGameTestFlowContext& InContext, TArray<FGameTestIdentProp>& OutProps)
	{
		if (!InFields.IsValid())
		{
			return;
		}
		TArray<TPair<FString, TSharedPtr<FJsonValue>>> Leaves;
		GameTestItemKey::Flatten(*InFields, 3, GameTestItemKey::MaxFieldsPerSource, Leaves);
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Leaf : Leaves)
		{
			if (!Leaf.Value.IsValid())
			{
				continue;
			}
			FGameTestIdentProp Prop;
			Prop.Prop = InPrefix + Leaf.Key;
			Prop.Name = Leaf.Key;
			Prop.Group = InGroup;
			Prop.Read = GameTestPropRead::ReadText(InObject, Prop.Prop, InContext, InItem);
			Prop.Score = GameTestItemKey::Score(InSource, Leaf.Key, *Leaf.Value, /*bInUnique=*/false, /*bInEqualsIndex=*/false, /*bInChanged=*/false, /*bInLocalizedText=*/false, Prop.Why);
			OutProps.Add(Prop);
		}
	}

	/** 文字部件（设计器起了名字的 TextBlock / RichTextBlock）的 :text 和 :textKey。 */
	void AddTextParts(UUserWidget* InWidget, FGameTestFlowContext& InContext, TArray<FGameTestIdentProp>& OutProps)
	{
		if (InWidget == nullptr || InWidget->WidgetTree == nullptr)
		{
			return;
		}
		TArray<UWidget*> Texts;
		InWidget->WidgetTree->ForEachWidget([&Texts](UWidget* Widget)
		{
			if (Texts.Num() < 30 && (Widget->IsA<UTextBlock>() || Widget->IsA<URichTextBlock>()))
			{
				Texts.Add(Widget);
			}
		});
		for (UWidget* Text : Texts)
		{
			const FString Part = Text->GetName();
			for (const TCHAR* State : { TEXT(":textKey"), TEXT(":text") })
			{
				FGameTestIdentProp Prop;
				Prop.Prop = FString::Printf(TEXT("@/%s%s"), *Part, State);
				Prop.Name = Part + State;
				Prop.Group = TEXT("part:") + Part;
				Prop.Read = GameTestPropRead::ReadText(InWidget, Prop.Prop, InContext);
				if (!Prop.Read.IsResolved())
				{
					continue;
				}
				const FJsonValueString Value(Prop.Read.Value.GetString());
				// 本地化键换语言也不变，比显示的字稳；长文字（说明、描述）不适合当标识
				Prop.Score = GameTestItemKey::Score(TEXT("text"), Part, Value, false, false, false, FCString::Strcmp(State, TEXT(":text")) == 0 && !Prop.Read.TextKey.IsEmpty(), Prop.Why)
					+ (FCString::Strcmp(State, TEXT(":textKey")) == 0 ? 2.0 : 0.0);
				OutProps.Add(Prop);
			}
		}
	}

	/** 规则匹配的距离：InClass 往上第几层还认这个名字（越小越近）；不认返回 -1。 */
	int32 RuleDistance(const UClass* InClass, const FString& InRuleClass)
	{
		int32 Last = -1;
		int32 Depth = 0;
		for (const UClass* Class = InClass; Class != nullptr; Class = Class->GetSuperClass(), ++Depth)
		{
			if (!GameTestObjectRoots::ClassMatches(Class, InRuleClass))
			{
				break;
			}
			Last = Depth;
		}
		return Last;
	}
}

int32 GameTestIdentSuggest::Score10(double InScore)
{
	// GameTestItemKey::Score 的口径：5 分以上算「够准」，负分是不建议；映射到 0–10
	return FMath::Clamp(FMath::RoundToInt(InScore + 3.0), 0, 10);
}

FGameTestIdentItem GameTestIdentSuggest::ItemFor(const FString& InProp, const FGameTestPropRead& InRead)
{
	FGameTestIdentItem Item;
	Item.Prop = InProp;
	Item.Op = InRead.Kind == EGameTestKind::Bool || InRead.Kind == EGameTestKind::Enum || InRead.Kind == EGameTestKind::Visibility ? TEXT("is") : TEXT("eq");
	Item.Value = InRead.Value.ToJson();
	if (!Item.Value.IsValid())
	{
		Item.Value = MakeShared<FJsonValueNull>();
	}
	return Item;
}

void GameTestIdentSuggest::Collect(UObject* InObject, UObject* InItem, FGameTestFlowContext& InContext, TArray<FGameTestIdentProp>& OutProps)
{
	using namespace GameTestIdentSuggestImpl;
	FGameTestKeyCache Cache;
	if (InObject != nullptr)
	{
		AddLeaves(GameTestItemKey::DumpFields(InObject, Cache), TEXT("@."), TEXT("self"), TEXT("self"), InObject, nullptr, InContext, OutProps);
		AddLeaves(GameTestItemKey::DumpLuaFields(InObject, Cache), TEXT("@#"), TEXT("lua"), TEXT("lua"), InObject, nullptr, InContext, OutProps);
		AddTextParts(Cast<UUserWidget>(InObject), InContext, OutProps);
	}
	if (InItem != nullptr)
	{
		AddLeaves(GameTestItemKey::DumpFields(InItem, Cache), TEXT("@item."), TEXT("item"), TEXT("item"), InObject, InItem, InContext, OutProps);
		AddLeaves(GameTestItemKey::DumpLuaFields(InItem, Cache), TEXT("@item#"), TEXT("itemLua"), TEXT("itemLua"), InObject, InItem, InContext, OutProps);
	}
	OutProps.RemoveAll([](const FGameTestIdentProp& Prop) { return !Prop.Read.IsResolved() || !FGameTestPropKind::IsComparable(Prop.Read.Kind); });
}

bool GameTestIdentSuggest::Pick(const FGameTestRefCandidate& InTarget, const TArray<FGameTestRefCandidate>& InSiblings, FGameTestFlowContext& InContext, TArray<FGameTestIdentItem>& OutIdent, int32 InMax)
{
	OutIdent.Reset();
	TArray<const FGameTestRefCandidate*> Remaining;
	for (const FGameTestRefCandidate& Sibling : InSiblings)
	{
		const bool bSelf = Sibling.Object == InTarget.Object && Sibling.Item == InTarget.Item;
		if (!bSelf)
		{
			Remaining.Add(&Sibling);
		}
	}
	if (Remaining.Num() == 0)
	{
		return true;
	}
	TArray<FGameTestIdentProp> Props;
	Collect(InTarget.Object.Get(), InTarget.Item.Get(), InContext, Props);
	Props.Sort([](const FGameTestIdentProp& A, const FGameTestIdentProp& B) { return A.Score > B.Score; });
	// 只看前面一批（每个候选都要逐个兄弟读一遍，太多会卡一帧）
	if (Props.Num() > 40)
	{
		Props.SetNum(40);
	}
	while (Remaining.Num() > 0 && OutIdent.Num() < InMax)
	{
		int32 BestIndex = INDEX_NONE;
		TArray<const FGameTestRefCandidate*> BestLeft;
		for (int32 Index = 0; Index < Props.Num(); ++Index)
		{
			const FGameTestIdentProp& Prop = Props[Index];
			if (Prop.Score < 0.0)
			{
				continue;
			}
			TArray<const FGameTestRefCandidate*> Left;
			for (const FGameTestRefCandidate* Sibling : Remaining)
			{
				const FGameTestPropRead Read = GameTestPropRead::ReadText(Sibling->Object.Get(), Prop.Prop, InContext, Sibling->Item.Get());
				if (Read.IsResolved() && Read.Value.LooseEquals(Prop.Read.Value))
				{
					Left.Add(Sibling);
				}
			}
			if (Left.Num() < Remaining.Num() && (BestIndex == INDEX_NONE || Left.Num() < BestLeft.Num()))
			{
				BestIndex = Index;
				BestLeft = Left;
				if (Left.Num() == 0)
				{
					break;
				}
			}
		}
		if (BestIndex == INDEX_NONE)
		{
			break;
		}
		OutIdent.Add(ItemFor(Props[BestIndex].Prop, Props[BestIndex].Read));
		Props.RemoveAt(BestIndex);
		Remaining = BestLeft;
	}
	return Remaining.Num() == 0;
}

bool GameTestIdentSuggest::RuleFor(const UClass* InClass, const TSharedPtr<FJsonValue>& InRules, TSharedPtr<FJsonObject>& OutRule)
{
	OutRule.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Rules = nullptr;
	if (InClass == nullptr || !InRules.IsValid() || !InRules->TryGetArray(Rules) || Rules == nullptr)
	{
		return false;
	}
	int32 Best = MAX_int32;
	for (const TSharedPtr<FJsonValue>& Value : *Rules)
	{
		const TSharedPtr<FJsonObject>* Rule = nullptr;
		FString Class;
		if (!Value.IsValid() || !Value->TryGetObject(Rule) || Rule == nullptr || !Rule->IsValid() || !(*Rule)->TryGetStringField(TEXT("class"), Class))
		{
			continue;
		}
		const int32 Distance = GameTestIdentSuggestImpl::RuleDistance(InClass, Class);
		if (Distance >= 0 && Distance < Best)
		{
			Best = Distance;
			OutRule = *Rule;
		}
	}
	return OutRule.IsValid();
}

bool GameTestIdentSuggest::FromRule(const FGameTestRefCandidate& InTarget, FGameTestFlowContext& InContext, TArray<FGameTestIdentItem>& OutIdent)
{
	OutIdent.Reset();
	const UObject* Object = InTarget.Object.Get();
	TSharedPtr<FJsonObject> Rule;
	if (Object == nullptr || !RuleFor(Object->GetClass(), InContext.Options.IdentRules, Rule))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Props = nullptr;
	if (!Rule->TryGetArrayField(TEXT("ident"), Props) || Props == nullptr)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Props)
	{
		FString Prop;
		if (Value.IsValid() && Value->TryGetString(Prop) && !Prop.IsEmpty())
		{
			const FGameTestPropRead Read = GameTestPropRead::ReadText(InTarget.Object.Get(), Prop, InContext, InTarget.Item.Get());
			if (Read.IsResolved())
			{
				OutIdent.Add(ItemFor(Prop, Read));
			}
		}
	}
	return OutIdent.Num() > 0;
}
