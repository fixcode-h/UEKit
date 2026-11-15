// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ForgeManifest.h"
#include "Core/ForgeJsonText.h"

#include "Misc/DateTime.h"

namespace ForgeManifest
{

namespace
{
	const TCHAR* const FieldEntries = TEXT("entries");
	const TCHAR* const FieldConfigPath = TEXT("configPath");
	const TCHAR* const FieldContentHash = TEXT("contentHash");
	const TCHAR* const FieldAssetPath = TEXT("assetPath");
	const TCHAR* const FieldImportedAt = TEXT("importedAtUtc");
	const TCHAR* const FieldAssetMTime = TEXT("assetMTimeUtc");
}

bool Load(const FString& InManifestPath, TMap<FString, FManifestEntry>& OutEntries, FString& OutError)
{
	OutEntries.Reset();

	// 清单不存在是正常状态（第一次运行），不是错误。
	if (!FPaths::FileExists(InManifestPath))
	{
		return true;
	}

	TSharedPtr<FJsonObject> Json;
	if (!ForgeJsonText::LoadObject(InManifestPath, Json, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!Json->TryGetArrayField(FieldEntries, Entries))
	{
		return true;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Entries)
	{
		const TSharedPtr<FJsonObject>* EntryObject = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(EntryObject) || !EntryObject->IsValid())
		{
			continue;
		}

		FManifestEntry Entry;
		(*EntryObject)->TryGetStringField(FieldConfigPath, Entry.ConfigPath);
		(*EntryObject)->TryGetStringField(FieldContentHash, Entry.ContentHash);
		(*EntryObject)->TryGetStringField(FieldAssetPath, Entry.AssetPath);
		(*EntryObject)->TryGetStringField(FieldImportedAt, Entry.ImportedAtUtc);
		(*EntryObject)->TryGetStringField(FieldAssetMTime, Entry.AssetMTimeUtc);

		if (!Entry.ConfigPath.IsEmpty())
		{
			OutEntries.Add(Entry.ConfigPath, MoveTemp(Entry));
		}
	}

	return true;
}

bool Save(const FString& InManifestPath, const TMap<FString, FManifestEntry>& InEntries, FString& OutError)
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Reserve(InEntries.Num());

	// 按键排序后写出：清单是要入库的文本，顺序稳定才能让 diff 只显示真正的变化。
	TArray<FString> SortedKeys;
	InEntries.GetKeys(SortedKeys);
	SortedKeys.Sort();

	for (const FString& Key : SortedKeys)
	{
		const FManifestEntry& Entry = InEntries[Key];

		const TSharedPtr<FJsonObject> EntryObject = MakeShared<FJsonObject>();
		EntryObject->SetStringField(FieldConfigPath, Entry.ConfigPath);
		EntryObject->SetStringField(FieldContentHash, Entry.ContentHash);
		EntryObject->SetStringField(FieldAssetPath, Entry.AssetPath);
		EntryObject->SetStringField(FieldImportedAt, Entry.ImportedAtUtc);

		// 老条目（还没重新导入过）没有这个字段：不写空值——它们继续走 dry-run 精判。
		if (!Entry.AssetMTimeUtc.IsEmpty())
		{
			EntryObject->SetStringField(FieldAssetMTime, Entry.AssetMTimeUtc);
		}
		Entries.Add(MakeShared<FJsonValueObject>(EntryObject));
	}

	Json->SetArrayField(FieldEntries, Entries);
	return ForgeJsonText::SaveObject(InManifestPath, Json, OutError);
}

} // namespace ForgeManifest
