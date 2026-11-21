// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestPropPathKey.h"

#include "Observe/Ref/GameTestPropPath.h"

FString GameTestPropPathKey::FromSource(const FString& InSource, const FString& InPath)
{
	if (InSource.Equals(TEXT("text"), ESearchCase::IgnoreCase))
	{
		return FGameTestPropPath::OfState(TEXT("text"));
	}
	if (InPath.IsEmpty())
	{
		return FString();
	}
	if (InSource.Equals(TEXT("self"), ESearchCase::IgnoreCase))
	{
		return TEXT("@.") + InPath;
	}
	if (InSource.Equals(TEXT("lua"), ESearchCase::IgnoreCase))
	{
		return TEXT("@#") + InPath;
	}
	if (InSource.Equals(TEXT("item"), ESearchCase::IgnoreCase))
	{
		return TEXT("@item.") + InPath;
	}
	if (InSource.Equals(TEXT("itemLua"), ESearchCase::IgnoreCase))
	{
		return TEXT("@item#") + InPath;
	}
	return FString();
}

FString GameTestPropPathKey::FromBy(const FString& InBy, bool bInList)
{
	const FString By = InBy.TrimStartAndEnd();
	if (!By.StartsWith(TEXT("@")))
	{
		return FromSource(bInList ? TEXT("item") : TEXT("self"), By);
	}
	FString Source;
	FString Path;
	if (!By.Mid(1).Split(TEXT("."), &Source, &Path))
	{
		Source = By.Mid(1);
	}
	return FromSource(Source, Path);
}

bool GameTestPropPathKey::ToBy(const FString& InProp, bool bInList, FString& OutBy)
{
	FGameTestPropPath Path;
	FString Error;
	if (!FGameTestPropPath::Parse(InProp, Path, Error))
	{
		return false;
	}
	if (Path.Segs.Num() == 0)
	{
		if (!Path.bItem && Path.State.Equals(TEXT("text"), ESearchCase::IgnoreCase))
		{
			OutBy = TEXT("@text");
			return true;
		}
		return false;
	}
	if (Path.HasState() || Path.Segs[0].Kind == EGameTestPathSeg::Child || Path.Segs[0].Kind == EGameTestPathSeg::Index)
	{
		return false;
	}
	// 字段路径：第一段之后 . 分开，下标写 [n]（GameTestItemKey 的字段写法）
	FString Field = Path.Segs[0].Name;
	for (int32 Index = 1; Index < Path.Segs.Num(); ++Index)
	{
		const FGameTestPathSeg& Seg = Path.Segs[Index];
		if (Seg.Kind == EGameTestPathSeg::Index)
		{
			Field += FString::Printf(TEXT("[%s]"), *Seg.Name);
		}
		else if (Seg.Kind == EGameTestPathSeg::Prop || Seg.Kind == EGameTestPathSeg::Lua)
		{
			Field += TEXT(".") + Seg.Name;
		}
		else
		{
			// 子控件、函数：条目标识读不了
			return false;
		}
	}
	const bool bLua = Path.Segs[0].Kind == EGameTestPathSeg::Lua;
	if (Path.Segs[0].Kind == EGameTestPathSeg::Fn)
	{
		return false;
	}
	if (Path.bItem)
	{
		OutBy = bLua ? TEXT("@itemLua.") + Field : (bInList ? Field : TEXT("@item.") + Field);
	}
	else
	{
		OutBy = bLua ? TEXT("@lua.") + Field : (bInList ? TEXT("@self.") + Field : Field);
	}
	return true;
}
