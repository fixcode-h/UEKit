 // Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Umg/UmgWidgetCodec.h"

#include "Core/ForgeClassResolver.h"
#include "Core/ForgePropertyCodec.h"

#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Blueprint/WidgetTree.h"

#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UmgWidgetCodec
{

namespace
{
	/** 短名按这个顺序找。引擎自带控件的家在 UMG，CommonUI 的控件是第二个常见来源。 */
	const TArray<FString> ShortNamePrefixes =
	{
		TEXT("/Script/UMG."),
		TEXT("/Script/CommonUI."),
	};

	/**
	 * 导出时控件类怎么写。
	 *
	 * 引擎与 CommonUI 的类写短名（`TextBlock`），其余（项目自定义 C++ 类、蓝图类）写全路径——
	 * 短名只在已知前缀下才有意义，对第三方类名做猜测会把「找不到」变成「找到了别的」。
	 */
	FString DescribeWidgetClass(const UClass* InClass)
	{
		if (InClass == nullptr)
		{
			return FString();
		}

		const FString PackageName = InClass->GetOutermost() != nullptr ? InClass->GetOutermost()->GetName() : FString();
		if (PackageName == TEXT("/Script/UMG") || PackageName == TEXT("/Script/CommonUI"))
		{
			return InClass->GetName();
		}

		return InClass->GetPathName();
	}
}

UClass* ResolveWidgetClass(const FString& InClassName, FString& OutError)
{
	if (InClassName.IsEmpty())
	{
		OutError = TEXT("控件类名为空");
		return nullptr;
	}

	// 全路径（`/Script/...`、`/Game/...`）：交给统一解析器——它同时认识 UnrealSharp 的运行时类
	// （C# 控件族不在包资产里，`LoadClass` 能否命中取决于加载时机，解析器里有按名兜底）。
	if (InClassName.Contains(TEXT("/")) || InClassName.Contains(TEXT(".")))
	{
		return FForgeClassResolver::Resolve(InClassName, UWidget::StaticClass(), OutError);
	}

	return FForgeClassResolver::ResolveShortName(InClassName, ShortNamePrefixes, UWidget::StaticClass(), OutError);
}

void ExportWidget(UWidget* InWidget, FForgeNode& OutNode, TArray<FForgeDiagnostic>& OutDiagnostics, const FString& InNodePath)
{
	if (InWidget == nullptr)
	{
		return;
	}

	OutNode.Name = InWidget->GetName();
	OutNode.ClassName = DescribeWidgetClass(InWidget->GetClass());

	ForgePropertyCodec::ExportDeltaProperties(
		InWidget,
		OutNode.Props,
		OutDiagnostics,
		FString::Printf(TEXT("%s.props"), *InNodePath));

	// Slot 不是 child widget：它是面板随子控件一起实例化的 UPanelSlot，挂在控件自己身上。
	if (UPanelSlot* Slot = InWidget->Slot)
	{
		ForgePropertyCodec::ExportDeltaProperties(
			Slot,
			OutNode.Slot,
			OutDiagnostics,
			FString::Printf(TEXT("%s.slot"), *InNodePath));
	}

	if (const UPanelWidget* Panel = Cast<UPanelWidget>(InWidget))
	{
		const int32 ChildCount = Panel->GetChildrenCount();
		OutNode.Children.Reserve(ChildCount);

		for (int32 ChildIndex = 0; ChildIndex < ChildCount; ++ChildIndex)
		{
			UWidget* Child = Panel->GetChildAt(ChildIndex);
			if (Child == nullptr)
			{
				continue;
			}

			FForgeNode ChildNode;
			ExportWidget(
				Child,
				ChildNode,
				OutDiagnostics,
				FString::Printf(TEXT("%s.children[%d]"), *InNodePath, ChildIndex));
			OutNode.Children.Add(MoveTemp(ChildNode));
		}
	}
}

void ImportWidgetInto(UWidget* InWidget, const FForgeNode& InNode, TArray<FForgeDiagnostic>& OutDiagnostics, const FString& InNodePath)
{
	if (InWidget == nullptr)
	{
		return;
	}

	// 先回到默认再应用配置：配置没写的属性不该在资产里残留旧值（导出方向是 delta 的，
	// 残留的那部分既导不出来也说不清来源）。
	ForgePropertyCodec::ResetToDefaults(InWidget);

	ForgePropertyCodec::ImportProperties(
		InWidget,
		InNode.Props,
		OutDiagnostics,
		FString::Printf(TEXT("%s.props"), *InNodePath));

	UPanelSlot* Slot = InWidget->Slot;
	const bool bHasSlotConfig = InNode.Slot.IsValid() && InNode.Slot->Values.Num() > 0;

	if (Slot == nullptr)
	{
		// 没有插槽（控件不是任何面板的子节点）时，只有配置确实写了 slot 才值得提醒；
		// 省略 slot 本来就是常态。
		if (bHasSlotConfig)
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Warning,
				FString::Printf(TEXT("%s.slot"), *InNodePath),
				FString::Printf(
					TEXT("控件 `%s` 没有插槽（它不是任何面板的子节点），slot 字段被忽略"),
					*InNode.Name));
		}
	}
	else
	{
		// 两种情况下都要先重置：写了的按配置填，**没写的回默认**。少了「没写也重置」
		// 这一步，资产里的旧槽位值会永远留着——导出每次都算出同一个差异、导入又改不动它，
		// 表现成「每次导入都报 update，资产却怎么都不收敛」。
		ForgePropertyCodec::ResetToDefaults(Slot);
		if (bHasSlotConfig)
		{
			ForgePropertyCodec::ImportProperties(
				Slot,
				InNode.Slot,
				OutDiagnostics,
				FString::Printf(TEXT("%s.slot"), *InNodePath));
		}
	}
}

UWidget* FindWidgetByName(UWidgetTree* InTree, const FString& InName)
{
	if (InTree == nullptr || InName.IsEmpty())
	{
		return nullptr;
	}

	return InTree->FindWidget(FName(*InName));
}

} // namespace UmgWidgetCodec
