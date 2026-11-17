// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/GameTestAutoScroll.h"

#include "GameTest.h"
#include "Observe/GameTestSelector.h"
#include "Observe/GameTestSlate.h"

#include "Components/ListView.h"
#include "Components/ScrollBox.h"
#include "Components/Widget.h"
#include "Widgets/SWidget.h"

namespace GameTestAutoScrollDetail
{
	/** 一步里最多滚几次滚动框：嵌套的滚动框一层一次，再多说明滚了也看不见（被裁掉、尺寸算错），别一直拖。 */
	constexpr int32 MaxScrollBoxAttempts = 4;

	void AddNote(GameTestAutoScroll::FState& InOutState, const FString& InNote)
	{
		InOutState.Note = InOutState.Note.IsEmpty() ? InNote : InOutState.Note + TEXT("；") + InNote;
	}

	/** 目标的矩形是不是整个落在滚动框的矩形里（绝对坐标，留 1 像素误差）。 */
	bool IsInside(const SWidget& InTarget, const SWidget& InBox)
	{
		const FGeometry& Target = InTarget.GetCachedGeometry();
		const FGeometry& Box = InBox.GetCachedGeometry();
		const FVector2D TargetMin = FVector2D(Target.GetAbsolutePosition());
		const FVector2D TargetMax = TargetMin + FVector2D(Target.GetAbsoluteSize());
		const FVector2D BoxMin = FVector2D(Box.GetAbsolutePosition());
		const FVector2D BoxMax = BoxMin + FVector2D(Box.GetAbsoluteSize());
		return TargetMin.X >= BoxMin.X - 1.0 && TargetMin.Y >= BoxMin.Y - 1.0
			&& TargetMax.X <= BoxMax.X + 1.0 && TargetMax.Y <= BoxMax.Y + 1.0;
	}
}

bool GameTestAutoScroll::ScrollToUngeneratedEntry(FState& InOutState)
{
	using namespace GameTestAutoScrollDetail;
	TWeakObjectPtr<UListView> WeakList;
	int32 Index = INDEX_NONE;
	FString Path;
	if (!GameTestSelector::TakeUngeneratedEntry(WeakList, Index, Path))
	{
		return false;
	}
	UListView* List = WeakList.Get();
	if (List == nullptr)
	{
		return false;
	}
	const FString Key = FString::Printf(TEXT("%s#%d"), *Path, Index);
	if (InOutState.ScrolledEntries.Contains(Key))
	{
		return false;
	}
	InOutState.ScrolledEntries.Add(Key);
	List->ScrollIndexIntoView(Index);
	AddNote(InOutState, FString::Printf(TEXT("列表 %s 的第 %d 个条目没生成，自动滚到它"), *Path, Index));
	UE_LOG(LogGameTest, Display, TEXT("自动滚动：列表 %s 滚到第 %d 个条目"), *Path, Index);
	return true;
}

bool GameTestAutoScroll::ScrollIntoScrollBox(const TSharedPtr<SWidget>& InTarget, FState& InOutState)
{
	using namespace GameTestAutoScrollDetail;
	if (!InTarget.IsValid() || InOutState.ScrollBoxAttempts >= MaxScrollBoxAttempts)
	{
		return false;
	}
	UWidget* TargetWidget = GameTestSlate::FindOwningWidget(InTarget);
	if (TargetWidget == nullptr)
	{
		return false;
	}
	// 由里往外找第一个没把目标放进可见范围的滚动框：嵌套的滚动框先滚里层。
	for (TSharedPtr<SWidget> Current = InTarget->GetParentWidget(); Current.IsValid(); Current = Current->GetParentWidget())
	{
		UScrollBox* Box = Cast<UScrollBox>(GameTestSlate::FindSourceWidget(*Current));
		if (Box == nullptr)
		{
			continue;
		}
		if (IsInside(*InTarget, *Current))
		{
			continue;
		}
		++InOutState.ScrollBoxAttempts;
		Box->ScrollWidgetIntoView(TargetWidget, /*AnimateScroll=*/false);
		AddNote(InOutState, FString::Printf(TEXT("%s 在滚动框 %s 的可见范围外，自动滚过去"), *TargetWidget->GetName(), *Box->GetName()));
		UE_LOG(LogGameTest, Display, TEXT("自动滚动：滚动框 %s 滚到 %s"), *Box->GetName(), *TargetWidget->GetName());
		return true;
	}
	return false;
}
