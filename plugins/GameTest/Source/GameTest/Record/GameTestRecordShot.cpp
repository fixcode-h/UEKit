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

	/** 一圈空心框：InOffset > 0 往外偏，< 0 往里收。 */
	void DrawRing(TArray<FColor>& InPixels, const FIntPoint& InSize, const FIntRect& InRect, int32 InOffset, const FColor& InColor)
	{
		const int32 L = InRect.Min.X - InOffset;
		const int32 T = InRect.Min.Y - InOffset;
		const int32 R = InRect.Max.X - 1 + InOffset;
		const int32 B = InRect.Max.Y - 1 + InOffset;
		if (L > R || T > B)
		{
			return;
		}
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

	/**
	 * 控件描边：2 圈红线 + 1 圈深色。框外留得下 3 像素就画在框外（不压控件内容）；
	 * 没外扩时框就是整张图，画在框外看不见，改成往里收（红线在最外、深色在里），只压控件边上 3 像素。
	 */
	void DrawOutline(TArray<FColor>& InPixels, const FIntPoint& InSize, const FIntRect& InFrame)
	{
		FIntRect Frame = InFrame;
		Frame.Clip(FIntRect(FIntPoint::ZeroValue, InSize));
		if (Frame.Width() < 1 || Frame.Height() < 1)
		{
			return;
		}
		const int32 Room = FMath::Min(FMath::Min(Frame.Min.X, Frame.Min.Y), FMath::Min(InSize.X - Frame.Max.X, InSize.Y - Frame.Max.Y));
		if (Room >= 3)
		{
			DrawRing(InPixels, InSize, Frame, 0, Outline);
			DrawRing(InPixels, InSize, Frame, 1, Outline);
			DrawRing(InPixels, InSize, Frame, 2, OutlineShadow);
			return;
		}
		DrawRing(InPixels, InSize, Frame, 0, Outline);
		DrawRing(InPixels, InSize, Frame, -1, Outline);
		DrawRing(InPixels, InSize, Frame, -2, OutlineShadow);
	}

	/** 后台线程：缩小、描框、压 PNG 写盘。框的坐标是相对裁剪区的原始像素。 */
	void SaveAsync(TArray<FColor>&& InPixels, const FIntPoint& InSize, const FIntPoint& InTo, const TOptional<FIntRect>& InFrame, const FString& InPath)
	{
		Async(EAsyncExecution::ThreadPool, [Pixels = MoveTemp(InPixels), InSize, InTo, InFrame, InPath]()
		{
			TArray<FColor> Out = Shrink(Pixels, InSize, InTo);
			const double Sx = static_cast<double>(InTo.X) / InSize.X;
			const double Sy = static_cast<double>(InTo.Y) / InSize.Y;
			if (InFrame.IsSet())
			{
				const FIntRect Frame(
					FMath::RoundToInt(InFrame->Min.X * Sx), FMath::RoundToInt(InFrame->Min.Y * Sy),
					FMath::RoundToInt(InFrame->Max.X * Sx), FMath::RoundToInt(InFrame->Max.Y * Sy));
				DrawOutline(Out, InTo, Frame);
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

	bool ReadRatio(const FJsonObject& InObject, const TCHAR* InField, double& OutValue, FString& OutError)
	{
		if (!InObject.HasField(InField))
		{
			return true;
		}
		double Value = 0.0;
		if (!InObject.TryGetNumberField(InField, Value) || Value < 0.0 || Value > 5.0)
		{
			OutError = FString::Printf(TEXT("shots.%s 要写 0–5 的数（边长的倍数，0.2 = 每边外扩边长的 20%%）"), InField);
			return false;
		}
		OutValue = Value;
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
		OutError = TEXT("shots 要写 true / false，或 {expand, padX, padXMax, padY, padYMax, maxWidth, maxHeight, screenWidth, gameClickBox}");
		return false;
	}
	const FJsonObject& Json = **Object;
	// padRatio、padMin、padMax、minWidth、minHeight 是 1.4.x 及以前一个比例管四边的写法，旧面板还会传：照旧收下，
	// 换算成左右、上下同一个比例并打开外扩，旧面板录出来的图和以前一样
	static const TCHAR* Known[] = {
		TEXT("enabled"), TEXT("expand"), TEXT("padX"), TEXT("padXMax"), TEXT("padY"), TEXT("padYMax"),
		TEXT("maxWidth"), TEXT("maxHeight"), TEXT("screenWidth"), TEXT("gameClickBox"),
		TEXT("padRatio"), TEXT("padMin"), TEXT("padMax"), TEXT("minWidth"), TEXT("minHeight"),
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
			OutError = FString::Printf(TEXT("shots 里不认识 %s（能写 enabled、expand、padX、padXMax、padY、padYMax、maxWidth、maxHeight、screenWidth、gameClickBox）"), *Pair.Key);
			return false;
		}
	}
	Json.TryGetBoolField(TEXT("enabled"), OutOptions.bEnabled);
	using GameTestRecordShotDetail::ReadInt;
	using GameTestRecordShotDetail::ReadRatio;
	if (Json.HasField(TEXT("padRatio")))
	{
		double Legacy = 0.0;
		int32 LegacyMax = 160;
		if (!ReadRatio(Json, TEXT("padRatio"), Legacy, OutError) || !ReadInt(Json, TEXT("padMax"), 0, 2000, LegacyMax, OutError))
		{
			return false;
		}
		OutOptions.bExpand = true;
		OutOptions.PadXRatio = OutOptions.PadYRatio = Legacy;
		OutOptions.PadXMax = OutOptions.PadYMax = LegacyMax;
		OutOptions.PadMin = 24;
		OutOptions.MinWidth = 200;
		OutOptions.MinHeight = 120;
	}
	Json.TryGetBoolField(TEXT("expand"), OutOptions.bExpand);
	return ReadRatio(Json, TEXT("padX"), OutOptions.PadXRatio, OutError)
		&& ReadInt(Json, TEXT("padXMax"), 0, 2000, OutOptions.PadXMax, OutError)
		&& ReadRatio(Json, TEXT("padY"), OutOptions.PadYRatio, OutError)
		&& ReadInt(Json, TEXT("padYMax"), 0, 2000, OutOptions.PadYMax, OutError)
		&& ReadInt(Json, TEXT("padMin"), 0, 1000, OutOptions.PadMin, OutError)
		&& ReadInt(Json, TEXT("minWidth"), 0, 4096, OutOptions.MinWidth, OutError)
		&& ReadInt(Json, TEXT("minHeight"), 0, 4096, OutOptions.MinHeight, OutError)
		&& ReadInt(Json, TEXT("maxWidth"), 64, 8192, OutOptions.MaxWidth, OutError)
		&& ReadInt(Json, TEXT("maxHeight"), 64, 8192, OutOptions.MaxHeight, OutError)
		&& ReadInt(Json, TEXT("screenWidth"), 0, 4096, OutOptions.ScreenWidth, OutError)
		&& ReadInt(Json, TEXT("gameClickBox"), 16, 2048, OutOptions.GameClickBox, OutError);
}

FIntRect FGameTestRecordShots::ComputeCrop(const FIntRect& InTarget, const FIntPoint& InImage, const FGameTestShotOptions& InOptions)
{
	const int32 Width = FMath::Max(1, InTarget.Width());
	const int32 Height = FMath::Max(1, InTarget.Height());
	FIntRect Crop(InTarget.Min, InTarget.Min + FIntPoint(Width, Height));
	if (!InOptions.bExpand)
	{
		// 不外扩：就是控件那一块，只夹进图内（控件一部分在屏幕外时只截看得见的）
		Crop.Clip(FIntRect(FIntPoint::ZeroValue, InImage));
		return Crop;
	}
	// 左右按宽、上下按高各算各的，各自封顶：细长条上下只扩一点，宽条目左右也不会扩出去一大片
	const auto Pad = [&InOptions](int32 InLength, double InRatio, int32 InMax)
	{
		const int32 Max = FMath::Max(InOptions.PadMin, InMax);
		return FMath::Clamp(FMath::RoundToInt(InLength * InRatio), InOptions.PadMin, Max);
	};
	const int32 PadX = Pad(Width, InOptions.PadXRatio, InOptions.PadXMax);
	const int32 PadY = Pad(Height, InOptions.PadYRatio, InOptions.PadYMax);
	Crop = FIntRect(Crop.Min.X - PadX, Crop.Min.Y - PadY, Crop.Max.X + PadX, Crop.Max.Y + PadY);

	// 设了最小尺寸时补齐（以控件中心为准，旧面板的写法）
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
	GameTestRecordShotDetail::SaveAsync(GameTestRecordShotDetail::CopyRegion(InPixels, InSize, Rect), Rect.Size(), To, {}, Path);
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
	const FString Path = NextPath(TEXT(""));
	const FIntPoint To = FitInside(Crop.Size(), FIntPoint(Options.MaxWidth, Options.MaxHeight));
	const FIntRect Frame(Target.Min - Crop.Min, Target.Max - Crop.Min);
	GameTestRecordShotDetail::SaveAsync(
		GameTestRecordShotDetail::CopyRegion(Pixels, Size, Crop), Crop.Size(), To, Frame, Path);
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
	Plain.bExpand = false;
	const FIntRect Crop = ComputeCrop(FIntRect(At - FIntPoint(Half, Half), At + FIntPoint(Half, Half)), Size, Plain);
	if (Crop.Width() < 1 || Crop.Height() < 1)
	{
		return;
	}
	const FString Path = NextPath(TEXT(""));
	GameTestRecordShotDetail::SaveAsync(
		GameTestRecordShotDetail::CopyRegion(Pixels, Size, Crop), Crop.Size(),
		FitInside(Crop.Size(), FIntPoint(Options.MaxWidth, Options.MaxHeight)), {}, Path);
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
