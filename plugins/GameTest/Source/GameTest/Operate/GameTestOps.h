// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UListView;
class UTreeView;
class UUserWidget;
class UWidget;
class UWidgetSwitcher;
class UWorld;

/**
 * 代码接口层的 UMG 操作。
 *
 * 判据（用户定的方案）：**不做鼠标/键盘的坐标模拟**，而是调用控件自己的接口——
 * 按钮走 `UButton::OnClicked`（公开委托，游戏/C# 绑的就是它），复选走 `SetIsChecked` + `OnCheckStateChanged`，
 * 输入框走 `SetText` + `OnTextChanged/OnTextCommitted`，列表走 `SetSelectedItem`，滚动走 `SetScrollOffset`。
 *
 * **为什么不是 Slate 那一层**：`SButton::ExecuteOnClick`、`UButton::SlateHandleClicked`、
 * `UCheckBox::SlateOnCheckStateChangedCallback`、`UEditableText::HandleOnTextChanged` 等
 * 都是 **protected**（引擎只让它们自己与子类用），从外面调不到。
 * 能调到的公开面就是 UMG 的委托与 setter，而它们正是蓝图/C# 绑定游戏逻辑的那一层；
 * 代价是 `UCheckBox::SetIsChecked` 这类 setter **不会**自己广播对应委托，
 * 所以这里显式补一次广播，并在消息与结果里写清楚补了什么（不留静默行为）。
 *
 * 好处是"无感"：不依赖控件是否可见、是否被遮挡、窗口是否在前台、分辨率是多少，
 * 因此同一条场景在 PIE、独立进程、打包构建里行为一致。
 * 代价是**遮挡与输入模式这类缺陷不会被这些操作暴露**——那类问题由 `probe` 命中测试
 * （见任务清单）单独检，不混在操作路径里。
 *
 * 每个分支都自己校验参数：给了该控件不认的字段就报错，不做静默忽略。
 */
class GAMETEST_API GameTestOps
{
public:
	static bool Click(UWidget* InWidget, FString& OutMessage, TSharedPtr<FJsonObject>& OutPayload);

	static bool SetText(UWidget* InWidget, const FString& InText, bool bInCommit, FString& OutMessage, TSharedPtr<FJsonObject>& OutPayload);

	/** `where` 已由调用方解析成索引进来了。 */
	static bool SelectListItem(UListView* InList, int32 InIndex, bool bInAdd, FString& OutMessage, TSharedPtr<FJsonObject>& OutPayload);

	/**
	 * 列表有没有活的 Slate 列表控件。
	 *
	 * 编辑器里条目类（`EntryWidgetClass`）不是蓝图时，引擎在 `UListViewBase::RebuildWidget` 里
	 * 把整块列表换成**一行错误文本**，`MyListView` 从未被创建——于是选中与读回全部**静默失效**（实测）。
	 * 观测侧（snapshot 的 `state.slateList`）与操作侧（select 的读回校验）共用这一份判断。
	 */
	static bool HasLiveSlateList(const UListView& InList);

	/** mode：offset / end / start / widget（ScrollBox）或 index（ListView）。 */
	static bool Scroll(
		UWidget* InWidget,
		const FString& InMode,
		double InValue,
		int32 InIndex,
		UWidget* InTargetWidget,
		FString& OutMessage,
		TSharedPtr<FJsonObject>& OutPayload);

	/** 通用赋值：滑条/微调框数值、复选、下拉选项、展开态、菜单开合、按键选择。 */
	static bool SetValue(UWidget* InWidget, const FJsonObject& InParams, FString& OutMessage, TSharedPtr<FJsonObject>& OutPayload);

	/** 页切换（`UWidgetSwitcher`）：`SetActiveWidgetIndex` + 读回。 */
	static bool SetSwitcherIndex(UWidgetSwitcher* InSwitcher, int32 InIndex, FString& OutMessage, TSharedPtr<FJsonObject>& OutPayload);

	/**
	 * 列表族的条目展开（`UTreeView`）：`SetItemExpansion` + 回报"已生成条目数"。
	 * 展开态本身**读不回来**（引擎没有公开读接口），所以把 `displayedEntries`（折叠时只有根、展开后子条目才有控件）当作可观测量。
	 */
	static bool SetListItemExpansion(UTreeView* InTree, UObject* InItem, bool bInExpanded, FString& OutMessage, TSharedPtr<FJsonObject>& OutPayload);

	/**
	 * 类路径/类名 → 已加载的类（要派生自 `InRequiredBase`，一般是 `UUserWidget`；GameTestCommonUI 用它解析可激活控件类）。
	 * `widget.add` 与 `stack.push` 共用这一处解析：`/Game/...` 补 `_C`、裸名在已加载类里找、失败给候选清单。
	 */
	static UClass* ResolveWidgetClass(const FString& InClassPath, UClass* InRequiredBase, FString& OutError);

	static bool AddWidgetToViewport(
		UWorld* InWorld,
		const FString& InClassPath,
		int32 InZOrder,
		UUserWidget*& OutWidget,
		FString& OutMessage);

	static bool RemoveWidget(UUserWidget* InWidget, FString& OutMessage);

	/** 这个控件能点什么（用于"目标不可点"时的提示）。 */
	static bool IsClickable(const UWidget* InWidget, FString& OutHint);

	/** 已加载的 UUserWidget 派生类名（类路径写错时的候选清单）。 */
	static void CollectLoadedWidgetClasses(TArray<FString>& OutClassPaths, int32 InLimit = 20);
};
