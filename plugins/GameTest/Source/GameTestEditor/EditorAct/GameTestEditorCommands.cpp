// Copyright Epic Games, Inc. All Rights Reserved.

#include "EditorAct/GameTestEditorCommands.h"

#include "AgentPanelBridgeUtil.h"

#include "Editor.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/InputChord.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Framework/Commands/UICommandList.h"
#include "InputCoreTypes.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"

namespace GameTestEditorCommandsDetail
{
	struct FEditorEntry
	{
		UObject* Asset = nullptr;
		IAssetEditorInstance* Instance = nullptr;
		double ActivatedAt = 0.0;
	};

	UAssetEditorSubsystem* Editors()
	{
		return GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	}

	/** 开着的资源编辑器，最近激活的在前。 */
	TArray<FEditorEntry> OpenEditors()
	{
		TArray<FEditorEntry> Out;
		UAssetEditorSubsystem* Subsystem = Editors();
		if (Subsystem == nullptr)
		{
			return Out;
		}
		for (UObject* Asset : Subsystem->GetAllEditedAssets())
		{
			IAssetEditorInstance* Instance = Asset != nullptr ? Subsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false) : nullptr;
			if (Instance == nullptr || Instance->GetEditorName().IsNone())
			{
				continue;
			}
			FEditorEntry Entry;
			Entry.Asset = Asset;
			Entry.Instance = Instance;
			Entry.ActivatedAt = Instance->GetLastActivationTime();
			Out.Add(Entry);
		}
		Out.Sort([](const FEditorEntry& A, const FEditorEntry& B)
		{
			return A.ActivatedAt > B.ActivatedAt;
		});
		return Out;
	}

	/**
	 * 资源编辑器的命令表。引擎的资源编辑器都派生自 FAssetEditorToolkit（新式的 UAssetEditor 也是经 FBaseAssetToolkit），
	 * IAssetEditorInstance 上没有取命令表的接口，只能转回去；UELink 的蓝图选中同步也是这么转的。
	 */
	TSharedPtr<FUICommandList> CommandsOf(IAssetEditorInstance* InInstance)
	{
		if (InInstance == nullptr)
		{
			return nullptr;
		}
		FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(InInstance);
		return Toolkit->GetToolkitCommands();
	}

	TSharedPtr<FUICommandList> LevelEditorCommands()
	{
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
		{
			return nullptr;
		}
		FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		return LevelEditor.GetGlobalLevelEditorActions();
	}

	/** 按顺序列出要试的命令表：指定资源的编辑器 → 其他开着的资源编辑器 → 关卡编辑器。 */
	TArray<TSharedPtr<FUICommandList>> CandidateLists(UObject* InAsset)
	{
		TArray<TSharedPtr<FUICommandList>> Lists;
		for (const FEditorEntry& Entry : OpenEditors())
		{
			const TSharedPtr<FUICommandList> List = CommandsOf(Entry.Instance);
			if (!List.IsValid())
			{
				continue;
			}
			if (InAsset != nullptr && Entry.Asset == InAsset)
			{
				Lists.Insert(List, 0);
			}
			else
			{
				Lists.Add(List);
			}
		}
		const TSharedPtr<FUICommandList> Level = LevelEditorCommands();
		if (Level.IsValid())
		{
			Lists.Add(Level);
		}
		return Lists;
	}

	bool IsMappedAnywhere(const TSharedPtr<FUICommandInfo>& InInfo, const TArray<TSharedPtr<FUICommandList>>& InLists)
	{
		for (const TSharedPtr<FUICommandList>& List : InLists)
		{
			if (List.IsValid() && List->IsActionMapped(InInfo))
			{
				return true;
			}
		}
		return false;
	}

	FString NameOf(const TSharedPtr<FUICommandInfo>& InInfo)
	{
		return FString::Printf(TEXT("%s.%s"), *InInfo->GetBindingContext().ToString(), *InInfo->GetCommandName().ToString());
	}
}

FString GameTestEditorCommands::ActiveEditorAsset()
{
	const TArray<GameTestEditorCommandsDetail::FEditorEntry> Open = GameTestEditorCommandsDetail::OpenEditors();
	return Open.Num() > 0 && Open[0].Asset != nullptr ? Open[0].Asset->GetPathName() : FString();
}

bool GameTestEditorCommands::FindByChord(const FKey& InKey, bool bInCtrl, bool bInAlt, bool bInShift, bool bInCmd, FString& OutName)
{
	OutName.Reset();
	FInputBindingManager& Manager = FInputBindingManager::Get();
	TArray<TSharedPtr<FBindingContext>> Contexts;
	Manager.GetKnownInputContexts(Contexts);
	const FInputChord Chord(InKey, EModifierKey::FromBools(bInCtrl, bInAlt, bInShift, bInCmd));

	const TArray<TSharedPtr<FUICommandList>> Lists = GameTestEditorCommandsDetail::CandidateLists(nullptr);
	FString Fallback;
	for (const TSharedPtr<FBindingContext>& Context : Contexts)
	{
		if (!Context.IsValid())
		{
			continue;
		}
		const TSharedPtr<FUICommandInfo> Info = Manager.FindCommandInContext(Context->GetContextName(), Chord, /*bCheckDefault=*/false);
		if (!Info.IsValid())
		{
			continue;
		}
		// 同一个键在好几个上下文里都绑了（Ctrl+S 关卡编辑器、资源编辑器都有）：开着的编辑器里绑了的才是这次按下去生效的
		if (GameTestEditorCommandsDetail::IsMappedAnywhere(Info, Lists))
		{
			OutName = GameTestEditorCommandsDetail::NameOf(Info);
			return true;
		}
		if (Fallback.IsEmpty())
		{
			Fallback = GameTestEditorCommandsDetail::NameOf(Info);
		}
	}
	OutName = Fallback;
	return !OutName.IsEmpty();
}

void GameTestEditorCommands::FindByLabel(const FString& InLabel, int32 InMax, TArray<FString>& OutNames)
{
	OutNames.Reset();
	const FString Label = InLabel.TrimStartAndEnd();
	if (Label.IsEmpty() || InMax <= 0)
	{
		return;
	}
	FInputBindingManager& Manager = FInputBindingManager::Get();
	TArray<TSharedPtr<FBindingContext>> Contexts;
	Manager.GetKnownInputContexts(Contexts);
	const TArray<TSharedPtr<FUICommandList>> Lists = GameTestEditorCommandsDetail::CandidateLists(nullptr);

	TArray<FString> Unmapped;
	for (const TSharedPtr<FBindingContext>& Context : Contexts)
	{
		if (!Context.IsValid())
		{
			continue;
		}
		TArray<TSharedPtr<FUICommandInfo>> Infos;
		Manager.GetCommandInfosFromContext(Context->GetContextName(), Infos);
		for (const TSharedPtr<FUICommandInfo>& Info : Infos)
		{
			if (!Info.IsValid() || !Info->GetLabel().ToString().TrimStartAndEnd().Equals(Label, ESearchCase::IgnoreCase))
			{
				continue;
			}
			// 开着的编辑器里绑了的排前面：同名的命令很多（「保存」「编译」每种编辑器都有一个）
			if (GameTestEditorCommandsDetail::IsMappedAnywhere(Info, Lists))
			{
				OutNames.Add(GameTestEditorCommandsDetail::NameOf(Info));
			}
			else
			{
				Unmapped.Add(GameTestEditorCommandsDetail::NameOf(Info));
			}
		}
	}
	OutNames.Append(Unmapped);
	if (OutNames.Num() > InMax)
	{
		OutNames.SetNum(InMax);
	}
}

bool GameTestEditorCommands::Execute(const FString& InName, const FString& InAsset, FString& OutMessage)
{
	FString Context;
	FString Command;
	if (!InName.Split(TEXT("."), &Context, &Command) || Context.IsEmpty() || Command.IsEmpty())
	{
		OutMessage = FString::Printf(TEXT("命令名要写成「上下文.命令」（例如 LevelEditor.Save），现在是 %s"), *InName);
		return false;
	}
	const TSharedPtr<FUICommandInfo> Info = FInputBindingManager::Get().FindCommandInContext(FName(*Context), FName(*Command));
	if (!Info.IsValid())
	{
		OutMessage = FString::Printf(TEXT("找不到命令 %s：上下文或命令名写错了，或者提供它的编辑器模块还没加载（先打开对应的编辑器）"), *InName);
		return false;
	}

	UObject* Asset = nullptr;
	if (!InAsset.IsEmpty())
	{
		FString Ignored;
		Asset = AgentPanel::FindObjectByPath(InAsset, /*bLoad=*/false, Ignored, /*bKeepBlueprint=*/true);
		UAssetEditorSubsystem* Subsystem = GameTestEditorCommandsDetail::Editors();
		if (Asset == nullptr || Subsystem == nullptr || Subsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/true) == nullptr)
		{
			OutMessage = FString::Printf(TEXT("命令 %s 要在 %s 的编辑器里执行，但它没有打开：前面加一步 editor.open asset=%s"), *InName, *InAsset, *InAsset);
			return false;
		}
	}

	const TSharedRef<const FUICommandInfo> InfoRef = Info.ToSharedRef();
	for (const TSharedPtr<FUICommandList>& List : GameTestEditorCommandsDetail::CandidateLists(Asset))
	{
		if (!List.IsValid() || !List->IsActionMapped(Info))
		{
			continue;
		}
		if (!List->CanExecuteAction(InfoRef))
		{
			OutMessage = FString::Printf(TEXT("命令 %s 现在不能执行（菜单里是灰的）：检查前面的步骤有没有把编辑器带到能执行它的状态"), *InName);
			return false;
		}
		if (!List->ExecuteAction(InfoRef))
		{
			OutMessage = FString::Printf(TEXT("命令 %s 执行失败"), *InName);
			return false;
		}
		OutMessage = FString::Printf(TEXT("执行了 %s（%s）"), *InName, *Info->GetLabel().ToString());
		return true;
	}
	OutMessage = FString::Printf(TEXT("命令 %s 没有绑在开着的编辑器上：先打开提供它的编辑器（资源编辑器的命令加 asset=资源路径）"), *InName);
	return false;
}
