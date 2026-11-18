// Copyright Epic Games, Inc. All Rights Reserved.

#include "EditorAct/GameTestEditorRefs.h"

#include "AgentPanelBridgeUtil.h"

#include "Components/ActorComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"

UWorld* GameTestEditorRefs::GetEditorWorld()
{
	return GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
}

bool GameTestEditorRefs::IsRecordable(UObject* InObject)
{
	if (!IsValid(InObject) || InObject->HasAnyFlags(RF_Transient))
	{
		return false;
	}
	const UPackage* Package = InObject->GetOutermost();
	return Package != nullptr && Package != GetTransientPackage() && !Package->HasAnyPackageFlags(PKG_PlayInEditor);
}

FString GameTestEditorRefs::MakeRef(UObject* InObject)
{
	if (!IsRecordable(InObject))
	{
		return FString();
	}
	const UWorld* EditorWorld = GetEditorWorld();
	if (AActor* Actor = Cast<AActor>(InObject))
	{
		if (EditorWorld != nullptr && Actor->GetWorld() == EditorWorld)
		{
			return TEXT("actor:") + Actor->GetActorLabel();
		}
	}
	if (UActorComponent* Component = Cast<UActorComponent>(InObject))
	{
		AActor* Owner = Component->GetOwner();
		if (Owner != nullptr && EditorWorld != nullptr && Owner->GetWorld() == EditorWorld)
		{
			return FString::Printf(TEXT("actor:%s/%s"), *Owner->GetActorLabel(), *Component->GetName());
		}
	}
	// 关卡里别的对象（关卡本身、世界设置）不是 Actor 也不是资源，按路径记也回放不稳，不录
	if (EditorWorld != nullptr && InObject->IsIn(EditorWorld))
	{
		return FString();
	}
	return InObject->GetPathName();
}

FString GameTestEditorRefs::Describe(UObject* InObject)
{
	if (!IsValid(InObject))
	{
		return FString();
	}
	if (AActor* Actor = Cast<AActor>(InObject))
	{
		return Actor->GetActorLabel();
	}
	if (UActorComponent* Component = Cast<UActorComponent>(InObject))
	{
		AActor* Owner = Component->GetOwner();
		return Owner != nullptr ? FString::Printf(TEXT("%s · %s"), *Owner->GetActorLabel(), *Component->GetName()) : Component->GetName();
	}
	// 类默认对象（Default__BP_Shop_C）写成蓝图名，和内容浏览器里看到的一样
	FString Name = InObject->GetName();
	if (Name.RemoveFromStart(TEXT("Default__")))
	{
		Name.RemoveFromEnd(TEXT("_C"));
	}
	return Name;
}

AActor* GameTestEditorRefs::FindActorByLabel(const FString& InLabel)
{
	UWorld* World = GetEditorWorld();
	if (World == nullptr || InLabel.IsEmpty())
	{
		return nullptr;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor != nullptr && Actor->GetActorLabel() == InLabel)
		{
			return Actor;
		}
	}
	return nullptr;
}

UObject* GameTestEditorRefs::Resolve(const FString& InRef, FString& OutError)
{
	if (InRef.IsEmpty())
	{
		OutError = TEXT("没有给对象（actor:<大纲里的名字>，或资源路径 /Game/...）");
		return nullptr;
	}
	if (!InRef.StartsWith(TEXT("actor:")))
	{
		UObject* Found = AgentPanel::FindObjectByPath(InRef, /*bLoad=*/true, OutError);
		if (Found == nullptr && OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("找不到 %s（资源被删了或改名了？）"), *InRef);
		}
		return Found;
	}

	FString Label = InRef.RightChop(6);
	FString ComponentName;
	FString Left;
	if (Label.Split(TEXT("/"), &Left, &ComponentName))
	{
		Label = Left;
	}
	AActor* Actor = FindActorByLabel(Label);
	if (Actor == nullptr)
	{
		OutError = FString::Printf(TEXT("当前关卡里没有标签是「%s」的 Actor：先打开录制时的关卡（editor.open map=…），或者前面的 editor.spawn 没成功"), *Label);
		return nullptr;
	}
	if (ComponentName.IsEmpty())
	{
		return Actor;
	}
	TInlineComponentArray<UActorComponent*> Components(Actor);
	for (UActorComponent* Component : Components)
	{
		if (Component != nullptr && Component->GetName() == ComponentName)
		{
			return Component;
		}
	}
	OutError = FString::Printf(TEXT("「%s」上没有叫 %s 的组件"), *Label, *ComponentName);
	return nullptr;
}

FString GameTestEditorRefs::ToPackageName(const FString& InPath)
{
	FString Package = InPath;
	int32 Dot = INDEX_NONE;
	if (Package.FindChar(TEXT('.'), Dot))
	{
		Package.LeftInline(Dot);
	}
	return Package;
}
