// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

#include "Core/Script/GameTestBuiltins.h"

#include "Blueprint/IUserListEntry.h"
#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/UserWidget.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/ListView.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/PanelWidget.h"
#include "Components/RichTextBlock.h"
#include "Components/SceneComponent.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "GameFramework/Actor.h"

bool IGameTestState::AppliesTo(const UObject* InObject) const
{
	if (InObject == nullptr)
	{
		return false;
	}
	switch (GetScope())
	{
	case EGameTestStateScope::Widget:
		return InObject->IsA<UWidget>();
	case EGameTestStateScope::Actor:
		return InObject->IsA<AActor>() || InObject->IsA<USceneComponent>();
	default:
		return true;
	}
}

TGameTestRegistry<IGameTestState>& FGameTestStateRegistry::Get()
{
	static TGameTestRegistry<IGameTestState> Registry;
	GameTestBuiltins::EnsureRegistered();
	return Registry;
}

const IGameTestState* FGameTestStateRegistry::Find(const FString& InName)
{
	return InName.IsEmpty() ? nullptr : Get().Find(FName(*InName));
}

UWidget* GameTestStateUtil::WidgetOf(UObject* InObject)
{
	return Cast<UWidget>(InObject);
}

bool GameTestStateUtil::LocationOf(const UObject* InObject, FVector& OutLocation)
{
	if (const AActor* Actor = Cast<AActor>(InObject))
	{
		OutLocation = Actor->GetActorLocation();
		return true;
	}
	if (const USceneComponent* Component = Cast<USceneComponent>(InObject))
	{
		OutLocation = Component->GetComponentLocation();
		return true;
	}
	return false;
}

FGameTestStateValue GameTestStateUtil::NotApplicable(const UObject* InObject, const TCHAR* InState)
{
	FGameTestStateValue Out;
	Out.Value = FGameTestValue::Unresolved(FString::Printf(TEXT("%s 没有 :%s"), InObject != nullptr ? *InObject->GetClass()->GetName() : TEXT("（空对象）"), InState), TEXT("prop_missing"));
	return Out;
}

FGameTestStateValue GameTestStateUtil::Of(const FGameTestValue& InValue)
{
	FGameTestStateValue Out;
	Out.Value = InValue;
	return Out;
}

UWidget* GameTestStateUtil::ParentOf(const UWidget* InWidget)
{
	if (InWidget == nullptr)
	{
		return nullptr;
	}
	if (UPanelWidget* Parent = InWidget->GetParent())
	{
		return Parent;
	}
	return InWidget->GetTypedOuter<UUserWidget>();
}

bool GameTestStateUtil::TextWidgetText(UWidget* InWidget, FText& OutText)
{
	if (const UTextBlock* Text = Cast<UTextBlock>(InWidget))
	{
		OutText = Text->GetText();
		return true;
	}
	if (const URichTextBlock* Rich = Cast<URichTextBlock>(InWidget))
	{
		OutText = Rich->GetText();
		return true;
	}
	if (const UEditableText* Editable = Cast<UEditableText>(InWidget))
	{
		OutText = Editable->GetText();
		return true;
	}
	if (const UEditableTextBox* EditableBox = Cast<UEditableTextBox>(InWidget))
	{
		OutText = EditableBox->GetText();
		return true;
	}
	if (const UMultiLineEditableTextBox* MultiLine = Cast<UMultiLineEditableTextBox>(InWidget))
	{
		OutText = MultiLine->GetText();
		return true;
	}
	// 按钮、边框这类：取里面第一个文字控件（和录制 hint.text 一样只往下找，不跨进嵌套 WBP 之外）
	if (const UPanelWidget* Panel = Cast<UPanelWidget>(InWidget))
	{
		for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
		{
			if (TextWidgetText(Panel->GetChildAt(Index), OutText))
			{
				return true;
			}
		}
	}
	return false;
}

UListView* GameTestStateUtil::OwningList(UObject* InObject, UObject*& OutItem)
{
	OutItem = nullptr;
	UUserWidget* Entry = Cast<UUserWidget>(InObject);
	if (Entry == nullptr || !Entry->GetClass()->ImplementsInterface(UUserListEntry::StaticClass()))
	{
		return nullptr;
	}
	TScriptInterface<IUserListEntry> ListEntry;
	ListEntry.SetObject(Entry);
	UListView* List = Cast<UListView>(UUserListEntryLibrary::GetOwningListView(ListEntry));
	if (List == nullptr || !Entry->GetClass()->ImplementsInterface(UUserObjectListEntry::StaticClass()))
	{
		return List;
	}
	TScriptInterface<IUserObjectListEntry> ObjectEntry;
	ObjectEntry.SetObject(Entry);
	OutItem = UUserObjectListEntryLibrary::GetListItemObject(ObjectEntry);
	return List;
}
