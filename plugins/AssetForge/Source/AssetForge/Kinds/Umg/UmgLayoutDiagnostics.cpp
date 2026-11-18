// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Umg/UmgLayoutDiagnostics.h"

namespace UmgLayoutDiagnostics
{

namespace
{
	/** FMargin / FAnchors / FVector2D 在这里只需要数值，用本地结构避免把 SlateCore 拉进算法层。 */
	struct FSlotOffsets
	{
		double Left = 0.0;
		double Top = 0.0;
		double Right = 0.0;
		double Bottom = 0.0;
	};

	struct FRectInfo
	{
		FString Name;
		FString NodePath;
		FVector2D Position = FVector2D::ZeroVector;
		FVector2D Size = FVector2D::ZeroVector;
		bool bSizeEstimated = false;
	};

	FString JoinNodePath(const FString& InParentPath, const FString& InName)
	{
		return InParentPath.IsEmpty() ? InName : FString::Printf(TEXT("%s/%s"), *InParentPath, *InName);
	}

	/**
	 * 读一个二维向量。同时接受引擎导出的 `{"X":..,"Y":..}` 与手写的 `[x, y]`：
	 * 前者是 FJsonObjectConverter 的原生形态（必须支持，否则导出再诊断会失效），
	 * 后者是给人和 AI 的简写。
	 */
	bool ReadVector2D(const TSharedPtr<FJsonObject>& InObject, const TCHAR* InField, FVector2D& OutValue)
	{
		if (!InObject.IsValid())
		{
			return false;
		}

		if (const TSharedPtr<FJsonObject>* StructValue = nullptr; InObject->TryGetObjectField(InField, StructValue))
		{
			double X = 0.0;
			double Y = 0.0;
			(*StructValue)->TryGetNumberField(TEXT("X"), X);
			(*StructValue)->TryGetNumberField(TEXT("Y"), Y);
			OutValue = FVector2D(X, Y);
			return true;
		}

		if (const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr; InObject->TryGetArrayField(InField, ArrayValue))
		{
			if (ArrayValue->Num() >= 2 && (*ArrayValue)[0].IsValid() && (*ArrayValue)[1].IsValid())
			{
				OutValue = FVector2D((*ArrayValue)[0]->AsNumber(), (*ArrayValue)[1]->AsNumber());
				return true;
			}
		}

		return false;
	}

	bool ReadOffsets(const TSharedPtr<FJsonObject>& InObject, const TCHAR* InField, FSlotOffsets& OutValue)
	{
		if (!InObject.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* StructValue = nullptr;
		if (!InObject->TryGetObjectField(InField, StructValue) || !StructValue->IsValid())
		{
			return false;
		}

		(*StructValue)->TryGetNumberField(TEXT("Left"), OutValue.Left);
		(*StructValue)->TryGetNumberField(TEXT("Top"), OutValue.Top);
		(*StructValue)->TryGetNumberField(TEXT("Right"), OutValue.Right);
		(*StructValue)->TryGetNumberField(TEXT("Bottom"), OutValue.Bottom);
		return true;
	}

	/**
	 * 复刻 SConstraintCanvas::OnArrangeChildren 的定位算法
	 * （Runtime/Slate/Private/Widgets/Layout/SConstraintCanvas.cpp:235-290）。
	 *
	 * 为什么不"按直觉"写：UMG 的 Offsets 在锚点拉伸与点锚两种情形下语义不同——
	 * 拉伸时 Right/Bottom 是**到右/下边缘的距离**，点锚时它是**尺寸**。
	 * 用错一种会把所有越界判断都算反。
	 */
	FRectInfo ComputeCanvasRect(const FForgeNode& InChild, const FVector2D& InContainerSize, const FString& InNodePath)
	{
		FRectInfo Info;
		Info.Name = InChild.Name;
		Info.NodePath = InNodePath;

		const TSharedPtr<FJsonObject>& SlotJson = InChild.Slot;

		// UCanvasPanelSlot 的实际存储是 LayoutData（一个 FAnchorData）：Anchors / Offsets /
		// Alignment 是它的**成员**，不是插槽自己的属性——写在外层会找不到属性。
		// 这里对平铺写法也放行（手写时更短），但导出永远产出 LayoutData 形式。
		const TSharedPtr<FJsonObject>* LayoutObject = nullptr;
		const TSharedPtr<FJsonObject> Layout =
			(SlotJson.IsValid() && SlotJson->TryGetObjectField(TEXT("LayoutData"), LayoutObject) && LayoutObject->IsValid())
				? *LayoutObject
				: SlotJson;

		FVector2D AnchorMin = FVector2D::ZeroVector;
		FVector2D AnchorMax = FVector2D::ZeroVector;
		FVector2D Alignment = FVector2D::ZeroVector;
		FSlotOffsets Offsets;
		bool bAutoSize = false;

		if (const TSharedPtr<FJsonObject>* AnchorsObject = nullptr; Layout.IsValid() && Layout->TryGetObjectField(TEXT("Anchors"), AnchorsObject))
		{
			ReadVector2D(*AnchorsObject, TEXT("Minimum"), AnchorMin);
			ReadVector2D(*AnchorsObject, TEXT("Maximum"), AnchorMax);
		}

		ReadOffsets(Layout, TEXT("Offsets"), Offsets);
		ReadVector2D(Layout, TEXT("Alignment"), Alignment);
		if (SlotJson.IsValid())
		{
			SlotJson->TryGetBoolField(TEXT("bAutoSize"), bAutoSize);
		}

		const FVector2D AnchorPixelsMin(AnchorMin.X * InContainerSize.X, AnchorMin.Y * InContainerSize.Y);
		const FVector2D AnchorPixelsMax(AnchorMax.X * InContainerSize.X, AnchorMax.Y * InContainerSize.Y);

		// bAutoSize 时尺寸取决于内容的期望大小，配置里没有这个信息——如实标注为估算，
		// 让调用方知道这个矩形的尺寸不可信，而不是拿 0 当真值去报越界。
		Info.bSizeEstimated = bAutoSize;
		const FVector2D SlotSize(Offsets.Right, Offsets.Bottom);
		const FVector2D Size = bAutoSize ? FVector2D::ZeroVector : SlotSize;

		const bool bDockedX = !FMath::IsNearlyEqual(AnchorMin.X, AnchorMax.X);
		if (bDockedX)
		{
			Info.Position.X = AnchorPixelsMin.X + Offsets.Left;
			Info.Size.X = AnchorPixelsMax.X - Info.Position.X - Offsets.Right;
		}
		else
		{
			Info.Size.X = Size.X;
			Info.Position.X = AnchorPixelsMin.X + Offsets.Left - Size.X * Alignment.X;
		}

		const bool bDockedY = !FMath::IsNearlyEqual(AnchorMin.Y, AnchorMax.Y);
		if (bDockedY)
		{
			Info.Position.Y = AnchorPixelsMin.Y + Offsets.Top;
			Info.Size.Y = AnchorPixelsMax.Y - Info.Position.Y - Offsets.Bottom;
		}
		else
		{
			Info.Size.Y = Size.Y;
			Info.Position.Y = AnchorPixelsMin.Y + Offsets.Top - Size.Y * Alignment.Y;
		}

		return Info;
	}

	double ComputeOverlapArea(const FRectInfo& InA, const FRectInfo& InB)
	{
		const double OverlapX = FMath::Min(InA.Position.X + InA.Size.X, InB.Position.X + InB.Size.X)
			- FMath::Max(InA.Position.X, InB.Position.X);
		const double OverlapY = FMath::Min(InA.Position.Y + InA.Size.Y, InB.Position.Y + InB.Size.Y)
			- FMath::Max(InA.Position.Y, InB.Position.Y);

		if (OverlapX <= 0.0 || OverlapY <= 0.0)
		{
			return 0.0;
		}

		return OverlapX * OverlapY;
	}

	void AnalyzeCanvasChildren(const FForgeNode& InPanel, const FVector2D& InContainerSize, const FString& InNodePath, TArray<FLayoutIssue>& OutIssues)
	{
		TArray<FRectInfo> Rects;
		Rects.Reserve(InPanel.Children.Num());

		for (const FForgeNode& Child : InPanel.Children)
		{
			const FString ChildPath = JoinNodePath(InNodePath, Child.Name);
			const FRectInfo Rect = ComputeCanvasRect(Child, InContainerSize, ChildPath);
			Rects.Add(Rect);

			if (Rect.bSizeEstimated)
			{
				OutIssues.Add({
					EForgeSeverity::Info,
					ChildPath,
					TEXT("该控件设了 bAutoSize，尺寸由内容决定，本次诊断无法估算它的位置；要精确检查请显式设置 Offsets") });
				continue;
			}

			if (Rect.Size.X < 0.0 || Rect.Size.Y < 0.0)
			{
				OutIssues.Add({
					EForgeSeverity::Error,
					ChildPath,
					FString::Printf(
						TEXT("插槽算出的尺寸为负（%.0f x %.0f）：锚点按边缘拉伸时，Offsets 表示到各边的距离，两者相减不该为负"),
						Rect.Size.X,
						Rect.Size.Y) });
				continue;
			}

			const bool bOutside =
				Rect.Position.X + Rect.Size.X <= 0.0
				|| Rect.Position.Y + Rect.Size.Y <= 0.0
				|| Rect.Position.X >= InContainerSize.X
				|| Rect.Position.Y >= InContainerSize.Y;

			if (bOutside)
			{
				OutIssues.Add({
					EForgeSeverity::Warning,
					ChildPath,
					FString::Printf(
						TEXT("控件完全落在 %gx%g 的画面之外：位置 (%.0f, %.0f) 尺寸 (%.0f, %.0f)"),
						InContainerSize.X,
						InContainerSize.Y,
						Rect.Position.X,
						Rect.Position.Y,
						Rect.Size.X,
						Rect.Size.Y) });
			}
		}

		// 同级重叠：UMG 里背景图层叠是常态，所以只在两者面积重叠过半时才提，
		// 否则每份配置都会刷出一堆没人看的信息。
		for (int32 First = 0; First < Rects.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < Rects.Num(); ++Second)
			{
				const FRectInfo& A = Rects[First];
				const FRectInfo& B = Rects[Second];
				if (A.bSizeEstimated || B.bSizeEstimated)
				{
					continue;
				}

				const double AreaA = A.Size.X * A.Size.Y;
				const double AreaB = B.Size.X * B.Size.Y;
				if (AreaA <= 0.0 || AreaB <= 0.0)
				{
					continue;
				}

				const double Overlap = ComputeOverlapArea(A, B);
				if (Overlap > 0.5 * FMath::Min(AreaA, AreaB))
				{
					OutIssues.Add({
						EForgeSeverity::Warning,
						A.NodePath,
						FString::Printf(
							TEXT("与同级控件 `%s` 重叠面积超过较小者的 50%%（重叠 %.0f 平方像素）"),
							*B.Name,
							Overlap) });
				}
			}
		}
	}

	void AnalyzeNode(const FForgeNode& InNode, const FVector2D& InContainerSize, const FString& InNodePath, TArray<FLayoutIssue>& OutIssues)
	{
		if (InNode.ClassName == TEXT("CanvasPanel"))
		{
			AnalyzeCanvasChildren(InNode, InContainerSize, InNodePath, OutIssues);
		}

		// 容器尺寸对子层沿用同一份估算：面板嵌套在 CanvasPanel 里但自身拉伸时，
		// 尺寸确实等于父容器；这是近似，所以越界结论只作为警告而不是错误。
		for (const FForgeNode& Child : InNode.Children)
		{
			AnalyzeNode(Child, InContainerSize, JoinNodePath(InNodePath, Child.Name), OutIssues);
		}
	}
}

void Analyze(const FForgeNode& InRoot, const FVector2D& InScreenSize, TArray<FLayoutIssue>& OutIssues)
{
	if (InScreenSize.X <= 0.0 || InScreenSize.Y <= 0.0)
	{
		return;
	}

	AnalyzeNode(InRoot, InScreenSize, TEXT("root"), OutIssues);
}

} // namespace UmgLayoutDiagnostics
