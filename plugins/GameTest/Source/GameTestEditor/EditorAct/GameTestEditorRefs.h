// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UObject;
class UWorld;

/**
 * editor.* 步骤怎么指一个对象（1.10.0）。
 *
 * - 关卡里的 Actor 写 `actor:<标签>`，它的组件写 `actor:<标签>/<组件名>`：Actor 的对象名（BP_Shop_C_2）每次放进关卡都可能变，
 *   大纲里看到的标签才是人认的名字，录 `editor.spawn` 时也把标签一起记下、回放时设回去，后面的步骤才找得到同一个。
 * - 资源（含蓝图的类默认对象）写对象路径，回放时按路径找、没加载就加载。
 */
namespace GameTestEditorRefs
{
	/** 编辑器关卡世界（不是 PIE）；不在编辑器里时为空。 */
	UWorld* GetEditorWorld();

	/** 能不能录：PIE 世界、临时包、临时对象里的变化不录。 */
	bool IsRecordable(UObject* InObject);

	/** 对象 → 引用文本；认不出（PIE、临时对象）返回空。 */
	FString MakeRef(UObject* InObject);

	/** 给人看的名字：Actor 用标签，组件写「标签 · 组件名」，资源用资源名。 */
	FString Describe(UObject* InObject);

	/** 引用文本 → 对象；找不到时 OutError 写清楚是哪一段没找到、怎么办。 */
	UObject* Resolve(const FString& InRef, FString& OutError);

	/** 编辑器关卡里按标签找 Actor（第一个）。 */
	AActor* FindActorByLabel(const FString& InLabel);

	/** 对象路径或包名 → 包名（`/Game/A/B.B` → `/Game/A/B`）。 */
	FString ToPackageName(const FString& InPath);
}
