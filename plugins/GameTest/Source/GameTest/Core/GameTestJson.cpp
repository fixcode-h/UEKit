// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FString GameTestJson::StripComments(const FString& InText)
{
	FString Out;
	Out.Reserve(InText.Len());

	bool bInString = false;
	bool bEscaped = false;

	for (int32 Index = 0; Index < InText.Len(); ++Index)
	{
		const TCHAR Char = InText[Index];

		if (bInString)
		{
			Out.AppendChar(Char);
			if (bEscaped)
			{
				bEscaped = false;
			}
			else if (Char == TEXT('\\'))
			{
				bEscaped = true;
			}
			else if (Char == TEXT('"'))
			{
				bInString = false;
			}
			continue;
		}

		if (Char == TEXT('"'))
		{
			bInString = true;
			Out.AppendChar(Char);
			continue;
		}

		if (Char == TEXT('/') && Index + 1 < InText.Len() && InText[Index + 1] == TEXT('/'))
		{
			// 跳到行尾：换行符本身留下，行号因此不会错位。
			while (Index < InText.Len() && InText[Index] != TEXT('\n'))
			{
				++Index;
			}
			if (Index < InText.Len())
			{
				Out.AppendChar(InText[Index]);
			}
			continue;
		}

		Out.AppendChar(Char);
	}

	return Out;
}

TSharedPtr<FJsonObject> GameTestJson::ParseObjectFromText(const FString& InText, FString& OutError)
{
	const FString Stripped = StripComments(InText);

	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Stripped);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		OutError = FString::Printf(TEXT("JSON 解析失败：%s"), *Reader->GetErrorMessage());
		return nullptr;
	}

	return Object;
}

bool GameTestJson::ReadTextFile(const FString& InAbsolutePath, FString& OutText, FString& OutError)
{
	if (!IFileManager::Get().FileExists(*InAbsolutePath))
	{
		OutError = FString::Printf(TEXT("文件不存在：%s"), *InAbsolutePath);
		return false;
	}

	if (!FFileHelper::LoadFileToString(OutText, *InAbsolutePath))
	{
		OutError = FString::Printf(TEXT("读取失败：%s"), *InAbsolutePath);
		return false;
	}

	return true;
}

TSharedPtr<FJsonObject> GameTestJson::ParseObjectFromFile(const FString& InAbsolutePath, FString& OutError)
{
	FString Text;
	if (!ReadTextFile(InAbsolutePath, Text, OutError))
	{
		return nullptr;
	}

	FString ParseError;
	const TSharedPtr<FJsonObject> Object = ParseObjectFromText(Text, ParseError);
	if (!Object.IsValid())
	{
		OutError = FString::Printf(TEXT("%s：%s"), *InAbsolutePath, *ParseError);
		return nullptr;
	}

	return Object;
}

bool GameTestJson::TryGetString(const FJsonObject& InObject, const TCHAR* InField, FString& OutValue)
{
	return InObject.TryGetStringField(InField, OutValue);
}

bool GameTestJson::TryGetInt(const FJsonObject& InObject, const TCHAR* InField, int32& OutValue)
{
	double Value = 0.0;
	if (!InObject.TryGetNumberField(InField, Value))
	{
		return false;
	}

	OutValue = static_cast<int32>(Value);
	return true;
}

bool GameTestJson::TryGetNumber(const FJsonObject& InObject, const TCHAR* InField, double& OutValue)
{
	return InObject.TryGetNumberField(InField, OutValue);
}

bool GameTestJson::TryGetBool(const FJsonObject& InObject, const TCHAR* InField, bool& OutValue)
{
	return InObject.TryGetBoolField(InField, OutValue);
}

TSharedPtr<FJsonObject> GameTestJson::TryGetObject(const FJsonObject& InObject, const TCHAR* InField)
{
	const TSharedPtr<FJsonObject>* Found = nullptr;
	if (InObject.TryGetObjectField(InField, Found) && Found != nullptr)
	{
		return *Found;
	}
	return nullptr;
}

const TArray<TSharedPtr<FJsonValue>>* GameTestJson::TryGetArray(const FJsonObject& InObject, const TCHAR* InField)
{
	const TArray<TSharedPtr<FJsonValue>>* Found = nullptr;
	if (InObject.TryGetArrayField(InField, Found))
	{
		return Found;
	}
	return nullptr;
}

FString GameTestJson::ToCompactString(const TSharedPtr<FJsonObject>& InJson)
{
	if (!InJson.IsValid())
	{
		return TEXT("{}");
	}

	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(InJson.ToSharedRef(), Writer);
	return Out;
}

FString GameTestJson::ToPrettyString(const TSharedPtr<FJsonObject>& InJson)
{
	if (!InJson.IsValid())
	{
		return TEXT("{}");
	}

	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(InJson.ToSharedRef(), Writer);
	return Out;
}

FString GameTestJson::ToCompactString(const TArray<TSharedPtr<FJsonValue>>& InArray)
{
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(InArray, Writer);
	return Out;
}
