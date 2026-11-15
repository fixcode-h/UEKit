// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestWidgetJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/GameTestWidgetNode.h"

TSharedPtr<FGameTestWidgetNode> FGameTestWidgetNode::FindByPath(const FString& InPath) const
{
	if (Path.Equals(InPath, ESearchCase::IgnoreCase))
	{
		return MakeShared<FGameTestWidgetNode>(*this);
	}

	for (const TSharedPtr<FGameTestWidgetNode>& Child : Children)
	{
		if (Child.IsValid())
		{
			if (TSharedPtr<FGameTestWidgetNode> Found = Child->FindByPath(InPath))
			{
				return Found;
			}
		}
	}

	return nullptr;
}

TSharedPtr<FJsonObject> GameTestWidgetJson::ToJson(
	const FGameTestWidgetNode& InNode,
	bool bInIncludeGeometry,
	bool bInIncludeProps,
	int32 InMaxNodes,
	int32& InOutNodeCount)
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), InNode.Name);
	Json->SetStringField(TEXT("class"), InNode.ClassName);
	Json->SetStringField(TEXT("path"), InNode.Path);
	Json->SetStringField(TEXT("visibility"), InNode.Visibility);

	if (!InNode.bEnabled)
	{
		Json->SetBoolField(TEXT("enabled"), false);
	}
	if (InNode.bFocused)
	{
		Json->SetBoolField(TEXT("focused"), true);
	}
	if (!InNode.Text.IsEmpty())
	{
		Json->SetStringField(TEXT("text"), InNode.Text);
	}
	if (InNode.State.IsValid() && InNode.State->Values.Num() > 0)
	{
		Json->SetObjectField(TEXT("state"), InNode.State);
	}
	if (bInIncludeProps && InNode.Props.IsValid() && InNode.Props->Values.Num() > 0)
	{
		Json->SetObjectField(TEXT("props"), InNode.Props);
	}
	if (InNode.bIsUserWidget && !InNode.NestedScreenClass.IsEmpty())
	{
		Json->SetStringField(TEXT("nestedScreen"), InNode.NestedScreenClass);
	}
	if (bInIncludeGeometry && InNode.bHasGeometry)
	{
		TArray<TSharedPtr<FJsonValue>> Position;
		Position.Add(MakeShared<FJsonValueNumber>(InNode.Position.X));
		Position.Add(MakeShared<FJsonValueNumber>(InNode.Position.Y));
		Json->SetArrayField(TEXT("pos"), Position);

		TArray<TSharedPtr<FJsonValue>> Size;
		Size.Add(MakeShared<FJsonValueNumber>(InNode.Size.X));
		Size.Add(MakeShared<FJsonValueNumber>(InNode.Size.Y));
		Json->SetArrayField(TEXT("size"), Size);
	}

	if (InNode.Items.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(InNode.Items.Num());
		for (const TSharedPtr<FGameTestListItem>& Item : InNode.Items)
		{
			if (Item.IsValid())
			{
				Items.Add(MakeShared<FJsonValueObject>(ItemToJson(*Item)));
			}
		}
		Json->SetArrayField(TEXT("items"), Items);
	}

	++InOutNodeCount;
	if (InOutNodeCount > InMaxNodes)
	{
		Json->SetBoolField(TEXT("truncated"), true);
		return Json;
	}

	if (InNode.Children.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Children;
		Children.Reserve(InNode.Children.Num());
		for (const TSharedPtr<FGameTestWidgetNode>& Child : InNode.Children)
		{
			if (!Child.IsValid())
			{
				continue;
			}

			Children.Add(MakeShared<FJsonValueObject>(ToJson(*Child, bInIncludeGeometry, bInIncludeProps, InMaxNodes, InOutNodeCount)));
			if (InOutNodeCount > InMaxNodes)
			{
				Json->SetBoolField(TEXT("truncated"), true);
				break;
			}
		}
		Json->SetArrayField(TEXT("children"), Children);
	}

	return Json;
}

TSharedPtr<FJsonObject> GameTestWidgetJson::ItemToJson(const FGameTestListItem& InItem)
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("index"), InItem.Index);
	Json->SetStringField(TEXT("class"), InItem.ClassName);
	if (InItem.bSelected)
	{
		Json->SetBoolField(TEXT("selected"), true);
	}
	if (InItem.Properties.IsValid() && InItem.Properties->Values.Num() > 0)
	{
		Json->SetObjectField(TEXT("props"), InItem.Properties);
	}
	return Json;
}
