// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FGameTestWidgetNode;
class UListView;

/**
 * 选择器：把一段文本解析成"树里的哪个控件"。
 *
 * 语法（`target` 字段）：
 * - `@句柄`            —— 引用之前 `find` 捕获的控件（绕过重解析）
 * - `屏名:路径`        —— 先按屏名（类名或控件名，忽略大小写）挑屏，再走路径
 * - `路径`             —— 不写屏名：在所有屏里找，多屏命中即歧义
 * - 路径段：`名字` / `#类名` / `#类名#序号` / `text=子串` / `*`（任一子节点）/ `..`（任一后代）/ `.`（就是这个屏本身）
 * - `列表名[序号]`、`列表名[属性=值]` —— ListView 的某个条目（条目控件，只认已经生成出来的）
 * - `容器[条件,条件]`（1.4.0）—— 任意容器（ListView 系、面板、DynamicEntryBox）里按标识认条目：`Data.ItemId=1001`、
 *                         `@self.字段`、`@lua.字段`、`@itemLua.字段`、`@text=文字`、`@row=1,@col=2`，见 GameTestItemKey
 * - `A>B`              —— A 落在一个嵌套的 WBP 实例上（或列表条目上），B 从**这个实例自己的**控件树根开始走
 * - `A>>Slate段`       —— 在 A 的 Slate 子树里找纯 Slate 控件（下拉项、菜单行）；段写法 `类型[text=子串][序号]`，
 *                         多段用 `/` 分隔，每段在上一段的任意深度里找
 * - `>>Slate段`        —— 不挂在 UMG 上：在游戏窗口和它弹出的子窗口里找
 * - 屏名可以写 WBP 类名（去掉 _C）、控件名，或资源路径（`/Game/UI/WBP_Main`）
 *
 * 解析失败一律**当作错误**并带上候选清单——不做"取第一个匹配"这种静默行为。
 * 整棵树是唯一的搜索空间：与快照共用同一份模型，AI 看到的与点到的是同一个控件。
 */
class GAMETEST_API GameTestSelector
{
public:
	struct FParsed
	{
		bool bHandle = false;
		FString Handle;
		bool bAnyScreen = false;
		FString Screen;
		/** 第一段路径（屏内）。 */
		TArray<FString> Segments;
		/** `>` 之后每进一层嵌套 WBP 的路径。 */
		TArray<TArray<FString>> NestedHops;
		/** `>>` 之后的 Slate 段。 */
		TArray<FString> SlateSegments;
		/** 整个目标以 `>>` 开头：不经 UMG，直接在游戏窗口里找 Slate 控件。 */
		bool bSlateOnly = false;
	};

	static bool Parse(const FString& InText, FParsed& OutParsed, FString& OutError);

	/** 屏名匹配：空或 `*` 表示任意；否则与类名或控件名忽略大小写相等。 */
	static bool MatchScreen(const FString& InScreen, const FGameTestWidgetNode& InScreenRoot);

	/**
	 * 在若干屏根里解析路径；唯一命中才成功，失败时 OutError 带候选。
	 * 屏名命中多个屏、每个屏里都走得通（同一个 WBP 的几个实例）时，失败并把全部命中放进 `OutAmbiguous`（给了的话）：
	 * 选择器自己不挑，由拿着录制线索的调用方决定（`GameTestHintPick`）。
	 */
	static bool Resolve(const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens, const FParsed& InParsed, TSharedPtr<FGameTestWidgetNode>& OutNode, FString& OutError, TArray<TSharedPtr<FGameTestWidgetNode>>* OutAmbiguous = nullptr);

	/**
	 * 上一次解析失败是不是因为 `列表[...]` 指向的条目还没生成（虚拟化列表没滚到）：是的话交出列表和序号并清掉记录。
	 * 解析本身不滚（`gt.resolve` 只看不动）；回放的 UI 动作拿它把条目滚出来再等（GameTestAutoScroll）。
	 */
	static bool TakeUngeneratedEntry(TWeakObjectPtr<UListView>& OutList, int32& OutIndex, FString& OutListPath);

	/** 解析 + 一步到位（`Resolve` 的便捷包装；@句柄 由调用方处理）。 */
	static bool ResolveText(const TArray<TSharedPtr<FGameTestWidgetNode>>& InScreens, const FString& InText, TSharedPtr<FGameTestWidgetNode>& OutNode, FString& OutError, TArray<TSharedPtr<FGameTestWidgetNode>>* OutAmbiguous = nullptr);
};
