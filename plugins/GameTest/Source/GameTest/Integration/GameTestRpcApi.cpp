// Copyright Epic Games, Inc. All Rights Reserved.

// gt.api：接口库列表（自带 + 项目接口），面板的接口选择器和 Agent 都看它。

#include "Api/GameTestApi.h"
#include "Api/GameTestApiFile.h"
#include "Api/GameTestProjectApis.h"
#include "Core/GameTestJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Integration/GameTestRpcScript.h"

namespace
{
	constexpr int32 ApiDefaultLimit = 60;
	constexpr int32 ApiMaxLimit = 200;
	/** 返回预算（协议：默认约 4000 字，列表类最多 12000）：接口说明长，超了截断给 nextOffset。 */
	constexpr int32 ApiMaxChars = 12000;

	bool Matches(const FGameTestApiDesc& InDesc, const FString& InQuery, const FString& InCategory, const FString& InSrc, int32 InReadOnly)
	{
		if (!InSrc.IsEmpty() && InDesc.Src != InSrc)
		{
			return false;
		}
		if (!InCategory.IsEmpty() && !InDesc.Category.Equals(InCategory, ESearchCase::IgnoreCase))
		{
			return false;
		}
		if (InReadOnly >= 0 && InDesc.bReadOnly != (InReadOnly == 1))
		{
			return false;
		}
		return InQuery.IsEmpty() || InDesc.Name.Contains(InQuery) || InDesc.Summary.Contains(InQuery) || InDesc.Display.Contains(InQuery)
			|| InDesc.Category.Contains(InQuery);
	}
}

namespace GameTestRpcScript
{
	bool Api(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		// 面板可能刚扫完 Lua 段（进程外写文件）：按时间戳看一眼
		const FString LoadError = GameTestProjectApis::Refresh();

		FString Query, Category, Src;
		InArgs->TryGetStringField(TEXT("query"), Query);
		InArgs->TryGetStringField(TEXT("category"), Category);
		InArgs->TryGetStringField(TEXT("src"), Src);
		bool bReadOnly = false;
		const int32 ReadOnly = InArgs->TryGetBoolField(TEXT("readOnly"), bReadOnly) ? (bReadOnly ? 1 : 0) : -1;
		int32 Offset = 0, Limit = ApiDefaultLimit;
		InArgs->TryGetNumberField(TEXT("offset"), Offset);
		InArgs->TryGetNumberField(TEXT("limit"), Limit);
		Offset = FMath::Max(0, Offset);
		Limit = FMath::Clamp(Limit, 1, ApiMaxLimit);

		TArray<const FGameTestApiDesc*> Found;
		for (const TSharedRef<IGameTestApi>& Api : GameTestApis::Get().GetAll())
		{
			if (Matches(Api->GetDesc(), Query, Category, Src, ReadOnly))
			{
				Found.Add(&Api->GetDesc());
			}
		}
		// 自带的在前，同来源按分类、名字：面板按分类分组时顺序稳定
		Found.Sort([](const FGameTestApiDesc& A, const FGameTestApiDesc& B)
		{
			const bool bABuiltin = A.Src == TEXT("builtin"), bBBuiltin = B.Src == TEXT("builtin");
			if (bABuiltin != bBBuiltin)
			{
				return bABuiltin;
			}
			return A.Category != B.Category ? A.Category < B.Category : A.Name < B.Name;
		});

		TArray<TSharedPtr<FJsonValue>> Items;
		int32 Chars = 0;
		bool bTruncated = false;
		int32 Index = Offset;
		for (; Index < Found.Num() && Items.Num() < Limit; ++Index)
		{
			const TSharedPtr<FJsonObject> Item = Found[Index]->ToJson();
			Chars += GameTestJson::ToCompactString(Item).Len();
			if (Chars > ApiMaxChars && Items.Num() > 0)
			{
				bTruncated = true;
				break;
			}
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}

		OutResult->SetArrayField(TEXT("items"), Items);
		OutResult->SetNumberField(TEXT("total"), Found.Num());
		OutResult->SetBoolField(TEXT("hasMore"), Index < Found.Num());
		if (Index < Found.Num())
		{
			OutResult->SetNumberField(TEXT("nextOffset"), Index);
		}
		if (bTruncated)
		{
			OutResult->SetBoolField(TEXT("truncated"), true);
		}
		if (!Src.IsEmpty() && Src != TEXT("builtin"))
		{
			OutResult->SetArrayField(TEXT("rejected"), GameTestApiFile::LoadRejected(Src));
		}
		else if (Src.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Rejected = GameTestApiFile::LoadRejected(TEXT("cpp"));
			Rejected.Append(GameTestApiFile::LoadRejected(TEXT("lua")));
			if (Rejected.Num() > 0)
			{
				OutResult->SetArrayField(TEXT("rejected"), Rejected);
			}
		}
		if (!LoadError.IsEmpty())
		{
			// 项目接口读不了不挡自带接口：照常列，错误另报
			OutResult->SetStringField(TEXT("loadError"), LoadError);
		}
		return true;
	}
}
