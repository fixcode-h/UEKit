// Copyright Epic Games, Inc. All Rights Reserved.

#include "Record/GameTestRecordShot.h"

#include "Core/GameTestWorld.h"
#include "Observe/GameTestSelectorWriter.h"
#include "Observe/GameTestSlate.h"
#include "Observe/GameTestSnapshot.h"
#include "Shot/GameTestShot.h"

#include "Async/Async.h"
#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectHash.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace GameTestRecordShotDetail
{
	/** 被点控件的框：亮红，外面包一圈深色，深浅背景上都看得清。 */
	const FColor Outline(255, 59, 48, 255);
	const FColor OutlineShadow(20, 20, 20, 255);

	/** 从整张截图里拷出一块（调用方保证框在图内）。 */
	TArray<FColor> CopyRegion(const TArray<FColor>& InPixels, const FIntPoint& InSize, const FIntRect& InRect)
	{
		TArray<FColor> Out;
		const int32 Width = InRect.Width();
		const int32 Height = InRect.Height();
		Out.SetNumUninitialized(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			FMemory::Memcpy(&Out[Y * Width], &InPixels[(InRect.Min.Y + Y) * InSize.X + InRect.Min.X], Width * sizeof(FColor));
		}
		return Out;
	}

	/** 按面积平均缩小（只缩不放）：文字缩小后比取最近点清楚，后面还要拿去 OCR。 */
	TArray<FColor> Shrink(const TArray<FColor>& InPixels, const FIntPoint& InSize, const FIntPoint& InTo)
	{
		if (InTo == InSize)
		{
			return InPixels;
		}
		TArray<FColor> Out;
		Out.SetNumUninitialized(InTo.X * InTo.Y);
		const double ScaleX = static_cast<double>(InSize.X) / InTo.X;
		const double ScaleY = static_cast<double>(InSize.Y) / InTo.Y;
		for (int32 Y = 0; Y < InTo.Y; ++Y)
		{
			const int32 Y0 = FMath::FloorToInt(Y * ScaleY);
			const int32 Y1 = FMath::Clamp(FMath::CeilToInt((Y + 1) * ScaleY), Y0 + 1, InSize.Y);
			for (int32 X = 0; X < InTo.X; ++X)
			{
				const int32 X0 = FMath::FloorToInt(X * ScaleX);
				const int32 X1 = FMath::Clamp(FMath::CeilToInt((X + 1) * ScaleX), X0 + 1, InSize.X);
				uint32 R = 0, G = 0, B = 0, N = 0;
				for (int32 SY = Y0; SY < Y1; ++SY)
				{
					for (int32 SX = X0; SX < X1; ++SX)
					{
						const FColor& C = InPixels[SY * InSize.X + SX];
						R += C.R;
						G += C.G;
						B += C.B;
						++N;
					}
				}
				Out[Y * InTo.X + X] = N > 0 ? FColor(R / N, G / N, B / N, 255) : FColor::Black;
			}
		}
		return Out;
	}

	void Put(TArray<FColor>& InPixels, const FIntPoint& InSize, int32 InX, int32 InY, const FColor& InColor)
	{
		if (InX >= 0 && InY >= 0 && InX < InSize.X && InY < InSize.Y)
		{
			InPixels[InY * InSize.X + InX] = InColor;
		}
	}

	/** 空心框：从 InRect 往外画 InThickness 圈。 */
	void DrawFrame(TArray<FColor>& InPixels, const FIntPoint& InSize, const FIntRect& InRect, int32 InInset, int32 InThickness, const FColor& InColor)
	{
		for (int32 Ring = InInset; Ring < InInset + InThickness; ++Ring)
		{
			const int32 L = InRect.Min.X - Ring;
			const int32 T = InRect.Min.Y - Ring;
			const int32 R = InRect.Max.X - 1 + Ring;
			const int32 B = InRect.Max.Y - 1 + Ring;
			for (int32 X = L; X <= R; ++X)
			{
				Put(InPixels, InSize, X, T, InColor);
				Put(InPixels, InSize, X, B, InColor);
			}
			for (int32 Y = T; Y <= B; ++Y)
			{
				Put(InPixels, InSize, L, Y, InColor);
				Put(InPixels, InSize, R, Y, InColor);
			}
		}
	}

	/** 点击位置的十字：深色 3px 打底，中间 1px 红线。 */
	void DrawCross(TArray<FColor>& InPixels, const FIntPoint& InSize, const FIntPoint& InAt)
	{
		constexpr int32 Arm = 7;
		for (int32 D = -Arm; D <= Arm; ++D)
		{
			for (int32 W = -1; W <= 1; ++W)
			{
				Put(InPixels, InSize, InAt.X + D, InAt.Y + W, OutlineShadow);
				Put(InPixels, InSize, InAt.X + W, InAt.Y + D, OutlineShadow);
			}
		}
		for (int32 D = -Arm; D <= Arm; ++D)
		{
			Put(InPixels, InSize, InAt.X + D, InAt.Y, Outline);
			Put(InPixels, InSize, InAt.X, InAt.Y + D, Outline);
		}
	}

	/** 后台线程：缩小、描框、画十字、压 PNG 写盘。框和十字的坐标是相对裁剪区的原始像素。 */
	void SaveAsync(TArray<FColor>&& InPixels, const FIntPoint& InSize, const FIntPoint& InTo, const TOptional<FIntRect>& InFrame,
		const TOptional<FIntPoint>& InCross, const FString& InPath)
	{
		Async(EAsyncExecution::ThreadPool, [Pixels = MoveTemp(InPixels), InSize, InTo, InFrame, InCross, InPath]()
		{
			TArray<FColor> Out = Shrink(Pixels, InSize, InTo);
			const double Sx = static_cast<double>(InTo.X) / InSize.X;
			const double Sy = static_cast<double>(InTo.Y) / InSize.Y;
			if (InFrame.IsSet())
			{
				const FIntRect Frame(
					FMath::RoundToInt(InFrame->Min.X * Sx), FMath::RoundToInt(InFrame->Min.Y * Sy),
					FMath::RoundToInt(InFrame->Max.X * Sx), FMath::RoundToInt(InFrame->Max.Y * Sy));
				DrawFrame(Out, InTo, Frame, 0, 2, Outline);
				DrawFrame(Out, InTo, Frame, 2, 1, OutlineShadow);
			}
			if (InCross.IsSet())
			{
				DrawCross(Out, InTo, FIntPoint(FMath::RoundToInt(InCross->X * Sx), FMath::RoundToInt(InCross->Y * Sy)));
			}
			// TakeScreenshot 的 alpha 不一定是 255：存 PNG 前补成不透明，不然看图软件里是半透明的
			for (FColor& C : Out)
			{
				C.A = 255;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(InPath), /*Tree=*/true);
			const FImageView Image(Out.GetData(), InTo.X, InTo.Y);
			FImageUtils::SaveImageByExtension(*InPath, Image);
		});
	}

	FIntRect ToPixels(const FVector2D& InMin, const FVector2D& InSize, const FVector2D& InWindowPos, double InScale)
	{
		const FVector2D Local = InMin - InWindowPos;
		return FIntRect(
			FMath::RoundToInt(Local.X * InScale), FMath::RoundToInt(Local.Y * InScale),
			FMath::RoundToInt((Local.X + InSize.X) * InScale), FMath::RoundToInt((Local.Y + InSize.Y) * InScale));
	}

	bool ReadInt(const FJsonObject& InObject, const TCHAR* InField, int32 InMin, int32 InMax, int32& OutValue, FString& OutError)
	{
		double Value = 0.0;
		if (!InObject.HasField(InField))
		{
			return true;
		}
		if (!InObject.TryGetNumberField(InField, Value) || Value < InMin || Value > InMax)
		{
			OutError = FString::Printf(TEXT("shots.%s 要写 %d–%d 的整数"), InField, InMin, InMax);
			return false;
		}
		OutValue = FMath::RoundToInt(Value);
		return true;
	}
}

bool FGameTestShotOptions::FromJson(const TSharedPtr<FJsonValue>& InValue, FGameTestShotOptions& OutOptions, FString& OutError)
{
	OutOptions = FGameTestShotOptions();
	if (!InValue.IsValid() || InValue->IsNull())
	{
		return true;
	}
	// 先看类型再取值：FJsonValue 的 TryGetBool 对数字、字符串也会返回 true
	if (InValue->Type == EJson::Boolean)
	{
		OutOptions.bEnabled = InValue->AsBool();
		return true;
	}
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!InValue->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
	{
		OutError = TEXT("shots 要写 true / false，或 {padRatio, padMin, padMax, minWidth, minHeight, maxWidth, maxHeight, screenWidth}");
		return false;
	}
	const FJsonObject& Json = **Object;
	static const TCHAR* Known[] = {
		TEXT("enabled"), TEXT("padRatio"), TEXT("padMin"), TEXT("padMax"), TEXT("minWidth"), TEXT("minHeight"),
		TEXT("maxWidth"), TEXT("maxHeight"), TEXT("screenWidth"), TEXT("gameClickBox"),
	};
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Json.Values)
	{
		bool bKnown = false;
		for (const TCHAR* Name : Known)
		{
			bKnown |= Pair.Key == Name;
		}
		if (!bKnown)
		{
			OutError = FString::Printf(TEXT("shots 里不认识 %s（能写 enabled、padRatio、padMin、padMax、minWidth、minHeight、maxWidth、maxHeight、screenWidth、gameClickBox）"), *Pair.Key);
			return false;
		}
	}
	Json.TryGetBoolField(TEXT("enabled"), OutOptions.bEnabled);
	double Ratio = OutOptions.PadRatio;
	if (Json.HasField(TEXT("padRatio")) && (!Json.TryGetNumberField(TEXT("padRatio"), Ratio) || Ratio < 0.0 || Ratio > 5.0))
	{
		OutError = TEXT("shots.padRatio 要写 0–5 的数（控件宽高的倍数，0.5 = 每边外扩一半）");
		return false;
	}
	OutOptions.PadRatio = Ratio;
	using GameTestRecordShotDetail::ReadInt;
	return ReadInt(Json, TEXT("padMin"), 0, 1000, OutOptions.PadMin, OutError)
		&& ReadInt(Json, TEXT("padMax"), 0, 2000, OutOptions.PadMax, OutError)
		&& ReadInt(Json, TEXT("minWidth"), 16, 4096, OutOptions.MinWidth, OutError)
		&& ReadInt(Json, TEXT("minHeight"), 16, 4096, OutOptions.MinHeight, OutError)
		&& ReadInt(Json, TEXT("maxWidth"), 64, 8192, OutOptions.MaxWidth, OutError)
		&& ReadInt(Json, TEXT("maxHeight"), 64, 8192, OutOptions.MaxHeight, OutError)
		&& ReadInt(Json, TEXT("screenWidth"), 0, 4096, OutOptions.ScreenWidth, OutError)
		&& ReadInt(Json, TEXT("gameClickBox"), 16, 2048, OutOptions.GameClickBox, OutError);
}

FIntRect FGameTestRecordShots::ComputeCrop(const FIntRect& InTarget, const FIntPoint& InImage, const FGameTestShotOptions& InOptions)
{
	const int32 Width = FMath::Max(1, InTarget.Width());
	const int32 Height = FMath::Max(1, InTarget.Height());
	const int32 PadMax = FMath::Max(InOptions.PadMin, InOptions.PadMax);
	const int32 PadX = FMath::Clamp(FMath::RoundToInt(Width * InOptions.PadRatio), InOptions.PadMin, PadMax);
	const int32 PadY = FMath::Clamp(FMath::RoundToInt(Height * InOptions.PadRatio), InOptions.PadMin, PadMax);
	FIntRect Crop(InTarget.Min.X - PadX, InTarget.Min.Y - PadY, InTarget.Max.X + PadX, InTarget.Max.Y + PadY);

	// 太小的补到最小尺寸（以控件中心为准）：小图标也要带点上下文
	const FIntPoint Center = InTarget.Min + FIntPoint(Width / 2, Height / 2);
	if (Crop.Width() < InOptions.MinWidth)
	{
		Crop.Min.X = Center.X - InOptions.MinWidth / 2;
		Crop.Max.X = Crop.Min.X + InOptions.MinWidth;
	}
	if (Crop.Height() < InOptions.MinHeight)
	{
		Crop.Min.Y = Center.Y - InOptions.MinHeight / 2;
		Crop.Max.Y = Crop.Min.Y + InOptions.MinHeight;
	}

	// 碰到图边就往里挪，挪不下（比整张图还大）才截掉
	auto Fit = [](int32& InOutMin, int32& InOutMax, int32 InLimit)
	{
		const int32 Size = FMath::Min(InOutMax - InOutMin, InLimit);
		InOutMin = FMath::Clamp(InOutMin, 0, InLimit - Size);
		InOutMax = InOutMin + Size;
	};
	Fit(Crop.Min.X, Crop.Max.X, InImage.X);
	Fit(Crop.Min.Y, Crop.Max.Y, InImage.Y);
	return Crop;
}

FIntPoint FGameTestRecordShots::FitInside(const FIntPoint& InSize, const FIntPoint& InMax)
{
	if (InSize.X <= InMax.X && InSize.Y <= InMax.Y)
	{
		return InSize;
	}
	const double Scale = FMath::Min(static_cast<double>(InMax.X) / InSize.X, static_cast<double>(InMax.Y) / InSize.Y);
	return FIntPoint(FMath::Max(1, FMath::FloorToInt(InSize.X * Scale)), FMath::Max(1, FMath::FloorToInt(InSize.Y * Scale)));
}

void FGameTestRecordShots::Begin(const FGameTestShotOptions& InOptions)
{
	Options = InOptions;
	bActive = true;
	Counter = 0;
	LastScreens.Reset();
	LastScreenPath.Reset();
	PendingOpened.Reset();
	Dir = GameTestShot::GetOutputRootDir() / TEXT("shots") / FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
	// 压 PNG 在后台线程：图片编码模块先在游戏线程加载好，后台线程里不去加载模块
	FModuleManager::Get().LoadModule(TEXT("ImageWrapper"));
}

void FGameTestRecordShots::End()
{
	bActive = false;
	PendingOpened.Reset();
}

FString FGameTestRecordShots::NextPath(const TCHAR* InPrefix)
{
	return Dir / FString::Printf(TEXT("%s%03d.png"), InPrefix, ++Counter);
}

bool FGameTestRecordShots::Capture(TArray<FColor>& OutPixels, FIntPoint& OutSize, FVector2D& OutWindowPos, double& OutScale) const
{
	if (!FSlateApplication::IsInitialized())
	{
		return false;
	}
	const TSharedPtr<SWindow> Window = GameTestSlate::GetGameWindow();
	if (!Window.IsValid())
	{
		return false;
	}
	FIntVector Size = FIntVector::ZeroValue;
	if (!FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), OutPixels, Size) || Size.X <= 0 || Size.Y <= 0
		|| OutPixels.Num() != Size.X * Size.Y)
	{
		return false;
	}
	OutSize = FIntPoint(Size.X, Size.Y);
	OutWindowPos = FVector2D(Window->GetPositionInScreen());
	const FVector2D WindowSize = FVector2D(Window->GetSizeInScreen());
	OutScale = WindowSize.X > 1.0 ? static_cast<double>(Size.X) / WindowSize.X : 1.0;
	return true;
}

TArray<FString> FGameTestRecordShots::TopScreens()
{
	TArray<FString> Names;
	UWorld* World = GameTestWorld::FindGameWorld();
	if (World == nullptr)
	{
		return Names;
	}
	TArray<UObject*> Objects;
	GetObjectsOfClass(UUserWidget::StaticClass(), Objects, /*bIncludeDerivedClasses=*/true);
	for (UObject* Object : Objects)
	{
		const UUserWidget* Widget = Cast<UUserWidget>(Object);
		if (!IsValid(Widget) || Widget->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || Widget->GetWorld() != World
			|| !Widget->GetCachedWidget().IsValid() || !Widget->IsVisible() || !GameTestSelectorWriter::IsTopLevel(*Widget))
		{
			continue;
		}
		Names.AddUnique(GameTestSnapshot::FriendlyClassName(Widget->GetClass()));
	}
	Names.Sort();
	return Names;
}

void FGameTestRecordShots::MaybeSaveScreen(const TArray<FColor>& InPixels, const FIntPoint& InSize, const FVector2D& InWindowPos, double InScale)
{
	if (Options.ScreenWidth <= 0)
	{
		return;
	}
	const TArray<FString> Screens = TopScreens();
	if (!LastScreenPath.IsEmpty() && Screens == LastScreens)
	{
		return;
	}
	// 只要游戏视口那一块：PIE 在关卡编辑器视口里时「游戏窗口」是整个编辑器
	FIntRect Rect(FIntPoint::ZeroValue, InSize);
	if (const TSharedPtr<SWidget> Viewport = GameTestSlate::GetGameViewportWidget())
	{
		const FGeometry& Geometry = Viewport->GetCachedGeometry();
		Rect = GameTestRecordShotDetail::ToPixels(
			FVector2D(Geometry.GetAbsolutePosition()), FVector2D(Geometry.GetAbsoluteSize()), InWindowPos, InScale);
		Rect.Clip(FIntRect(FIntPoint::ZeroValue, InSize));
	}
	if (Rect.Width() < 16 || Rect.Height() < 16)
	{
		return;
	}
	const FString Path = NextPath(TEXT("screen-"));
	const FIntPoint To = FitInside(Rect.Size(), FIntPoint(Options.ScreenWidth, Options.ScreenWidth * 4));
	GameTestRecordShotDetail::SaveAsync(GameTestRecordShotDetail::CopyRegion(InPixels, InSize, Rect), Rect.Size(), To, {}, {}, Path);
	LastScreens = Screens;
	LastScreenPath = GameTestShot::ToProjectRelative(Path);
}

void FGameTestRecordShots::OnUiPress(const TSharedPtr<SWidget>& InSlate, const FVector2D& InScreenPosition, const TSharedPtr<FJsonObject>& InHint)
{
	if (!IsEnabled() || !InHint.IsValid())
	{
		return;
	}
	// 点完打开了什么：记下按下前在屏上的界面，Tick 里对比
	FPendingOpened Pending;
	Pending.Hint = InHint;
	Pending.Before = TopScreens();
	Pending.PressedAt = FPlatformTime::Seconds();
	PendingOpened = MoveTemp(Pending);

	TArray<FColor> Pixels;
	FIntPoint Size;
	FVector2D WindowPos;
	double Scale = 1.0;
	if (!Capture(Pixels, Size, WindowPos, Scale))
	{
		return;
	}
	MaybeSaveScreen(Pixels, Size, WindowPos, Scale);
	if (!LastScreenPath.IsEmpty())
	{
		InHint->SetStringField(TEXT("screen"), LastScreenPath);
	}

	FIntRect Target;
	if (InSlate.IsValid())
	{
		const FGeometry& Geometry = InSlate->GetCachedGeometry();
		Target = GameTestRecordShotDetail::ToPixels(
			FVector2D(Geometry.GetAbsolutePosition()), FVector2D(Geometry.GetAbsoluteSize()), WindowPos, Scale);
	}
	if (Target.Width() < 1 || Target.Height() < 1)
	{
		// 几何没算出来：退回以点击位置为中心的方块
		const FIntRect Around = GameTestRecordShotDetail::ToPixels(InScreenPosition, FVector2D::ZeroVector, WindowPos, Scale);
		Target = FIntRect(Around.Min - FIntPoint(8, 8), Around.Min + FIntPoint(8, 8));
	}
	const FIntRect Crop = ComputeCrop(Target, Size, Options);
	if (Crop.Width() < 1 || Crop.Height() < 1)
	{
		return;
	}
	const FIntRect Click = GameTestRecordShotDetail::ToPixels(InScreenPosition, FVector2D::ZeroVector, WindowPos, Scale);
	const FString Path = NextPath(TEXT(""));
	const FIntPoint To = FitInside(Crop.Size(), FIntPoint(Options.MaxWidth, Options.MaxHeight));
	const FIntRect Frame(Target.Min - Crop.Min, Target.Max - Crop.Min);
	GameTestRecordShotDetail::SaveAsync(
		GameTestRecordShotDetail::CopyRegion(Pixels, Size, Crop), Crop.Size(), To, Frame, FIntPoint(Click.Min - Crop.Min), Path);
	InHint->SetStringField(TEXT("shot"), GameTestShot::ToProjectRelative(Path));
	// 红框在存下来的图里的位置 [x, y, w, h]：面板 OCR 只认框里的字
	const double Sx = static_cast<double>(To.X) / Crop.Width();
	const double Sy = static_cast<double>(To.Y) / Crop.Height();
	TArray<TSharedPtr<FJsonValue>> Box;
	Box.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Frame.Min.X * Sx)));
	Box.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Frame.Min.Y * Sy)));
	Box.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Frame.Width() * Sx)));
	Box.Add(MakeShared<FJsonValueNumber>(FMath::RoundToInt(Frame.Height() * Sy)));
	InHint->SetArrayField(TEXT("shotBox"), Box);
}

void FGameTestRecordShots::OnGamePress(const FVector2D& InScreenPosition, const TSharedPtr<FJsonObject>& InHint)
{
	if (!IsEnabled() || !InHint.IsValid())
	{
		return;
	}
	TArray<FColor> Pixels;
	FIntPoint Size;
	FVector2D WindowPos;
	double Scale = 1.0;
	if (!Capture(Pixels, Size, WindowPos, Scale))
	{
		return;
	}
	MaybeSaveScreen(Pixels, Size, WindowPos, Scale);
	if (!LastScreenPath.IsEmpty())
	{
		InHint->SetStringField(TEXT("screen"), LastScreenPath);
	}
	const FIntPoint At = GameTestRecordShotDetail::ToPixels(InScreenPosition, FVector2D::ZeroVector, WindowPos, Scale).Min;
	const int32 Half = Options.GameClickBox / 2;
	FGameTestShotOptions Plain = Options;
	Plain.PadRatio = 0.0;
	Plain.PadMin = 0;
	Plain.PadMax = 0;
	const FIntRect Crop = ComputeCrop(FIntRect(At - FIntPoint(Half, Half), At + FIntPoint(Half, Half)), Size, Plain);
	if (Crop.Width() < 1 || Crop.Height() < 1)
	{
		return;
	}
	const FString Path = NextPath(TEXT(""));
	GameTestRecordShotDetail::SaveAsync(
		GameTestRecordShotDetail::CopyRegion(Pixels, Size, Crop), Crop.Size(),
		FitInside(Crop.Size(), FIntPoint(Options.MaxWidth, Options.MaxHeight)), {}, FIntPoint(At - Crop.Min), Path);
	InHint->SetStringField(TEXT("shot"), GameTestShot::ToProjectRelative(Path));
}

void FGameTestRecordShots::Tick(double InNow)
{
	if (!PendingOpened.IsSet())
	{
		return;
	}
	FPendingOpened& Pending = PendingOpened.GetValue();
	const double Since = InNow - Pending.PressedAt;
	if (Since > 2.0 || !Pending.Hint.IsValid())
	{
		PendingOpened.Reset();
		return;
	}
	// 按下后 0.2 秒起、每 0.25 秒看一次（取界面清单要遍历所有 UUserWidget，不每帧做）
	if (Since < 0.2 || InNow - Pending.CheckedAt < 0.25)
	{
		return;
	}
	Pending.CheckedAt = InNow;
	TArray<TSharedPtr<FJsonValue>> Opened;
	for (const FString& Screen : TopScreens())
	{
		if (!Pending.Before.Contains(Screen))
		{
			Opened.Add(MakeShared<FJsonValueString>(Screen));
		}
	}
	if (Opened.Num() > 0)
	{
		Pending.Hint->SetArrayField(TEXT("opened"), Opened);
		PendingOpened.Reset();
	}
}
