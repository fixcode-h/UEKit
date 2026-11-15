// 世界大纲的变化推送：Actor 增删、挂接、换文件夹、关卡加载卸载时记下是哪个世界变了，最多每 0.5 秒推一次
// uelink.outliner 事件 {worlds: [...]}。事件不带数据，网页收到后重读 outliner（只有名字和层级，很便宜），
// 这样不用轮询，PIE 里大量生成子弹之类也只是每 0.5 秒一次。没人订阅时 PublishEvent 什么都不做
#include "AgentPanelBridge.h"
#include "AgentPanelEditorShared.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace
{
	TSet<FString> DirtyWorlds;
	bool bAllDirty = false;
	FTSTicker::FDelegateHandle Tick;
	TArray<FDelegateHandle> Hooks;

	void Mark(const AActor* Actor)
	{
		const UWorld* World = Actor ? Actor->GetWorld() : nullptr;
		if (World)
		{
			DirtyWorlds.Add(FAgentPanelBridge::WorldId(World));
		}
	}

	bool Flush(float)
	{
		if (!bAllDirty && DirtyWorlds.Num() == 0)
		{
			return true;
		}
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Worlds;
		for (const FString& W : DirtyWorlds)
		{
			Worlds.Add(MakeShared<FJsonValueString>(W));
		}
		E->SetArrayField(TEXT("worlds"), Worlds);
		// 关卡列表变了（加载卸载子关卡、开始结束 PIE）分不清是哪个世界，all 让网页都重读
		E->SetBoolField(TEXT("all"), bAllDirty);
		DirtyWorlds.Reset();
		bAllDirty = false;
		FAgentPanelBridge::Get().PublishEvent(TEXT("uelink.outliner"), E);
		return true;
	}
}

void AgentPanelEd::StartOutlinerPush()
{
	if (!GEngine || !GIsEditor || IsRunningCommandlet() || Tick.IsValid())
	{
		return;
	}
	Hooks.Add(GEngine->OnLevelActorAdded().AddLambda([](AActor* A) { Mark(A); }));
	Hooks.Add(GEngine->OnLevelActorDeleted().AddLambda([](AActor* A) { Mark(A); }));
	Hooks.Add(GEngine->OnLevelActorAttached().AddLambda([](AActor* A, const AActor*) { Mark(A); }));
	Hooks.Add(GEngine->OnLevelActorDetached().AddLambda([](AActor* A, const AActor*) { Mark(A); }));
	Hooks.Add(GEngine->OnLevelActorFolderChanged().AddLambda([](const AActor* A, FName) { Mark(A); }));
	Hooks.Add(GEngine->OnLevelActorListChanged().AddLambda([]() { bAllDirty = true; }));
	Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Flush), 0.5f);
}

void AgentPanelEd::StopOutlinerPush()
{
	if (Tick.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Tick);
		Tick.Reset();
	}
	if (GEngine)
	{
		GEngine->OnLevelActorAdded().Remove(Hooks.IsValidIndex(0) ? Hooks[0] : FDelegateHandle());
		GEngine->OnLevelActorDeleted().Remove(Hooks.IsValidIndex(1) ? Hooks[1] : FDelegateHandle());
		GEngine->OnLevelActorAttached().Remove(Hooks.IsValidIndex(2) ? Hooks[2] : FDelegateHandle());
		GEngine->OnLevelActorDetached().Remove(Hooks.IsValidIndex(3) ? Hooks[3] : FDelegateHandle());
		GEngine->OnLevelActorFolderChanged().Remove(Hooks.IsValidIndex(4) ? Hooks[4] : FDelegateHandle());
		GEngine->OnLevelActorListChanged().Remove(Hooks.IsValidIndex(5) ? Hooks[5] : FDelegateHandle());
	}
	Hooks.Reset();
}
