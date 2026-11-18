// Copyright Epic Games, Inc. All Rights Reserved.

#include "Record/GameTestRecorder.h"

#include "Observe/GameTestItemKey.h"
#include "Shot/GameTestShot.h"
#include "GameTest.h"

#include "Async/Async.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace GameTestRecordKeyCacheDetail
{
	/** 一个条目的字段数有上限（每个来源 MaxFieldsPerSource），这里只防意外：缓存文件不超过几十 KB 量级 */
	constexpr int32 MaxCachedFields = 1000;
	constexpr int32 MaxCachedChars = 400000;
}

void FGameTestRecorder::CapturePressedFields(FGameTestKeySite& InOutSite)
{
	using namespace GameTestRecordKeyCacheDetail;
	InOutSite.PressedFields.Reset();
	UWidget* Container = InOutSite.Container.Get();
	if (Container == nullptr)
	{
		return;
	}
	TArray<FGameTestKeyEntry> Entries;
	if (!GameTestItemKey::CollectEntries(Container, Entries))
	{
		return;
	}
	const int32 Mine = GameTestItemKey::FindEntry(Entries, InOutSite.Entry.Widget.Get(), InOutSite.Entry.Item.Get());
	if (Mine == INDEX_NONE)
	{
		return;
	}
	const TSharedPtr<FJsonObject> Fields = GameTestItemKey::ListFields(Container, Entries, Mine, FString(), 0, MaxCachedFields, MaxCachedChars);
	if (!Fields.IsValid())
	{
		return;
	}
	// 和 gt.keys 的返回同形状，面板拿来直接用；值是按下那一刻的
	Fields->SetNumberField(TEXT("index"), Mine);
	Fields->SetNumberField(TEXT("entries"), Entries.Num());
	Fields->SetStringField(TEXT("recordedAt"), FDateTime::Now().ToIso8601());
	InOutSite.PressedFields = Fields;
}

void FGameTestRecorder::CacheKeyFields(const TSharedRef<FJsonObject>& InStep, const FGameTestKeySite& InSite)
{
	// 存不下只是以后换标识要开游戏，录制照常：每一步都悄悄跳过，不报错
	// 用按下时读的：松开后列表可能已经刷新（最近用过的排到前面），再读就是别的条目
	const TSharedPtr<FJsonObject> Fields = InSite.PressedFields;
	if (!Fields.IsValid() || KeyCacheDir.IsEmpty())
	{
		return;
	}

	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	if (!FJsonSerializer::Serialize(Fields.ToSharedRef(), Writer))
	{
		return;
	}

	const FString Path = KeyCacheDir / FString::Printf(TEXT("%03d.json"), ++KeyCacheCounter);
	const FString Dir = KeyCacheDir;
	// 写盘放后台：点击时不卡游戏线程
	Async(EAsyncExecution::ThreadPool, [Text = MoveTemp(Text), Path, Dir]()
	{
		IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
		if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogGameTest, Warning, TEXT("录制：条目字段缓存没写上（%s）"), *Path);
		}
	});

	// 写在 hint 下、不进 hint.key：改认法时 key 整个重写，这个路径要留着
	const TSharedPtr<FJsonObject>* Hint = nullptr;
	TSharedPtr<FJsonObject> HintObject;
	if (InStep->TryGetObjectField(TEXT("hint"), Hint) && Hint != nullptr && Hint->IsValid())
	{
		HintObject = *Hint;
	}
	else
	{
		HintObject = MakeShared<FJsonObject>();
		InStep->SetObjectField(TEXT("hint"), HintObject);
	}
	HintObject->SetStringField(TEXT("fields"), GameTestShot::ToProjectRelative(Path));
}
