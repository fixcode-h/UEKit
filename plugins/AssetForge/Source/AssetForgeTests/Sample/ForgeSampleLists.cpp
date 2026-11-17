// Copyright Epic Games, Inc. All Rights Reserved.

#include "Sample/ForgeSampleLists.h"

#include "Sample/ForgeSampleScreen.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ListView.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"

namespace ForgeSampleListsDetail
{
	// 配色和 gen.mjs 的 T 一致（sRGB 十六进制）
	FLinearColor Hex(const TCHAR* InHex)
	{
		return FLinearColor::FromSRGBColor(FColor::FromHex(InHex));
	}

	FSlateBrush Rounded(const FLinearColor& InColor, float InRadius, const TCHAR* InOutline = nullptr)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.TintColor = FSlateColor(InColor);
		Brush.OutlineSettings.CornerRadii = FVector4(InRadius, InRadius, InRadius, InRadius);
		Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
		if (InOutline != nullptr)
		{
			Brush.OutlineSettings.Color = FSlateColor(Hex(InOutline));
			Brush.OutlineSettings.Width = 1.0f;
		}
		return Brush;
	}

	UTextBlock* MakeText(UWidgetTree& InTree, const TCHAR* InName, int32 InSize, bool bInBold, const TCHAR* InColor)
	{
		UTextBlock* Text = InTree.ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), InName);
		FSlateFontInfo Font = Text->GetFont();
		Font.Size = InSize;
		Font.TypefaceFontName = bInBold ? FName(TEXT("Bold")) : FName(TEXT("Regular"));
		Text->SetFont(Font);
		Text->SetColorAndOpacity(FSlateColor(Hex(InColor)));
		return Text;
	}

	UButton* MakeButton(UWidgetTree& InTree, const TCHAR* InName, const TCHAR* InLabel, const TCHAR* InColor)
	{
		UButton* Button = InTree.ConstructWidget<UButton>(UButton::StaticClass(), InName);
		Button->SetBackgroundColor(Hex(InColor));
		UTextBlock* Label = MakeText(InTree, *FString::Printf(TEXT("L_%s"), InName), 12, true, TEXT("E8ECF4"));
		Label->SetText(FText::FromString(InLabel));
		Button->SetContent(Label);
		return Button;
	}

	UBorder* MakeBorder(UWidgetTree& InTree, const TCHAR* InName, const FSlateBrush& InBrush, const FMargin& InPadding)
	{
		UBorder* Border = InTree.ConstructWidget<UBorder>(UBorder::StaticClass(), InName);
		Border->SetBrush(InBrush);
		Border->SetPadding(InPadding);
		return Border;
	}

	UVerticalBoxSlot* AddRow(UVerticalBox& InBox, UWidget* InChild, float InBottom, EHorizontalAlignment InAlign = HAlign_Center)
	{
		UVerticalBoxSlot* Slot = InBox.AddChildToVerticalBox(InChild);
		Slot->SetHorizontalAlignment(InAlign);
		Slot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, InBottom));
		return Slot;
	}

	const TCHAR* QualityColor(int32 InQuality)
	{
		switch (InQuality)
		{
		case 2: return TEXT("5FD39A");
		case 3: return TEXT("6FB2FF");
		case 4: return TEXT("C38BFF");
		default: return TEXT("8D97AB");
		}
	}

	// 物品名 12 种，48 格里每种放 4 格：名字不能当标识
	const TCHAR* const ItemNames[] = {
		TEXT("生命药水"), TEXT("法力药水"), TEXT("疾行药剂"), TEXT("万能灵药"), TEXT("铁剑"), TEXT("猎弓"),
		TEXT("橡木法杖"), TEXT("战斧"), TEXT("皮甲"), TEXT("铁盔"), TEXT("护身符"), TEXT("传送卷轴"),
	};
	constexpr int32 ItemNameCount = UE_ARRAY_COUNT(ItemNames);
	constexpr int32 ItemCount = 48;
	constexpr int32 FirstItemId = 1001;

	const TCHAR* const MailTitles[] = { TEXT("系统奖励"), TEXT("活动奖励"), TEXT("补偿邮件") };
	const TCHAR* const MailSenders[] = { TEXT("系统"), TEXT("运营团队") };
	const TCHAR* const MailRewards[] = { TEXT("金币 ×500"), TEXT("钻石 ×20"), TEXT("体力 ×60"), TEXT("经验 ×1200") };
	constexpr int32 MailCount = 10;
	/** 2026-11-10 09:00 起每封隔一小时 */
	constexpr int64 FirstSentAt = 202611100900;

	// 技能格：(SkillType, Level) 两两不同；各自都重复。Level 不和格子序号相等（GameTest 会把「等于序号」的值当成像位置）
	const int32 SkillTypes[] = { 11, 12, 13, 14, 13, 11, 12, 14 };
	const int32 SkillLevels[] = { 1, 2, 1, 1, 2, 2, 1, 2 };
	constexpr int32 SkillCount = UE_ARRAY_COUNT(SkillTypes);
	static_assert(UE_ARRAY_COUNT(SkillTypes) == UE_ARRAY_COUNT(SkillLevels), "技能格的类型和等级要一一对应");

	FString SkillName(int32 InType)
	{
		switch (InType)
		{
		case 11: return TEXT("火球");
		case 12: return TEXT("冰锥");
		case 13: return TEXT("治疗");
		case 14: return TEXT("护盾");
		default: return TEXT("技能");
		}
	}

	const TCHAR* SkillColor(int32 InType)
	{
		switch (InType)
		{
		case 11: return TEXT("FF7A7A");
		case 12: return TEXT("6FB2FF");
		case 13: return TEXT("5FD39A");
		default: return TEXT("F5C451");
		}
	}

	/** 202611101400 → 11-10 14:00 */
	FString FormatSentAt(int64 InSentAt)
	{
		const int64 Minute = InSentAt % 100;
		const int64 Hour = (InSentAt / 100) % 100;
		const int64 Day = (InSentAt / 10000) % 100;
		const int64 Month = (InSentAt / 1000000) % 100;
		return FString::Printf(TEXT("%02lld-%02lld %02lld:%02lld"), Month, Day, Hour, Minute);
	}
}

// ---- 共同部分 ----

TSharedRef<SWidget> UForgeSampleEntry::RebuildWidget()
{
	EnsureBuilt();
	return Super::RebuildWidget();
}

void UForgeSampleEntry::EnsureBuilt()
{
	if (bBuilt)
	{
		return;
	}
	if (WidgetTree == nullptr)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"), RF_Transient);
	}
	bBuilt = true;
	if (WidgetTree->RootWidget == nullptr)
	{
		BuildTree(*WidgetTree);
	}
	Refresh();
}

void UForgeSampleEntry::Report(const FString& InText) const
{
	// 列表条目和运行时生成的格子 Outer 都在示例界面的 WidgetTree 下
	if (UForgeSampleScreen* Screen = GetTypedOuter<UForgeSampleScreen>())
	{
		Screen->ReportStatus(InText);
	}
}

// ---- 仓库一格 ----

void UForgeBagSlot::NativeOnListItemObjectSet(UObject* ListItemObject)
{
	IUserObjectListEntry::NativeOnListItemObjectSet(ListItemObject);
	Item = Cast<UForgeSampleItem>(ListItemObject);
	EnsureBuilt();
	Refresh();
}

void UForgeBagSlot::BuildTree(UWidgetTree& InTree)
{
	using namespace ForgeSampleListsDetail;
	UBorder* Bg = MakeBorder(InTree, TEXT("SlotBg"), Rounded(Hex(TEXT("1E2534")), 10.0f, TEXT("2A3346")), FMargin(8.0f));
	InTree.RootWidget = Bg;

	UVerticalBox* Col = InTree.ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("SlotCol"));
	Bg->SetContent(Col);

	UBorder* IconBorder = MakeBorder(InTree, TEXT("Icon"), Rounded(Hex(TEXT("2A3550")), 8.0f), FMargin(14.0f, 6.0f));
	UTextBlock* GlyphText = MakeText(InTree, TEXT("Glyph"), 20, true, TEXT("8D97AB"));
	IconBorder->SetContent(GlyphText);
	AddRow(*Col, IconBorder, 6.0f);

	UTextBlock* Name = MakeText(InTree, TEXT("NameText"), 12, false, TEXT("E8ECF4"));
	AddRow(*Col, Name, 2.0f);
	UTextBlock* CountLabel = MakeText(InTree, TEXT("CountText"), 11, false, TEXT("8D97AB"));
	AddRow(*Col, CountLabel, 6.0f);

	UButton* Use = MakeButton(InTree, TEXT("BtnUse"), TEXT("使用"), TEXT("323C52"));
	Use->OnClicked.AddDynamic(this, &UForgeBagSlot::OnUse);
	AddRow(*Col, Use, 0.0f, HAlign_Fill);

	Icon = IconBorder;
	Glyph = GlyphText;
	NameText = Name;
	CountText = CountLabel;
}

void UForgeBagSlot::Refresh()
{
	using namespace ForgeSampleListsDetail;
	const UForgeSampleItem* Data = Item.Get();
	if (Data == nullptr)
	{
		return;
	}
	if (UTextBlock* Text = Glyph.Get())
	{
		Text->SetText(FText::FromString(Data->Name.Left(1)));
		Text->SetColorAndOpacity(FSlateColor(Hex(QualityColor(Data->Quality))));
	}
	if (UTextBlock* Text = NameText.Get())
	{
		Text->SetText(FText::FromString(Data->Name));
	}
	if (UTextBlock* Text = CountText.Get())
	{
		Text->SetText(FText::FromString(FString::Printf(TEXT("×%d"), Data->Count)));
	}
}

void UForgeBagSlot::OnUse()
{
	UForgeSampleItem* Data = Item.Get();
	if (Data == nullptr)
	{
		return;
	}
	if (Data->Count <= 0)
	{
		Report(FString::Printf(TEXT("「%s」用完了（ItemId %d）"), *Data->Name, Data->ItemId));
		return;
	}
	--Data->Count;
	Refresh();
	Report(FString::Printf(TEXT("使用「%s」（ItemId %d），还剩 %d"), *Data->Name, Data->ItemId, Data->Count));
}

// ---- 邮件一行 ----

void UForgeMailRow::NativeOnListItemObjectSet(UObject* ListItemObject)
{
	IUserObjectListEntry::NativeOnListItemObjectSet(ListItemObject);
	Mail = Cast<UForgeSampleMail>(ListItemObject);
	EnsureBuilt();
	Refresh();
}

void UForgeMailRow::BuildTree(UWidgetTree& InTree)
{
	using namespace ForgeSampleListsDetail;
	// 外面包一层不画的边框留出行间距：ListView 的条目一行挨一行
	FSlateBrush None;
	None.DrawAs = ESlateBrushDrawType::NoDrawType;
	UBorder* Pad = MakeBorder(InTree, TEXT("RowPad"), None, FMargin(0.0f, 0.0f, 0.0f, 6.0f));
	InTree.RootWidget = Pad;
	UBorder* Bg = MakeBorder(InTree, TEXT("RowBg"), Rounded(Hex(TEXT("1E2534")), 8.0f), FMargin(12.0f, 8.0f));
	Pad->SetContent(Bg);

	UHorizontalBox* Cols = InTree.ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("RowCols"));
	Bg->SetContent(Cols);

	UVerticalBox* TextCol = InTree.ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("RowText"));
	UTextBlock* Title = MakeText(InTree, TEXT("TitleText"), 13, true, TEXT("E8ECF4"));
	AddRow(*TextCol, Title, 2.0f, HAlign_Left);
	UTextBlock* Meta = MakeText(InTree, TEXT("MetaText"), 11, false, TEXT("8D97AB"));
	AddRow(*TextCol, Meta, 0.0f, HAlign_Left);
	UHorizontalBoxSlot* TextSlot = Cols->AddChildToHorizontalBox(TextCol);
	TextSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	TextSlot->SetVerticalAlignment(VAlign_Center);

	UTextBlock* State = MakeText(InTree, TEXT("StateText"), 11, false, TEXT("F2A93B"));
	UHorizontalBoxSlot* StateSlot = Cols->AddChildToHorizontalBox(State);
	StateSlot->SetVerticalAlignment(VAlign_Center);
	StateSlot->SetPadding(FMargin(8.0f, 0.0f, 10.0f, 0.0f));

	UButton* Claim = MakeButton(InTree, TEXT("BtnClaim"), TEXT("领取"), TEXT("36C97A"));
	Claim->OnClicked.AddDynamic(this, &UForgeMailRow::OnClaim);
	UHorizontalBoxSlot* ClaimSlot = Cols->AddChildToHorizontalBox(Claim);
	ClaimSlot->SetVerticalAlignment(VAlign_Center);

	TitleText = Title;
	MetaText = Meta;
	StateText = State;
}

void UForgeMailRow::Refresh()
{
	using namespace ForgeSampleListsDetail;
	const UForgeSampleMail* Data = Mail.Get();
	if (Data == nullptr)
	{
		return;
	}
	if (UTextBlock* Text = TitleText.Get())
	{
		Text->SetText(FText::FromString(FString::Printf(TEXT("%s · %s"), *Data->Title, *Data->Reward)));
	}
	if (UTextBlock* Text = MetaText.Get())
	{
		Text->SetText(FText::FromString(FString::Printf(TEXT("%s · %s"), *Data->Sender, *FormatSentAt(Data->SentAt))));
	}
	if (UTextBlock* Text = StateText.Get())
	{
		Text->SetText(FText::FromString(Data->bClaimed ? TEXT("已领") : TEXT("未领")));
		Text->SetColorAndOpacity(FSlateColor(Hex(Data->bClaimed ? TEXT("8D97AB") : TEXT("F2A93B"))));
	}
}

void UForgeMailRow::OnClaim()
{
	using namespace ForgeSampleListsDetail;
	UForgeSampleMail* Data = Mail.Get();
	if (Data == nullptr)
	{
		return;
	}
	if (Data->bClaimed)
	{
		Report(FString::Printf(TEXT("「%s」已经领过了（%s 发来）"), *Data->Title, *FormatSentAt(Data->SentAt)));
		return;
	}
	Data->bClaimed = true;
	Refresh();
	Report(FString::Printf(TEXT("领取「%s」：%s（%s 发来）"), *Data->Title, *Data->Reward, *FormatSentAt(Data->SentAt)));
}

// ---- 技能格 ----

void UForgeSkillCell::BuildTree(UWidgetTree& InTree)
{
	using namespace ForgeSampleListsDetail;
	UBorder* Bg = MakeBorder(InTree, TEXT("CellBg"), Rounded(Hex(TEXT("1E2534")), 10.0f, TEXT("2A3346")), FMargin(10.0f, 8.0f));
	InTree.RootWidget = Bg;

	USizeBox* Width = InTree.ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("CellWidth"));
	Width->SetWidthOverride(84.0f);
	Bg->SetContent(Width);

	UVerticalBox* Col = InTree.ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("CellCol"));
	Width->SetContent(Col);

	UBorder* IconBorder = MakeBorder(InTree, TEXT("CellIcon"), Rounded(Hex(TEXT("2A3550")), 8.0f), FMargin(14.0f, 4.0f));
	UTextBlock* GlyphText = MakeText(InTree, TEXT("CellGlyph"), 18, true, TEXT("E8ECF4"));
	IconBorder->SetContent(GlyphText);
	AddRow(*Col, IconBorder, 4.0f);

	UTextBlock* Name = MakeText(InTree, TEXT("CellName"), 12, false, TEXT("E8ECF4"));
	AddRow(*Col, Name, 4.0f);

	// 等级画成两个小圆点、不写成字：写成字的话条目上的文字就唯一了，演示不到「要两个字段组合」
	UHorizontalBox* Pips = InTree.ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("CellPips"));
	for (int32 Index = 0; Index < 2; ++Index)
	{
		UBorder* Pip = MakeBorder(InTree, Index == 0 ? TEXT("Pip1") : TEXT("Pip2"), Rounded(Hex(TEXT("F5C451")), 3.0f), FMargin(6.0f, 3.0f));
		UHorizontalBoxSlot* PipSlot = Pips->AddChildToHorizontalBox(Pip);
		PipSlot->SetPadding(FMargin(2.0f, 0.0f));
		if (Index == 1)
		{
			Pip2 = Pip;
		}
	}
	AddRow(*Col, Pips, 6.0f);

	UButton* CastButton = MakeButton(InTree, TEXT("BtnCast"), TEXT("施放"), TEXT("5A95FF"));
	CastButton->OnClicked.AddDynamic(this, &UForgeSkillCell::OnCast);
	AddRow(*Col, CastButton, 0.0f, HAlign_Fill);

	Frame = Bg;
	Glyph = GlyphText;
	NameText = Name;
}

void UForgeSkillCell::Refresh()
{
	using namespace ForgeSampleListsDetail;
	const FString Name = SkillName(SkillType);
	if (UTextBlock* Text = Glyph.Get())
	{
		Text->SetText(FText::FromString(Name.Left(1)));
		Text->SetColorAndOpacity(FSlateColor(Hex(SkillColor(SkillType))));
	}
	if (UTextBlock* Text = NameText.Get())
	{
		Text->SetText(FText::FromString(Name));
	}
	if (UBorder* Pip = Pip2.Get())
	{
		Pip->SetBrush(Rounded(Hex(Level >= 2 ? TEXT("F5C451") : TEXT("2A3346")), 3.0f));
	}
	if (UBorder* Bg = Frame.Get())
	{
		Bg->SetBrush(Rounded(Hex(TEXT("1E2534")), 10.0f, Level >= 2 ? TEXT("8A6A2A") : TEXT("2A3346")));
	}
}

void UForgeSkillCell::OnCast()
{
	Report(FString::Printf(TEXT("施放「%s」Lv%d（SkillType %d）"), *ForgeSampleListsDetail::SkillName(SkillType), Level, SkillType));
}

// ---- 填数据 ----

void ForgeSampleLists::Populate(UUserWidget& InScreen)
{
	using namespace ForgeSampleListsDetail;
	if (InScreen.WidgetTree == nullptr)
	{
		return;
	}

	// TileView 是 ListView 的子类，一起按 ListView 找
	if (UListView* Bag = Cast<UListView>(InScreen.WidgetTree->FindWidget(TEXT("ItemList"))))
	{
		if (Bag->GetNumItems() == 0)
		{
			TArray<UObject*> Items;
			for (int32 Index = 0; Index < ItemCount; ++Index)
			{
				UForgeSampleItem* Item = NewObject<UForgeSampleItem>(&InScreen);
				Item->ItemId = FirstItemId + Index;
				Item->Name = ItemNames[Index % ItemNameCount];
				Item->Count = (Index * 7) % 15 + 1;
				Item->Quality = Index % 4 + 1;
				Items.Add(Item);
			}
			Bag->SetListItems(Items);
		}
	}

	if (UListView* Mails = Cast<UListView>(InScreen.WidgetTree->FindWidget(TEXT("MailList"))))
	{
		if (Mails->GetNumItems() == 0)
		{
			TArray<UObject*> Items;
			for (int32 Index = 0; Index < MailCount; ++Index)
			{
				UForgeSampleMail* Mail = NewObject<UForgeSampleMail>(&InScreen);
				Mail->SentAt = FirstSentAt + Index * 100;
				Mail->Title = MailTitles[Index % static_cast<int32>(UE_ARRAY_COUNT(MailTitles))];
				Mail->Sender = MailSenders[Index % static_cast<int32>(UE_ARRAY_COUNT(MailSenders))];
				Mail->Reward = MailRewards[Index % static_cast<int32>(UE_ARRAY_COUNT(MailRewards))];
				Items.Add(Mail);
			}
			Mails->SetListItems(Items);
		}
	}

	if (UWrapBox* Bar = Cast<UWrapBox>(InScreen.WidgetTree->FindWidget(TEXT("SkillBar"))))
	{
		if (Bar->GetChildrenCount() == 0)
		{
			for (int32 Index = 0; Index < SkillCount; ++Index)
			{
				// 不给名字：名字是「ForgeSkillCell_序号」，和 Lua 里 new 出来再 AddChild 的一样，GameTest 认作容器条目
				UForgeSkillCell* Cell = CreateWidget<UForgeSkillCell>(&InScreen, UForgeSkillCell::StaticClass());
				if (Cell == nullptr)
				{
					continue;
				}
				Cell->SkillType = SkillTypes[Index];
				Cell->Level = SkillLevels[Index];
				if (UWrapBoxSlot* CellSlot = Bar->AddChildToWrapBox(Cell))
				{
					CellSlot->SetPadding(FMargin(0.0f, 0.0f, 10.0f, 10.0f));
				}
			}
		}
	}
}
