// Copyright Epic Games, Inc. All Rights Reserved.

#include "Integration/Live/GameTestLive.h"

#include "Observe/GameTestSlate.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "Misc/Base64.h"
#include "Misc/Crc.h"
#include "Modules/ModuleManager.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace GameTestLiveFrameDetail
{
	/** 视口在截图里的像素区域：PIE 在关卡编辑器视口里时「游戏窗口」是整个编辑器，只要视口那一块。 */
	FIntRect ViewportRect(const SWindow& InWindow, const SWidget& InViewport, const FIntVector& InShotSize)
	{
		const FVector2D WindowPos = FVector2D(InWindow.GetPositionInScreen());
		const FVector2D WindowSize = FVector2D(InWindow.GetSizeInScreen());
		const double Scale = WindowSize.X > 1.0 ? static_cast<double>(InShotSize.X) / WindowSize.X : 1.0;
		const FGeometry& Geometry = InViewport.GetCachedGeometry();
		const FVector2D Min = (FVector2D(Geometry.GetAbsolutePosition()) - WindowPos) * Scale;
		const FVector2D Max = Min + FVector2D(Geometry.GetAbsoluteSize()) * Scale;
		FIntRect Rect(FMath::RoundToInt(Min.X), FMath::RoundToInt(Min.Y), FMath::RoundToInt(Max.X), FMath::RoundToInt(Max.Y));
		Rect.Clip(FIntRect(0, 0, InShotSize.X, InShotSize.Y));
		return Rect;
	}
}

bool GameTestLiveFrame::Capture(int32 InMaxWidth, int32 InQuality, uint32 InLastHash, TSharedPtr<FJsonObject>& OutFrame, uint32& OutHash, FString& OutError)
{
	OutFrame.Reset();
	if (!FSlateApplication::IsInitialized())
	{
		OutError = TEXT("这个进程没有 Slate（命令行或 -nullrhi），没有画面可看");
		return false;
	}
	const TSharedPtr<SWindow> Window = GameTestSlate::GetGameWindow();
	const TSharedPtr<SWidget> Viewport = GameTestSlate::GetGameViewportWidget();
	if (!Window.IsValid() || !Viewport.IsValid())
	{
		OutError = TEXT("没有游戏视口：先启动 PIE 或游戏");
		return false;
	}

	// 截整个窗口再裁视口：UMG 画在 Slate 上，视口自己的渲染目标里没有界面
	TArray<FColor> Pixels;
	FIntVector Size = FIntVector::ZeroValue;
	if (!FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), Pixels, Size) || Size.X <= 0 || Size.Y <= 0 || Pixels.Num() != Size.X * Size.Y)
	{
		OutError = TEXT("截不到游戏画面（窗口最小化了，或 -nullrhi）");
		return false;
	}
	const FIntRect Rect = GameTestLiveFrameDetail::ViewportRect(*Window, *Viewport, Size);
	if (Rect.Width() < 16 || Rect.Height() < 16)
	{
		OutError = TEXT("游戏视口太小或被挡住了，截不到画面");
		return false;
	}

	const int32 Width = Rect.Width();
	const int32 Height = Rect.Height();
	TArray<FColor> Region;
	Region.SetNumUninitialized(Width * Height);
	for (int32 Row = 0; Row < Height; ++Row)
	{
		FMemory::Memcpy(&Region[Row * Width], &Pixels[(Rect.Min.Y + Row) * Size.X + Rect.Min.X], Width * sizeof(FColor));
	}

	int32 OutWidth = Width;
	int32 OutHeight = Height;
	if (InMaxWidth > 0 && Width > InMaxWidth)
	{
		OutWidth = InMaxWidth;
		OutHeight = FMath::Max(1, Height * InMaxWidth / Width);
		TArray<FColor> Small;
		FImageUtils::ImageResize(Width, Height, Region, OutWidth, OutHeight, Small, false);
		Region = MoveTemp(Small);
	}
	for (FColor& Pixel : Region)
	{
		Pixel.A = 255;
	}

	// 画面没变（游戏停着、界面静止）就不压也不推：省编码，也省推送通道
	OutHash = FCrc::MemCrc32(Region.GetData(), static_cast<int32>(Region.Num() * sizeof(FColor)));
	if (InLastHash != 0 && OutHash == InLastHash)
	{
		return true;
	}

	IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::JPEG);
	if (!Wrapper.IsValid() || !Wrapper->SetRaw(Region.GetData(), static_cast<int64>(Region.Num()) * sizeof(FColor), OutWidth, OutHeight, ERGBFormat::BGRA, 8))
	{
		OutError = TEXT("压 JPEG 失败（ImageWrapper 模块不可用）");
		return false;
	}
	const TArray64<uint8> Jpeg = Wrapper->GetCompressed(FMath::Clamp(InQuality, 30, 95));
	if (Jpeg.Num() == 0)
	{
		OutError = TEXT("压 JPEG 失败（编码结果为空）");
		return false;
	}

	OutFrame = MakeShared<FJsonObject>();
	OutFrame->SetStringField(TEXT("jpg"), FBase64::Encode(Jpeg.GetData(), static_cast<uint32>(Jpeg.Num())));
	OutFrame->SetNumberField(TEXT("width"), OutWidth);
	OutFrame->SetNumberField(TEXT("height"), OutHeight);
	return true;
}
