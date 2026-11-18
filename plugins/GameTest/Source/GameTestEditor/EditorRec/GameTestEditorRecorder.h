// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class IInputProcessor;
class UObject;
class AActor;
class UBlueprint;
class UPackage;
struct FKey;
struct FPropertyChangedEvent;
class FEditPropertyChain;

/**
 * 录 UE 编辑器操作（定稿 10.1，GameTest 1.11.0）：把人在编辑器里的操作记成 editor.* 步骤。
 *
 * **能按语义录就不按界面录**：
 * - 打开关卡 / 资源（FEditorDelegates::OnMapOpened、UAssetEditorSubsystem::OnAssetEditorOpened）→ `editor.open`；
 * - 关卡里选中 Actor（USelection::SelectionChangedEvent）→ `editor.select`；
 * - 细节面板里改属性（FCoreUObjectDelegates::OnObjectPropertyChanged）→ `editor.prop`，连着改同一个属性合成一步；
 * - 放 Actor（GEngine->OnLevelActorAdded）→ `editor.spawn`，带上标签，后面的步骤按标签找它；
 * - 保存、编译（UPackage::PackageSavedWithContextEvent、GEditor->OnBlueprintPreCompile）→ `editor.save` / `editor.compile`；
 * - 快捷键（反查 FUICommandInfo）→ `editor.command`。
 * 这些事件之外的点击才按界面录（`editor.click`，按停靠页 + 类型 + 文字认），按钮上的字和某个命令的名字对得上时
 * 附上「换成命令更稳」的建议（`suggest.commands`）。点击、快捷键先挂 0.5 秒：这段时间里有了语义事件
 * （点「编译」按钮引出的编译事件），就只留语义步骤。按住 Ctrl+Alt 点一个控件录成 `editor.assert`（这一下不传给编辑器）。
 *
 * 视口、细节面板、大纲、内容浏览器里的点击不录（它们引出的选中、改属性另有事件）；游戏界面里的操作归游戏录制。
 * 碰到模态框插一条说明：模态框是嵌套消息循环，回放时能不能点要在本地验证，录到就提醒。只在游戏线程上用。
 */
class FGameTestEditorRecorder
{
public:
	static FGameTestEditorRecorder& Get();

	bool Start(const FString& InName, FString& OutError);

	/** 停止。`bInSave` 时写成场景（场景根下 `recorded/<名字>.json`，重名加序号），结果字段和游戏录制一样。 */
	bool Stop(bool bInSave, TSharedPtr<FJsonObject>& OutResult, FString& OutError);

	bool Mark(const FString& InText, FString& OutError);

	bool IsRecording() const
	{
		return bRecording;
	}

	/** 和游戏录制同样的字段，多一个 `target: "editor"`。 */
	TSharedRef<FJsonObject> DescribeStatus(int32 InLastSteps) const;

	/** 模块关闭：还在录就丢掉。 */
	void Shutdown();

	//~ 输入处理器回调（游戏线程）。返回 true 表示吃掉这个事件（只有 Ctrl+Alt 拾取会吃）。
	bool OnMouseDown(const FVector2D& InScreenPosition, bool bInPick);
	void OnKeyDown(const FKey& InKey, bool bInCtrl, bool bInAlt, bool bInShift, bool bInCmd);
	void OnTick();

private:
	/** 按界面录的一步（点击、快捷键）：先挂着，0.5 秒里没有语义事件才写进去。 */
	struct FPendingUi
	{
		TSharedPtr<FJsonObject> Step;
		double At = 0.0;
	};

	/** 改属性前的值（细节面板改之前引擎先广播一次），用来在步骤里写「从多少改成多少」。 */
	struct FPreValue
	{
		TWeakObjectPtr<UObject> Object;
		FName Member;
		TSharedPtr<FJsonValue> Value;
	};

	void Bind();
	void Unbind();
	void AddStep(const TSharedRef<FJsonObject>& InStep, bool bInSemantic);
	void Broadcast(const TCHAR* InPhase);
	bool IsSuppressed(UObject* InObject) const;

	void HandleMapOpened(const FString& InFilename);
	void HandleAssetOpened(UObject* InAsset);
	void HandlePreChange(UObject* InObject, const FEditPropertyChain& InChain);
	void HandlePropertyChanged(UObject* InObject, const FPropertyChangedEvent& InEvent);
	void HandleActorAdded(AActor* InActor);
	void HandlePreCompile(UBlueprint* InBlueprint);
	void HandlePackageSaved(const FString& InFilename, UPackage* InPackage);

	void FlushSpawns();
	void FlushSelection(double InNow);
	void FlushSaves();
	void FlushPendingUi(double InNow);
	void CheckModal();

	bool bRecording = false;
	FString Name;
	double StartedAt = 0.0;
	TArray<TSharedRef<FJsonObject>> Steps;
	bool bTruncated = false;

	TSharedPtr<IInputProcessor> InputProcessor;
	TArray<FDelegateHandle> Handles;
	FDelegateHandle AssetOpenedHandle;

	/** 最近一次语义事件的时间：挂着的点击、快捷键在它之前按下的就丢掉。 */
	double LastSemanticAt = 0.0;
	/** 打开关卡后一小段时间里的选中、放 Actor 是引擎自己做的，不录。 */
	double SuppressUntil = 0.0;
	TArray<FPendingUi> PendingUi;

	TArray<TWeakObjectPtr<AActor>> PendingSpawns;
	/** 刚放的 Actor（标签 → 放的时间）：放完紧跟着的选中、位置属性变化是放 Actor 带出来的，不另录。 */
	TMap<FString, double> RecentSpawns;

	bool bSelectionDirty = false;
	double SelectionDirtyAt = 0.0;
	FString LastSelectionKey;

	TArray<FString> PendingSaves;
	TMap<FString, double> RecentCompiles;
	FPreValue PreValue;
	FString LastModalTitle;
};
