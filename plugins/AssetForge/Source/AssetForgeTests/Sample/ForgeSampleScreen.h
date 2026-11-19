// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Engine/TimerHandle.h"
#include "Framework/Commands/InputChord.h"
#include "Types/SlateEnums.h"
#include "ForgeSampleScreen.generated.h"

class UButton;
class UForgeSampleScreen;
class UWidget;
class UWidgetSwitcher;

/**
 * 一个控件的事件转发：动态委托只能绑无参 / 固定参数的 UFUNCTION，拿不到「是哪个控件发的」，
 * 所以每个控件配一个转发对象，记住控件名，再交回给界面。
 */
UCLASS()
class UForgeSampleBinding : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TWeakObjectPtr<UForgeSampleScreen> Screen;

	UPROPERTY()
	TWeakObjectPtr<UWidget> Source;

	UFUNCTION()
	void OnClicked();

	UFUNCTION()
	void OnCheckChanged(bool bIsChecked);

	UFUNCTION()
	void OnValueChanged(float InValue);

	UFUNCTION()
	void OnOptionChanged(FString InSelectedItem, ESelectInfo::Type InSelectionType);

	UFUNCTION()
	void OnTextCommitted(const FText& InText, ETextCommit::Type InCommitMethod);

	/** 长按、悬停（`Hold_`、`Hover_` 按钮）才绑这几个 */
	UFUNCTION()
	void OnPressed();

	UFUNCTION()
	void OnReleased();

	UFUNCTION()
	void OnHovered();

	UFUNCTION()
	void OnUnhovered();

	UFUNCTION()
	void OnKeySelected(FInputChord InSelectedKey);

	/** 解绑这个对象挂过的所有委托（界面重建 / 析构时调） */
	void Unbind();
};

/**
 * AssetForge 示例测试界面（`docs/AssetForge/Test`）的父类：**按控件名约定**接好跳页、返回、弹窗。
 *
 * 为什么用 C++ 父类而不是蓝图图表：AssetForge 的 UMG 配置只管控件树和属性，不管事件图表；
 * 把逻辑收在一个按名字工作的父类里，界面就能完全由 JSON 描述，加页面、加按钮不用写代码，
 * 也不依赖项目自己的 UnLua。它在测试模块里（UncookedOnly，只在编辑器 / PIE 里有），
 * 所以示例资产不要打进包（`/Game/AgentTest` 加进「永不烘焙的目录」）。
 *
 * 名字约定（`__` 后面的部分只用来让名字不重复，不参与解析，例如 `Go_Page_Shop__Hero`）：
 * - `Go_<目标>`：目标是某个 WidgetSwitcher 的子控件，切过去；切的是 `Pages` 时记进返回历史，并关掉所有弹窗。
 * - `Back`：先关最上面打开的弹窗，没有弹窗就回到上一页。
 * - `Open_<弹窗>` / `Close_<弹窗>`：把控件设成可见 / 折叠（弹窗约定叫 `Popup_<一个词>`，重置界面时全部折叠）。
 * - `Pick_<文字控件>`：把按钮的文字写进这个 TextBlock（列表选中、物品详情）。
 * - `Submit_<输入框>`：把输入框的文字写进 `<输入框>_Echo`。
 * - `Go_` / `Open_` 的按钮有提示文字（ToolTipText）时，写进目标的 `<目标>_Subject`（「购买：生命药水」这类）。
 * - 滑条、下拉框、复选框变了写进 `<控件名>_Value`。
 * - 所有操作的结果写进 `Status`，当前页的标题写进 `Crumb`（页标题取 `<页>_Title`，没有就取导航按钮的文字）：
 *   GameTest 断言这两个文字就能确认点对了。
 * - 指向当前所在页 / 页签的 `Go_` 按钮染成 ActiveTint，其余恢复原来的 BackgroundColor。
 *
 * 1.5.0 起每一页是一个子 WBP（配置里 `ref` 引用 `pages/` 下的子配置），控件分在几棵 WidgetTree 里：
 * 名字约定照旧在**整个界面**里找（先自己的树，再进嵌套的蓝图子界面），所以名字在所有页里都不重名。
 * 「流程」「控件」两页给 GameTest 的脚本功能当靶子，多了这些约定（实现在 ForgeSampleFlow.cpp）：
 * - `Inc_<文字>` / `Dec_<文字>` / `Reset_<文字>`：文字里的整数 +1 / -1 / 归零；文字控件叫 `Counter` 时同步到 `Counter` 属性。
 * - `Claim_<名>`：第一次点把按钮上的字改成「已领取」，再点只提示领过了（练「已达成就跳过」）。
 * - `Delay_<控件>`：RevealDelaySeconds 秒后把控件显示出来（练等待）；`Toggle_<控件>`：显示 / 折叠来回切（练分支）。
 * - `Arm_<弹窗>`：ArmDelaySeconds 秒后自己弹出来（练守护：不定时弹出、挡住后面步骤的界面）。
 * - 复选框 `Enable_<控件>`：勾上才让那个控件可用（练「可点」判断）。
 * - `Hold_<名>`：按住到松开的时长写进 `<名>_Value`，满 HoldSeconds 算「蓄力完成」（练长按）。
 * - `Hover_<名>`：鼠标进出写进 `<名>_Value`（练真实鼠标悬停）。
 * - `Menu_<菜单锚点>`：开 / 关菜单（菜单内容是 C++ 类 ForgeSampleMenu）。
 * - 滑条有 `<名>_Bar` 进度条时一起动；按键选择器选的键写进 `<名>_Value`。
 */
UCLASS(Blueprintable)
class UForgeSampleScreen : public UUserWidget
{
	GENERATED_BODY()

public:
	/** 记返回历史的那个 WidgetSwitcher 的名字 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ForgeSample")
	FName PageSwitcherName = TEXT("Pages");

	/** 当前页 / 页签按钮的底色（线性颜色） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ForgeSample")
	FLinearColor ActiveTint = FLinearColor(0.08f, 0.26f, 1.0f, 1.0f);

	/** 显示后打开鼠标光标、切到「游戏和 UI」输入：手动录制时能直接点 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ForgeSample")
	bool bShowCursor = true;

	/** `Delay_` 按钮多久之后把目标显示出来（秒） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ForgeSample")
	float RevealDelaySeconds = 1.5f;

	/** `Arm_` 按钮多久之后弹出弹窗（秒） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ForgeSample")
	float ArmDelaySeconds = 1.0f;

	/** `Hold_` 按钮按住多久算长按（秒） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ForgeSample")
	float HoldSeconds = 1.0f;

	/** 「流程」页的计数（和 `Counter` 文字同步）：GameTest 的 field / invoke / 对象定位读它 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ForgeSample")
	int32 Counter = 0;

	/** 领过的 `Claim_` 名字个数 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ForgeSample")
	int32 ClaimCount = 0;

	/** 回到首页、清空历史、关掉所有弹窗 */
	UFUNCTION(BlueprintCallable, Category = "ForgeSample")
	void ResetScreen();

	/** 把计数设成某个值（GameTest invoke 练调函数、带参数） */
	UFUNCTION(BlueprintCallable, Category = "ForgeSample")
	void SetCounter(int32 InValue);

	/** 计数加 InDelta，返回加完的值（invoke 的 saveAs 练返回值） */
	UFUNCTION(BlueprintCallable, Category = "ForgeSample")
	int32 AddCounter(int32 InDelta);

	/** 只读：当前计数（条件、表达式里能调的只读函数） */
	UFUNCTION(BlueprintPure, Category = "ForgeSample")
	int32 GetCounter() const { return Counter; }

	/** 只读：某个 `Claim_` 名字领过没有 */
	UFUNCTION(BlueprintPure, Category = "ForgeSample")
	bool IsClaimed(const FString& InName) const { return ClaimedNames.Contains(InName); }

	/** 把「流程」页恢复原样：计数归零、领取状态清掉、延时显示的控件藏回去（无参，widget.call 能调） */
	UFUNCTION(BlueprintCallable, Category = "ForgeSample")
	void ResetFlow();

	/** 关掉所有弹出的菜单（菜单里的按钮点完调） */
	void CloseMenus();

	/** 列表条目页的条目控件（ForgeSampleLists）把操作结果写进 Status 用 */
	void ReportStatus(const FString& InText) { SetStatus(InText); }

	void HandleClick(UWidget* InSource);
	void HandleCheck(UWidget* InSource, bool bIsChecked);
	void HandleValue(UWidget* InSource, float InValue);
	void HandleOption(UWidget* InSource, const FString& InItem);
	void HandleCommit(UWidget* InSource, const FText& InText);
	void HandlePressed(UWidget* InSource);
	void HandleReleased(UWidget* InSource);
	void HandleHover(UWidget* InSource, bool bInHovered);
	void HandleKeySelected(UWidget* InSource, const FInputChord& InKey);

	/** 整个界面（含子界面）里按名字找控件 */
	UWidget* FindAny(const FString& InName) const;

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

private:
	/** 去掉 `__` 后缀的名字：`Go_Page_Shop__Hero` → `Go_Page_Shop` */
	static FString BaseName(const UWidget* InWidget);

	/** 按钮上给人看的文字：先找 `L_<按钮名>`，再找第一个 TextBlock */
	FString LabelOf(const UWidget* InWidget) const;

	/** 页 / 弹窗的标题：`<名字>_Title`，没有就取 `Go_<名字>__Nav` 按钮的文字，再没有就用名字 */
	FString TitleOf(const FString& InName) const;

	/** 「流程」「控件」两页的名字约定（ForgeSampleFlow.cpp）；认得这个名字返回 true */
	bool HandleFlowClick(UWidget* InSource, const FString& InName, const FString& InLabel);
	void SetVisible(UWidget* InWidget, bool bInVisible);
	void OpenPopup(UWidget* InPopup, const UWidget* InSource, const FString& InLabel);

	void BindAll();
	void UnbindAll();
	void GoTo(UWidget* InTarget, bool bRecordHistory);
	void Back();
	void CloseAllPopups();
	void SetText(const FString& InWidgetName, const FString& InText);
	void SetStatus(const FString& InText);
	void SetSubject(const FString& InTargetName, const UWidget* InSource);
	void Refresh();
	UWidgetSwitcher* PageSwitcher() const;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UForgeSampleBinding>> Bindings;

	/** 打开顺序：Back 先关最后打开的 */
	TArray<TWeakObjectPtr<UWidget>> OpenPopups;

	/** Pages 的返回历史（存页的控件，不存序号：配置里调了页的顺序也不乱） */
	TArray<TWeakObjectPtr<UWidget>> History;

	/** 每个 Go_ 按钮在配置里的底色，染色之后靠它恢复 */
	TMap<TWeakObjectPtr<UButton>, FLinearColor> NormalTints;

	/** 名字 → 控件（BindAll 时建；子界面里的控件也在） */
	TMap<FName, TWeakObjectPtr<UWidget>> ByName;

	/** 领过的 `Claim_` 名字 */
	TSet<FString> ClaimedNames;

	/** `Hold_` 按钮按下的时刻（FPlatformTime::Seconds） */
	TMap<FName, double> PressedAt;

	/** `Delay_` / `Arm_` 的计时器：重置界面时一起取消 */
	TArray<FTimerHandle> Timers;
};
