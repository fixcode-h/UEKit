// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Blueprint/UserWidget.h"
#include "CommonActivatableWidget.h"
#include "Probe/GameTestProbeListWidget.h"
#include "GameTestProbeContainers.generated.h"

class UCommonActivatableWidgetStack;
class UTextBlock;
class UTileView;
class UTreeView;

/**
 * 容器探针组（一个文件放四个紧密相关的靶子）：
 *
 * - `UGameTestProbeTileWidget`：TileView，验"列表族一致"（选择/条目读取与 ListView 同源）；
 * - `UGameTestProbeTreeWidget`：TreeView，验条目展开与"展开后条目才生成"；
 * - `UGameTestProbeStackWidget` + `UGameTestProbeScreen`：CommonUI 层级栈，验推入/弹出与当前界面。
 *
 * 列表族的条目类复用 `GameTestProbe::ResolveEntryClass()`（插件自带的 C++ 条目类）。
 * 栈推的是原生可激活类：`AddWidget` 走 `CreateWidget`，原生类可用。
 */

/** 探针树节点：带子项的数据项（子项由取子委托交给 STreeView）。 */
UCLASS()
class GAMETESTTESTS_API UGameTestProbeTreeItem : public UGameTestProbeItem
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TArray<TObjectPtr<UGameTestProbeItem>> Children;
};

/** TileView 探针：6 个条目（`Id` 1..6、`DisplayName` 条目1..6）。 */
UCLASS()
class GAMETESTTESTS_API UGameTestProbeTileWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget interface
	virtual TSharedRef<SWidget> RebuildWidget() override;
	//~ End UUserWidget interface

	/** 造 6 个条目；可重复调用（先清空）。 */
	void SeedProbeItems();

	UPROPERTY(Transient)
	TObjectPtr<UTileView> TileView;
};

/** TreeView 探针：2 个根（`Id` 1..2），每个根 2 个子项（`Id` 11/12 与 21/22）。 */
UCLASS()
class GAMETESTTESTS_API UGameTestProbeTreeWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget interface
	virtual TSharedRef<SWidget> RebuildWidget() override;
	//~ End UUserWidget interface

	/** 取子委托：STreeView 靠它展开子项（根的子项就是 `Children`）。 */
	void HandleGetItemChildren(UObject* InItem, TArray<UObject*>& OutChildren);

	void SeedProbeItems();

	UPROPERTY(Transient)
	TObjectPtr<UTreeView> TreeView;

	/** 根条目与它们的子项（子项只被这里引用，靠 UPROPERTY 保活）。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UGameTestProbeTreeItem>> RootItems;
};

/** CommonUI 层级栈探针：根就是一个 `UCommonActivatableWidgetStack`。 */
UCLASS()
class GAMETESTTESTS_API UGameTestProbeStackWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget interface
	virtual TSharedRef<SWidget> RebuildWidget() override;
	//~ End UUserWidget interface

	UPROPERTY(Transient)
	TObjectPtr<UCommonActivatableWidgetStack> Stack;
};

/** 可激活的探针界面：push 进栈后显示自己的类名（纯 C++，不需要 WBP）。 */
UCLASS()
class GAMETESTTESTS_API UGameTestProbeScreen : public UCommonActivatableWidget
{
	GENERATED_BODY()

protected:
	//~ Begin UUserWidget interface
	virtual TSharedRef<SWidget> RebuildWidget() override;
	//~ End UUserWidget interface

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> Label;
};
