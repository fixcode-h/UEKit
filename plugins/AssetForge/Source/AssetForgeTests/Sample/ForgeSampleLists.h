// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/UserWidget.h"
#include "ForgeSampleLists.generated.h"

class UButton;
class UBorder;
class UTextBlock;
class UWidgetTree;

/**
 * 示例界面「列表条目」页（AssetForge 1.4.0）：给 GameTest 1.4 的「这一格怎么认」当靶子。
 * 三种容器各对应录制时的一种情况：
 * - 仓库（TileView `ItemList`）：数据对象有 `ItemId`，录制时自动认，不弹窗；
 * - 邮件（ListView `MailList`）：只有 `SentAt` 不重复、又不像标识，录制时暂停问；
 * - 技能栏（WrapBox `SkillBar`，运行时生成的格子）：单个字段都不唯一，要 `SkillType` + `Level` 组合，录制时暂停问。
 *
 * 条目控件没有蓝图资源，控件树在代码里搭：列表的 EntryWidgetClass 直接填 C++ 类，示例配置不用依赖导入顺序。
 * 条目控件上**不加指向子控件的 UPROPERTY**：GameTest 读条目控件自己的字段当候选（技能格），
 * 对象指针会变成一堆「唯一」的假候选。子控件由 WidgetTree 持有，这里只存弱指针。
 */

/** 仓库里的一件物品（TileView 的数据对象） */
UCLASS()
class UForgeSampleItem : public UObject
{
	GENERATED_BODY()

public:
	/** 配置 Id，不重复：录制时按它认 */
	UPROPERTY(VisibleAnywhere, Category = "ForgeSample")
	int32 ItemId = 0;

	/** 名字会重复（同一种物品放了几格） */
	UPROPERTY(VisibleAnywhere, Category = "ForgeSample")
	FString Name;

	/** 点「使用」会变：录制时标「会变」 */
	UPROPERTY(VisibleAnywhere, Category = "ForgeSample")
	int32 Count = 0;

	UPROPERTY(VisibleAnywhere, Category = "ForgeSample")
	int32 Quality = 1;
};

/** 一封邮件（ListView 的数据对象） */
UCLASS()
class UForgeSampleMail : public UObject
{
	GENERATED_BODY()

public:
	/** 发送时间（yyyyMMddHHmm）：唯一，但名字不像标识，录制时拿不准、暂停问 */
	UPROPERTY(VisibleAnywhere, Category = "ForgeSample")
	int64 SentAt = 0;

	UPROPERTY(VisibleAnywhere, Category = "ForgeSample")
	FString Title;

	UPROPERTY(VisibleAnywhere, Category = "ForgeSample")
	FString Sender;

	UPROPERTY(VisibleAnywhere, Category = "ForgeSample")
	FString Reward;

	UPROPERTY(VisibleAnywhere, Category = "ForgeSample")
	bool bClaimed = false;
};

/** 三种条目控件的共同部分：第一次要用时在代码里搭控件树，结果写进示例界面的 Status */
UCLASS(Abstract)
class UForgeSampleEntry : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

	/** 控件树还没搭就搭（RebuildWidget 和列表设数据对象都可能先来） */
	void EnsureBuilt();
	virtual void BuildTree(UWidgetTree& InTree) {}
	virtual void Refresh() {}

	/** 写进外层 ForgeSampleScreen 的 Status（GameTest 断言它） */
	void Report(const FString& InText) const;

private:
	bool bBuilt = false;
};

/** 仓库一格：图标、名字、数量、「使用」 */
UCLASS()
class UForgeBagSlot : public UForgeSampleEntry, public IUserObjectListEntry
{
	GENERATED_BODY()

protected:
	virtual void NativeOnListItemObjectSet(UObject* ListItemObject) override;
	virtual void BuildTree(UWidgetTree& InTree) override;
	virtual void Refresh() override;

private:
	UFUNCTION()
	void OnUse();

	TWeakObjectPtr<UForgeSampleItem> Item;
	TWeakObjectPtr<UBorder> Icon;
	TWeakObjectPtr<UTextBlock> Glyph;
	TWeakObjectPtr<UTextBlock> NameText;
	TWeakObjectPtr<UTextBlock> CountText;
};

/** 邮件一行：标题、发件人和时间、状态、「领取」 */
UCLASS()
class UForgeMailRow : public UForgeSampleEntry, public IUserObjectListEntry
{
	GENERATED_BODY()

protected:
	virtual void NativeOnListItemObjectSet(UObject* ListItemObject) override;
	virtual void BuildTree(UWidgetTree& InTree) override;
	virtual void Refresh() override;

private:
	UFUNCTION()
	void OnClaim();

	TWeakObjectPtr<UForgeSampleMail> Mail;
	TWeakObjectPtr<UTextBlock> TitleText;
	TWeakObjectPtr<UTextBlock> MetaText;
	TWeakObjectPtr<UTextBlock> StateText;
};

/**
 * 技能栏一格：运行时 CreateWidget 再 AddChild 的（名字是「类名_序号」，GameTest 认作容器条目）。
 * 字段只有 SkillType、Level 两个，各自都重复，两个一起才唯一。
 */
UCLASS()
class UForgeSkillCell : public UForgeSampleEntry
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = "ForgeSample")
	int32 SkillType = 0;

	UPROPERTY(VisibleAnywhere, Category = "ForgeSample")
	int32 Level = 1;

protected:
	virtual void BuildTree(UWidgetTree& InTree) override;
	virtual void Refresh() override;

private:
	UFUNCTION()
	void OnCast();

	TWeakObjectPtr<UBorder> Frame;
	TWeakObjectPtr<UTextBlock> Glyph;
	TWeakObjectPtr<UTextBlock> NameText;
	TWeakObjectPtr<UBorder> Pip2;
};

namespace ForgeSampleLists
{
	/** 示例界面构造时调：列表还空着就填数据、生成技能格（重复加进视口不重复填） */
	void Populate(UUserWidget& InScreen);
}
