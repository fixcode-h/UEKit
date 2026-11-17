// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
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

	/** 回到首页、清空历史、关掉所有弹窗 */
	UFUNCTION(BlueprintCallable, Category = "ForgeSample")
	void ResetScreen();

	/** 列表条目页的条目控件（ForgeSampleLists）把操作结果写进 Status 用 */
	void ReportStatus(const FString& InText) { SetStatus(InText); }

	void HandleClick(UWidget* InSource);
	void HandleCheck(UWidget* InSource, bool bIsChecked);
	void HandleValue(UWidget* InSource, float InValue);
	void HandleOption(UWidget* InSource, const FString& InItem);
	void HandleCommit(UWidget* InSource, const FText& InText);

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
};
