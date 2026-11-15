// Copyright Epic Games, Inc. All Rights Reserved.

#include "Shot/GameTestShot.h"

#include "Core/GameTestJson.h"
#include "Observe/GameTestWidgetNode.h"
#include "GameTestSettings.h"

#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "ImageCore.h"
#include "Misc/Paths.h"
#include "Widgets/SWindow.h"

namespace
{
	/** 画框用的调色板：够区分十几个控件，颜色饱和、在深浅背景上都看得见。 */
	const FColor BoxPalette[] = {
		FColor(255, 80, 80), FColor(80, 255, 80), FColor(80, 160, 255), FColor(255, 220, 60),
		FColor(255, 100, 255), FColor(60, 255, 240), FColor(255, 160, 40), FColor(160, 120, 255),
		FColor(255, 255, 255), FColor(0, 200, 120),
	};

	constexpr int32 BoxThickness = 2;

	void DrawHorizontalLine(TArray<FColor>& InPixels, const FIntPoint& InSize, int32 InX0, int32 InX1, int32 InY, const FColor& InColor)
	{
		const int32 ClampedY = FMath::Clamp(InY, 0, InSize.Y - 1);
		for (int32 X = FMath::Clamp(InX0, 0, InSize.X - 1); X <= FMath::Clamp(InX1, 0, InSize.X - 1); ++X)
		{
			InPixels[ClampedY * InSize.X + X] = InColor;
		}
	}

	void DrawVerticalLine(TArray<FColor>& InPixels, const FIntPoint& InSize, int32 InY0, int32 InY1, int32 InX, const FColor& InColor)
	{
		const int32 ClampedX = FMath::Clamp(InX, 0, InSize.X - 1);
		for (int32 Y = FMath::Clamp(InY0, 0, InSize.Y - 1); Y <= FMath::Clamp(InY1, 0, InSize.Y - 1); ++Y)
		{
			InPixels[Y * InSize.X + ClampedX] = InColor;
		}
	}

	void DrawFilledRect(TArray<FColor>& InPixels, const FIntPoint& InSize, int32 InX, int32 InY, int32 InWidth, int32 InHeight, const FColor& InColor)
	{
		for (int32 Y = InY; Y < InY + InHeight; ++Y)
		{
			for (int32 X = InX; X < InX + InWidth; ++X)
			{
				if (X >= 0 && X < InSize.X && Y >= 0 && Y < InSize.Y)
				{
					InPixels[Y * InSize.X + X] = InColor;
				}
			}
		}
	}

	/**
	 * 编号用"骰子点"表示：不依赖字体。
	 * 每个框左上角画 (编号) 个 4x4 小方块，编号 1..9；超过 9 的框不画点（图例里仍有坐标）。
	 */
	void DrawIndexPips(TArray<FColor>& InPixels, const FIntPoint& InSize, int32 InX, int32 InY, int32 InIndex, const FColor& InColor)
	{
		if (InIndex < 1 || InIndex > 9)
		{
			return;
		}

		for (int32 Pip = 0; Pip < InIndex; ++Pip)
		{
			DrawFilledRect(InPixels, InSize, InX + 4 + Pip * 6, InY + 4, 4, 4, InColor);
		}
	}

	void CollectBoxes(
		const FGameTestWidgetNode& InNode,
		const FVector2D& InWindowPosition,
		float InScale,
		int32& InOutIndex,
		TArray<TSharedPtr<FJsonValue>>& OutBoxes,
		TArray<FIntRect>& OutRects)
	{
		if (InNode.bHasGeometry && InNode.Size.X > 1.0 && InNode.Size.Y > 1.0)
		{
			// 控件的 Position 是 Slate 的屏幕绝对坐标（含窗口在屏幕上的位置），而截图是
			// 窗口客户区的像素——不减窗口位置的话，框会整体偏移"窗口在屏幕哪"（实测踩过：
			// 窗口放在屏幕中部时，框全跑到图外）。
			const FVector2D LocalPosition = InNode.Position - InWindowPosition;
			const FIntRect Rect(
				FMath::RoundToInt(LocalPosition.X * InScale),
				FMath::RoundToInt(LocalPosition.Y * InScale),
				FMath::RoundToInt((LocalPosition.X + InNode.Size.X) * InScale),
				FMath::RoundToInt((LocalPosition.Y + InNode.Size.Y) * InScale));

			++InOutIndex;
			OutRects.Add(Rect);

			const TSharedPtr<FJsonObject> Box = MakeShared<FJsonObject>();
			Box->SetNumberField(TEXT("index"), InOutIndex);
			Box->SetStringField(TEXT("path"), InNode.Path);

			TArray<TSharedPtr<FJsonValue>> RectValues;
			RectValues.Add(MakeShared<FJsonValueNumber>(Rect.Min.X));
			RectValues.Add(MakeShared<FJsonValueNumber>(Rect.Min.Y));
			RectValues.Add(MakeShared<FJsonValueNumber>(Rect.Max.X - Rect.Min.X));
			RectValues.Add(MakeShared<FJsonValueNumber>(Rect.Max.Y - Rect.Min.Y));
			Box->SetArrayField(TEXT("rect"), RectValues);

			OutBoxes.Add(MakeShared<FJsonValueObject>(Box));
		}

		for (const TSharedPtr<FGameTestWidgetNode>& Child : InNode.Children)
		{
			if (Child.IsValid())
			{
				CollectBoxes(*Child, InWindowPosition, InScale, InOutIndex, OutBoxes, OutRects);
			}
		}
	}
}

FString GameTestShot::GetOutputRootDir()
{
	const UGameTestSettings* Settings = GetDefault<UGameTestSettings>();
	const FString OutputDir = Settings != nullptr ? Settings->OutputDir : TEXT("GameTest");
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / OutputDir);
}

FString GameTestShot::ToProjectRelative(const FString& InAbsolutePath)
{
	FString Relative = InAbsolutePath;
	FPaths::MakePathRelativeTo(Relative, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	return Relative;
}

bool GameTestShot::CaptureGameWindow(
	UWorld* InWorld,
	const FString& InAbsolutePath,
	const TArray<TSharedPtr<FGameTestWidgetNode>>* InAnnotateNodes,
	FString& OutMessage,
	TSharedPtr<FJsonObject>& OutPayload)
{
	if (InWorld == nullptr)
	{
		OutMessage = TEXT("没有正在运行的游戏世界（先 pie.start 或 wait until=pieReady）");
		return false;
	}

	UGameViewportClient* ViewportClient = InWorld->GetGameViewport();
	if (ViewportClient == nullptr)
	{
		OutMessage = TEXT("拿不到 GameViewportClient——游戏还没有自己的视口");
		return false;
	}

	const TSharedPtr<SWindow> Window = ViewportClient->GetWindow();
	if (!Window.IsValid())
	{
		OutMessage = TEXT("拿不到游戏窗口（Slate 未初始化或窗口已销毁）");
		return false;
	}

	if (!FSlateApplication::IsInitialized())
	{
		OutMessage = TEXT("Slate 未初始化，无法截图（-nullrhi 下也拿不到像素）");
		return false;
	}

	TArray<FColor> Pixels;
	FIntVector Size = FIntVector::ZeroValue;
	if (!FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), Pixels, Size))
	{
		OutMessage = TEXT("TakeScreenshot 失败（窗口不可绘？）");
		return false;
	}

	if (Pixels.Num() == 0 || Size.X <= 0 || Size.Y <= 0)
	{
		OutMessage = TEXT("截图为空——多半是 -nullrhi 或窗口尺寸为 0");
		return false;
	}

	const FIntPoint PixelSize(Size.X, Size.Y);

	TArray<TSharedPtr<FJsonValue>> Boxes;
	if (InAnnotateNodes != nullptr && InAnnotateNodes->Num() > 0)
	{
		const FVector2D WindowPosition = Window->GetPositionInScreen();
		const FVector2D WindowSize = Window->GetSizeInScreen();
		const float Scale = WindowSize.X > 1.0 ? static_cast<float>(PixelSize.X) / static_cast<float>(WindowSize.X) : 1.0f;

		TArray<FIntRect> Rects;
		int32 Index = 0;
		for (const TSharedPtr<FGameTestWidgetNode>& Screen : *InAnnotateNodes)
		{
			if (Screen.IsValid())
			{
				CollectBoxes(*Screen, WindowPosition, Scale, Index, Boxes, Rects);
			}
		}

		for (int32 RectIndex = 0; RectIndex < Rects.Num(); ++RectIndex)
		{
			const FIntRect& Rect = Rects[RectIndex];
			const FColor Color = BoxPalette[RectIndex % UE_ARRAY_COUNT(BoxPalette)];

			DrawHorizontalLine(Pixels, PixelSize, Rect.Min.X, Rect.Max.X, Rect.Min.Y, Color);
			DrawHorizontalLine(Pixels, PixelSize, Rect.Min.X, Rect.Max.X, Rect.Max.Y, Color);
			DrawVerticalLine(Pixels, PixelSize, Rect.Min.Y, Rect.Max.Y, Rect.Min.X, Color);
			DrawVerticalLine(Pixels, PixelSize, Rect.Min.Y, Rect.Max.Y, Rect.Max.X, Color);
			DrawIndexPips(Pixels, PixelSize, Rect.Min.X, Rect.Min.Y, RectIndex + 1, Color);
		}
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(InAbsolutePath), /*Tree=*/true);

	const FImageView Image(Pixels.GetData(), PixelSize.X, PixelSize.Y);
	if (!FImageUtils::SaveImageByExtension(*InAbsolutePath, Image))
	{
		OutMessage = FString::Printf(TEXT("写 PNG 失败：%s"), *InAbsolutePath);
		return false;
	}

	OutPayload->SetStringField(TEXT("path"), ToProjectRelative(InAbsolutePath));
	OutPayload->SetStringField(TEXT("absolutePath"), InAbsolutePath);
	OutPayload->SetNumberField(TEXT("width"), PixelSize.X);
	OutPayload->SetNumberField(TEXT("height"), PixelSize.Y);
	if (Boxes.Num() > 0)
	{
		OutPayload->SetArrayField(TEXT("boxes"), Boxes);
	}

	OutMessage = FString::Printf(TEXT("已截图 %dx%d → %s"), PixelSize.X, PixelSize.Y, *ToProjectRelative(InAbsolutePath));
	return true;
}
