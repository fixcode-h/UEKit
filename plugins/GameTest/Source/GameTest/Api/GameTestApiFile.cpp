// Copyright Epic Games, Inc. All Rights Reserved.

#include "Api/GameTestApiFile.h"

#include "Core/GameTestJson.h"
#include "GameTestCompat.h"
#include "GameTestSettings.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	const TCHAR* const ApiFileKind = TEXT("gametest-api");
	constexpr int32 FormatVersion = 1;

	/** 比较内容时去掉扫描时间：只有接口真的变了才算变。 */
	FString Fingerprint(const TSharedPtr<FJsonObject>& InSection)
	{
		if (!InSection.IsValid())
		{
			return FString();
		}
		const TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
		Copy->Values = InSection->Values;
		Copy->RemoveField(TEXT("scannedAt"));
		return GameTestJson::ToCompactString(Copy);
	}

	void ReadSection(const FJsonObject& InRoot, const TCHAR* InSrc, TArray<FGameTestProjectApi>& OutItems)
	{
		const TSharedPtr<FJsonObject> Section = GameTestJson::TryGetObject(InRoot, InSrc);
		if (!Section.IsValid())
		{
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* Items = GameTestJson::TryGetArray(*Section, TEXT("items"));
		if (Items == nullptr)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Items)
		{
			const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Item.IsValid())
			{
				continue;
			}
			FGameTestProjectApi Api;
			Api.Src = InSrc;
			GameTestJson::TryGetString(*Item, TEXT("name"), Api.Name);
			GameTestJson::TryGetString(*Item, TEXT("class"), Api.Class);
			GameTestJson::TryGetString(*Item, TEXT("module"), Api.Module);
			GameTestJson::TryGetString(*Item, TEXT("function"), Api.Function);
			GameTestJson::TryGetString(*Item, TEXT("category"), Api.Category);
			GameTestJson::TryGetString(*Item, TEXT("display"), Api.Display);
			GameTestJson::TryGetString(*Item, TEXT("note"), Api.Note);
			GameTestJson::TryGetBool(*Item, TEXT("readOnly"), Api.bReadOnly);
			GameTestJson::TryGetString(*Item, TEXT("file"), Api.File);
			GameTestJson::TryGetInt(*Item, TEXT("line"), Api.Line);
			GameTestJson::TryGetString(*Item, TEXT("returns"), Api.Returns);
			if (const TArray<TSharedPtr<FJsonValue>>* Args = GameTestJson::TryGetArray(*Item, TEXT("args")))
			{
				Api.Args = *Args;
			}
			// 名字或函数缺了的条目调不到：跳过，不让一条坏数据拖垮整个清单
			if (!Api.Name.IsEmpty() && !Api.Function.IsEmpty())
			{
				OutItems.Add(MoveTemp(Api));
			}
		}
	}
}

const TCHAR* GameTestApiFile::FileName()
{
	return TEXT("_api.json");
}

FString GameTestApiFile::GetPath()
{
	return UGameTestSettings::ResolveScenarioRootDir() / FileName();
}

FSimpleMulticastDelegate& GameTestApiFile::OnChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

bool GameTestApiFile::Load(TArray<FGameTestProjectApi>& OutItems, TMap<FString, FString>& OutRenamed, FString& OutError)
{
	OutItems.Reset();
	OutRenamed.Reset();
	const FString Path = GetPath();
	if (!IFileManager::Get().FileExists(*Path))
	{
		return true;
	}
	FString ParseError;
	const TSharedPtr<FJsonObject> Root = GameTestJson::ParseObjectFromFile(Path, ParseError);
	if (!Root.IsValid())
	{
		OutError = FString::Printf(TEXT("%s 读不了：%s（这是生成物，删掉后在编辑器里重扫：gt.api.scan）"), FileName(), *ParseError);
		return false;
	}
	ReadSection(*Root, TEXT("cpp"), OutItems);
	ReadSection(*Root, TEXT("lua"), OutItems);
	if (const TSharedPtr<FJsonObject> Renamed = GameTestJson::TryGetObject(*Root, TEXT("renamed")))
	{
		for (const auto& Pair : Renamed->Values)
		{
			FString NewName;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(NewName))
			{
				OutRenamed.Add(GAMETEST_JSON_KEY(Pair.Key), NewName);
			}
		}
	}
	return true;
}

bool GameTestApiFile::WriteSection(const FString& InSrc, const TSharedRef<FJsonObject>& InSection, bool bInDryRun, bool& bOutChanged, FString& OutError)
{
	bOutChanged = false;
	if (InSrc != TEXT("cpp") && InSrc != TEXT("lua"))
	{
		OutError = FString::Printf(TEXT("_api.json 只有 cpp、lua 两段，没有 %s"), *InSrc);
		return false;
	}

	const FString Path = GetPath();
	TSharedPtr<FJsonObject> Root;
	if (IFileManager::Get().FileExists(*Path))
	{
		FString ParseError;
		Root = GameTestJson::ParseObjectFromFile(Path, ParseError);
		// 原文件坏了：它是生成物，整份重写比报错让人手修更省事；另一段下次扫描时补回来
	}
	if (!Root.IsValid())
	{
		Root = MakeShared<FJsonObject>();
	}

	bOutChanged = Fingerprint(GameTestJson::TryGetObject(*Root, *InSrc)) != Fingerprint(InSection);
	if (!bOutChanged || bInDryRun)
	{
		return true;
	}

	// 固定字段顺序：kind、version 在前，便于人看
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("kind"), ApiFileKind);
	Out->SetNumberField(TEXT("version"), FormatVersion);
	for (const TCHAR* Src : { TEXT("cpp"), TEXT("lua") })
	{
		if (InSrc == Src)
		{
			Out->SetObjectField(Src, InSection);
		}
		else if (const TSharedPtr<FJsonObject> Keep = GameTestJson::TryGetObject(*Root, Src))
		{
			Out->SetObjectField(Src, Keep);
		}
	}
	if (const TSharedPtr<FJsonObject> Renamed = GameTestJson::TryGetObject(*Root, TEXT("renamed")))
	{
		Out->SetObjectField(TEXT("renamed"), Renamed);
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	if (!FFileHelper::SaveStringToFile(GameTestJson::ToPrettyString(Out), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("写不了 %s（文件只读？版本管理里先签出）"), *Path);
		return false;
	}
	OnChanged().Broadcast();
	return true;
}

TArray<TSharedPtr<FJsonValue>> GameTestApiFile::LoadRejected(const FString& InSrc)
{
	const FString Path = GetPath();
	if (!IFileManager::Get().FileExists(*Path))
	{
		return {};
	}
	FString ParseError;
	const TSharedPtr<FJsonObject> Root = GameTestJson::ParseObjectFromFile(Path, ParseError);
	const TSharedPtr<FJsonObject> Section = Root.IsValid() ? GameTestJson::TryGetObject(*Root, *InSrc) : nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Rejected = nullptr;
	if (Section.IsValid() && Section->TryGetArrayField(TEXT("rejected"), Rejected) && Rejected != nullptr)
	{
		return *Rejected;
	}
	return {};
}
