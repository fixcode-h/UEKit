// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

class UWidget;
class UPanelSlot;
class UWidgetTree;

/**
 * 适配层：控件树（UWidget）↔ 节点树（FForgeNode）。
 *
 * 控件与插槽的属性都不在这里逐个列举——它们走 ForgePropertyCodec 的反射通道。
 * 本文件只负责**结构**：类名解析、父子关系、以及插槽这个 UMG 特有的挂在控件上的子对象。
 */
namespace UmgWidgetCodec
{
	/**
	 * 解析控件类名。
	 *
	 * 短名（`TextBlock`）按 `/Script/UMG` 起手逐个前缀尝试；含 `/` 或 `.` 的按全路径加载，
	 * 蓝图类缺 `_C` 时自动补（导出写的是 `GetPathName()`，含 `_C`，人工手写往往省略）。
	 */
	ASSETFORGE_API UClass* ResolveWidgetClass(const FString& InClassName, FString& OutError);

	/** 导出方向：把一个控件及其子树读成节点。 */
	ASSETFORGE_API void ExportWidget(
		UWidget* InWidget,
		FForgeNode& OutNode,
		TArray<FForgeDiagnostic>& OutDiagnostics,
		const FString& InNodePath);

	/** 导入方向：把节点上的属性与插槽写进一个已存在的控件。 */
	ASSETFORGE_API void ImportWidgetInto(
		UWidget* InWidget,
		const FForgeNode& InNode,
		TArray<FForgeDiagnostic>& OutDiagnostics,
		const FString& InNodePath);

	/** 在 WidgetTree 里按名找一个控件（含子树）。 */
	ASSETFORGE_API UWidget* FindWidgetByName(UWidgetTree* InTree, const FString& InName);
}
