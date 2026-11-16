// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ForgeJsonText.h"
#include "AssetForgeCompat.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Policies/PrettyJsonPrintPolicy.h"

namespace ForgeJsonText
{

FString StripComments(const FString& InText)
{
	FString Out;
	Out.Reserve(InText.Len());

	bool bInString = false;
	bool bEscaped = false;

	const int32 Length = InText.Len();
	for (int32 Index = 0; Index < Length; ++Index)
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

		if (Char == TEXT('/') && Index + 1 < Length && InText[Index + 1] == TEXT('/'))
		{
			while (Index < Length && InText[Index] != TEXT('\n'))
			{
				++Index;
			}
			if (Index < Length)
			{
				Out.AppendChar(InText[Index]);
			}
			continue;
		}

		if (Char == TEXT('/') && Index + 1 < Length && InText[Index + 1] == TEXT('*'))
		{
			Index += 2;
			while (Index + 1 < Length && !(InText[Index] == TEXT('*') && InText[Index + 1] == TEXT('/')))
			{
				// 注释内容丢弃，但换行保留：解析报错时行号仍能对上原文件。
				if (InText[Index] == TEXT('\n'))
				{
					Out.AppendChar(TEXT('\n'));
				}
				++Index;
			}
			++Index;
			continue;
		}

		Out.AppendChar(Char);
	}

	return Out;
}

bool ParseObject(const FString& InJson, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
{
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InJson);
	if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
	{
		OutError = FString::Printf(TEXT("JSON 解析失败：%s"), *Reader->GetErrorMessage());
		return false;
	}
	return true;
}

bool LoadObject(const FString& InFilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *InFilePath))
	{
		OutError = FString::Printf(TEXT("无法读取文件：%s"), *InFilePath);
		return false;
	}

	return ParseObject(StripComments(Text), OutObject, OutError);
}

bool SaveObject(const FString& InFilePath, const TSharedPtr<FJsonObject>& InObject, FString& OutError)
{
	if (!InObject.IsValid())
	{
		OutError = TEXT("待写入的 JSON 对象为空");
		return false;
	}

	const FString Directory = FPaths::GetPath(InFilePath);
	if (!Directory.IsEmpty() && !IFileManager::Get().DirectoryExists(*Directory))
	{
		if (!IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true))
		{
			OutError = FString::Printf(TEXT("无法创建目录：%s"), *Directory);
			return false;
		}
	}

	const FString Text = ToPrettyString(InObject);
	if (!FFileHelper::SaveStringToFile(Text, *InFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("无法写入文件：%s"), *InFilePath);
		return false;
	}

	return true;
}

FString NormalizeKey(const FString& InKey)
{
	FString Key = InKey;
	if (Key.Len() > 1 && Key[0] == TEXT('b') && FChar::IsUpper(Key[1]))
	{
		Key.RightChopInline(1);
	}

	Key.ToLowerInline();
	return Key;
}

namespace
{
	/** 数字 token 里可能出现的字符（含科学计数法）。 */
	bool IsNumberChar(TCHAR InChar)
	{
		return FChar::IsDigit(InChar)
			|| InChar == TEXT('.')
			|| InChar == TEXT('-')
			|| InChar == TEXT('+')
			|| InChar == TEXT('e')
			|| InChar == TEXT('E');
	}

	/** 能精确还原 InValue 的最短十进制写法（17 位是 double 的保证上限）。 */
	FString ShortestNumberText(double InValue)
	{
		for (int32 Digits = 1; Digits <= 17; ++Digits)
		{
			const FString Text = FString::Printf(TEXT("%.*g"), Digits, InValue);
			if (FCString::Atod(*Text) == InValue)
			{
				return Text;
			}
		}

		return FString::Printf(TEXT("%.17g"), InValue);
	}

	/** 跳过尾部空白后，最后一个字符是不是「值即将开始」的前导（`:`、`[`、`,`）。 */
	bool IsValuePosition(const FString& InText)
	{
		for (int32 Index = InText.Len() - 1; Index >= 0; --Index)
		{
			const TCHAR Char = InText[Index];
			if (Char == TEXT(' ') || Char == TEXT('\t') || Char == TEXT('\n') || Char == TEXT('\r'))
			{
				continue;
			}

			return Char == TEXT(':') || Char == TEXT('[') || Char == TEXT(',');
		}

		return false;
	}

	/**
	 * 把值位置上的数字改写成最短往返写法。
	 *
	 * 引擎按 17 位有效数字写 double，于是 float 属性（0.6f）导出成 0.6000000238418579：
	 * 配置里全是噪声，更麻烦的是它与手写的 0.6 **判不相等**——每次导入都报同一个差异，
	 * 资产却怎么都不收敛。字符串整体跳过，只动真正的数值 token。
	 */
	FString ShortenNumbers(const FString& InText)
	{
		FString Out;
		Out.Reserve(InText.Len());

		int32 Index = 0;
		while (Index < InText.Len())
		{
			const TCHAR Char = InText[Index];

			if (Char == TEXT('"'))
			{
				Out.AppendChar(Char);
				++Index;
				while (Index < InText.Len())
				{
					const TCHAR Inner = InText[Index];
					Out.AppendChar(Inner);
					++Index;

					if (Inner == TEXT('\\') && Index < InText.Len())
					{
						Out.AppendChar(InText[Index]);
						++Index;
						continue;
					}

					if (Inner == TEXT('"'))
					{
						break;
					}
				}
				continue;
			}

			const bool bNumberStart = FChar::IsDigit(Char)
				|| ((Char == TEXT('-') || Char == TEXT('+'))
					&& Index + 1 < InText.Len()
					&& (FChar::IsDigit(InText[Index + 1]) || InText[Index + 1] == TEXT('.')));

			if (bNumberStart && IsValuePosition(Out))
			{
				int32 End = Index;
				while (End < InText.Len() && IsNumberChar(InText[End]))
				{
					++End;
				}

				Out += ShortestNumberText(FCString::Atod(*InText.Mid(Index, End - Index)));
				Index = End;
				continue;
			}

			Out.AppendChar(Char);
			++Index;
		}

		return Out;
	}
}

FString ToCondensedString(const TSharedPtr<FJsonObject>& InObject)
{
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);

	if (InObject.IsValid())
	{
		FJsonSerializer::Serialize(InObject.ToSharedRef(), Writer);
	}

	Writer->Close();
	return ShortenNumbers(Out);
}

namespace
{
	TSharedPtr<FJsonValue> SortValue(const TSharedPtr<FJsonValue>& InValue);

	/**
	 * 对象引用文本的规范化：两种写法归一路径。
	 *
	 * 同一个引用在两边写法不同：手写配置里是纯路径（`/Game/X.Y`），引擎导出来的带类名
	 * （`/Script/Engine.Texture2D'/Game/X.Y'`）。导入两种都收（都走 StaticLoadObject 那套），
	 * 比较若按原样比，这份**能导入成功**的配置就会被判成「每次都不同」——与键规范化要解决的
	 * 是同一类假差异，只是发生在值上。所以只在这里（比较用的规范形态）动手，写盘仍是原样。
	 *
	 * 规则：剥掉 `类名'…'` 这层壳；`/Game/A/B.B` 这种「末段与前一段同名」的也归一。
	 * 只对看起来像资产路径的字符串动手，其它文本原样返回。
	 */
	FString NormalizeObjectReferenceText(const FString& InText)
	{
		FString Path = InText;

		const int32 FirstQuote = Path.Find(TEXT("'"));
		if (FirstQuote != INDEX_NONE && Path.EndsWith(TEXT("'")))
		{
			Path = Path.Mid(FirstQuote + 1, Path.Len() - FirstQuote - 2);
		}

		if (!Path.StartsWith(TEXT("/")))
		{
			return InText;
		}

		// `/Game/A/B.B` 与 `/Game/A/B` 是同一个对象；而 `B.C` 里的 C 是子对象，不能剥。
		FString Parent;
		FString LastSegment;
		if (Path.Split(TEXT("."), &Parent, &LastSegment, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			const int32 ParentDot = Parent.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			const int32 ParentSlash = Parent.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			const int32 SegmentStart = FMath::Max(ParentDot, ParentSlash) + 1;
			if (Parent.RightChop(SegmentStart) == LastSegment)
			{
				Path = Parent;
			}
		}

		return Path;
	}

	/** 按键递归排序。数组顺序有意义，所以只对元素递归，不重排元素本身。 */
		TSharedPtr<FJsonObject> SortObject(const TSharedPtr<FJsonObject>& InObject)
		{
			if (!InObject.IsValid())
			{
				return nullptr;
			}

			// 规范化后的键 → 原键：规范化只用于比较，取值还得用原键。
			TMap<FString, FString> SortedKeys;
			for (const auto& Pair : InObject->Values)
			{
				const FString Key(ASSETFORGE_JSON_KEY(Pair.Key));
				SortedKeys.Add(NormalizeKey(Key), Key);
			}

			TArray<FString> Ordered;
			SortedKeys.GetKeys(Ordered);
			Ordered.Sort();

			const TSharedPtr<FJsonObject> Sorted = MakeShared<FJsonObject>();
			for (const FString& NormalizedKey : Ordered)
			{
				const FString Key = SortedKeys[NormalizedKey];
				const TSharedPtr<FJsonValue>* Value = InObject->Values.Find(FForgeJsonKey(Key));
				if (Value != nullptr)
				{
					// 写规范化键：比较看的是这份形态，两边才会一致。
					Sorted->SetField(NormalizedKey, SortValue(*Value));
				}
			}

			return Sorted;
		}

	TSharedPtr<FJsonValue> SortValue(const TSharedPtr<FJsonValue>& InValue)
	{
		if (!InValue.IsValid())
		{
			return InValue;
		}

		if (const TSharedPtr<FJsonObject>* ObjectValue = nullptr; InValue->TryGetObject(ObjectValue))
		{
			return MakeShared<FJsonValueObject>(SortObject(*ObjectValue));
		}

		if (const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr; InValue->TryGetArray(ArrayValue))
		{
			TArray<TSharedPtr<FJsonValue>> SortedArray;
			SortedArray.Reserve(ArrayValue->Num());
			for (const TSharedPtr<FJsonValue>& Element : *ArrayValue)
			{
				SortedArray.Add(SortValue(Element));
			}
			return MakeShared<FJsonValueArray>(SortedArray);
		}

		// 字符串按对象引用归一：两种写法（纯路径 / `类名'路径'`）必须判成同一个引用。
		if (InValue->Type == EJson::String)
		{
			return MakeShared<FJsonValueString>(NormalizeObjectReferenceText(InValue->AsString()));
		}

		return InValue;
	}
}

FString ToCanonicalString(const TSharedPtr<FJsonObject>& InObject)
{
	return ToCondensedString(SortObject(InObject));
}

FString ToPrettyString(const TSharedPtr<FJsonObject>& InObject)
{
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);

	if (InObject.IsValid())
	{
		FJsonSerializer::Serialize(InObject.ToSharedRef(), Writer);
	}

	Writer->Close();
	return ShortenNumbers(Out);
}

} // namespace ForgeJsonText
