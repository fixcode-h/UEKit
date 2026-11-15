// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/UserWidget.h"
#include "GameTestProbeListWidget.generated.h"

class UListView;
class UTextBlock;

/**
 * 探针公共件：列表族条目类的解析。只用插件自带的 C++ 条目类，不引用项目资产；
 * TileView / TreeView 探针共用它。
 */
class GAMETESTTESTS_API GameTestProbe
{
public:
	static UClass* ResolveEntryClass();
};

/**
 * 探针条目：带 `Id` 与 `DisplayName` 的数据项。
 * 用来验"按数据项属性选中条目"——那需要真实列表，而这个数据类就是它的内容。
 */
UCLASS()
class GAMETESTTESTS_API UGameTestProbeItem : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GameTest")
	int32 Id = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GameTest")
	FString DisplayName;
};

/** 探针条目控件：把 `DisplayName` 显示出来（纯 C++，没有 WBP）。 */
UCLASS()
class GAMETESTTESTS_API UGameTestProbeEntryWidget : public UUserWidget, public IUserObjectListEntry
{
	GENERATED_BODY()

public:
	UGameTestProbeEntryWidget(const FObjectInitializer& InObjectInitializer);

protected:
	//~ Begin IUserObjectListEntry interface
	virtual void NativeOnListItemObjectSet(UObject* InListItemObject) override;
	//~ End IUserObjectListEntry interface

	//~ Begin UUserWidget interface
	virtual TSharedRef<SWidget> RebuildWidget() override;
	//~ End UUserWidget interface

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> Label;
};

/**
 * 探针列表：一个自带 `ListView` 的界面，用 `widget.add class=GameTestProbeListWidget` 直接生成。
 *
 * 为什么需要它：项目里还没有业务 ListView，而"按属性选中条目"这条契约必须有真实列表才能验。
 * 靶子放在测试插件里，不依赖任何游戏流程，也不会污染游戏资产。
 */
UCLASS()
class GAMETESTTESTS_API UGameTestProbeListWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** 造 3 个条目：`Id` 1..3、`DisplayName` 条目1..3；可重复调用（先清空）。 */
	UFUNCTION(BlueprintCallable, Category = "GameTest")
	void SeedProbeItems();

protected:
	//~ Begin UUserWidget interface
	virtual TSharedRef<SWidget> RebuildWidget() override;
	//~ End UUserWidget interface

	UPROPERTY(Transient)
	TObjectPtr<UListView> ListView;
};
