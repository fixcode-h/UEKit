// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FKey;

/**
 * 编辑器命令（菜单、工具栏、快捷键背后的 FUICommandInfo）：录制时按快捷键或按钮上的字反查，回放时按名字执行（1.11.0）。
 *
 * 命令名写成 `上下文.命令`（`LevelEditor.Save`、`FullBlueprintEditor.Compile`），和「编辑器偏好设置 → 键盘快捷键」里的一致。
 * 执行要找到绑了这个命令的命令表：给了资源时先找它的资源编辑器，再找其他开着的资源编辑器，最后是关卡编辑器的全局命令表。
 */
namespace GameTestEditorCommands
{
	/** 快捷键 → 命令名；没有绑这个键的命令返回 false。同一个键在几个上下文里都有时，优先开着的编辑器里绑了的那个。 */
	bool FindByChord(const FKey& InKey, bool bInCtrl, bool bInAlt, bool bInShift, bool bInCmd, FString& OutName);

	/** 按显示名找命令（界面点击录完后提示「这个按钮背后是命令，换成命令更稳」），最多 InMax 个。 */
	void FindByLabel(const FString& InLabel, int32 InMax, TArray<FString>& OutNames);

	/** 执行命令。InAsset 非空时先在这个资源的编辑器里找（要先打开）。 */
	bool Execute(const FString& InName, const FString& InAsset, FString& OutMessage);

	/** 最近激活的资源编辑器在编辑哪个资源（对象路径）；没有开着的返回空。 */
	FString ActiveEditorAsset();
}
