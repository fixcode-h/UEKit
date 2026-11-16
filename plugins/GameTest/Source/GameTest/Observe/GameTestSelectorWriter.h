// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class SWidget;
class UUserWidget;
class UWidget;

/** 写出来的选择器和附带的线索。 */
struct FGameTestWrittenTarget
{
	/** 选择器（`屏名:路径`、`A>B`、`列表[3]`、`A>>Slate段`）。 */
	FString Target;

	/**
	 * 线索：`asset`（WBP 资源路径）、`class`、`text`、`rect`（视口归一化 [x, y, w, h]）。
	 * 用来显示；选择器命中同一个 WBP 的几个实例时，回放按 `text`、`rect` 挑回录的那一个（GameTestHintPick），别的情况不拿它换目标。
	 * 解析核对没过、但按线索挑得回来时多一个 `pick`（说明）；挑不回来时多一个 `ambiguous`（原因）。
	 */
	TSharedPtr<FJsonObject> Hint;

	/** 用回放同一套解析核对过：唯一、指回同一个控件。 */
	bool bVerified = false;
};

/**
 * 控件 → 选择器（录制和 `gt.inspect` 用）。
 *
 * 规则（和类无关）：
 * 1. 从控件沿 `GetParent()` 走到所在 WBP 控件树的根，路径用**设计器里起的名字**；运行时生成的名字
 *    （`WBP_Item_C_12` 这类，名字等于类名加序号、又不在 WBP 的控件树里）换成 `#类名#同类序号`。
 * 2. 所在的 WBP 是嵌套的实例：先写出这个实例，再用 `>` 进去。
 * 3. 所在的 WBP 是列表条目：写成 `列表[属性=值]`（能找到唯一的 Id 类属性时）或 `列表[序号]`。
 * 4. 所在的 WBP 没有 UMG 父面板、挂在某个控件的 Slate 上（DynamicEntryBox、自定义列表、Lua 挂的）：
 *    写成 `容器/#类名#同类序号`，快照把它挂在同一个容器下面（GameTestSlate::CollectAttachedUserWidgets）。
 * 5. 写完用回放同一套解析核对：必须唯一、指回同一个控件；不唯一但按线索挑得回来时记 `pick`，否则记 `ambiguous`。
 */
class GAMETEST_API GameTestSelectorWriter
{
public:
	static bool Write(UWidget& InWidget, FGameTestWrittenTarget& OutTarget, FString& OutError);

	/** 纯 Slate 控件（下拉项、菜单行）：`最近的 UMG 锚点>>类型[text=子串]`，没有锚点时 `>>类型[text=子串]`。 */
	static bool WriteSlate(const TSharedRef<SWidget>& InWidget, FGameTestWrittenTarget& OutTarget, FString& OutError);

	/** 屏幕坐标 → 控件内归一化坐标（0–1，三位小数）；几何无效时返回 false。 */
	static bool ToLocalPoint(const SWidget& InWidget, const FVector2D& InScreenPosition, FVector2D& OutPoint);

	/** 是不是顶层屏：不在任何面板里、也不在别的 WBP 的控件树里（列表条目不算顶层）。 */
	static bool IsTopLevel(const UWidget& InWidget);
};
