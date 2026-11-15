#include "Core/ForgeClassResolver.h"

#include "AssetForge.h"
#include "UObject/UObjectIterator.h"

UClass* FForgeClassResolver::Resolve(const FString& InClassReference, UClass* InRequiredBase, FString& OutError)
{
	OutError.Reset();

	if (InClassReference.IsEmpty())
	{
		OutError = TEXT("类引用为空");
		return nullptr;
	}

	// ① 常规加载：原样、以及补/去 `_C` 的写法（导出写的是 `GetPathName()`，含 `_C`；人工手写往往省略）。
	TArray<FString, TInlineAllocator<2>> Candidates;
	Candidates.Add(InClassReference);
	if (InClassReference.EndsWith(TEXT("_C")))
	{
		Candidates.Add(InClassReference.LeftChop(2));
	}
	else
	{
		Candidates.Add(InClassReference + TEXT("_C"));
	}

	for (const FString& Candidate : Candidates)
	{
		UClass* Loaded = LoadObject<UClass>(nullptr, *Candidate);
		if (Loaded == nullptr)
		{
			continue;
		}

		if (InRequiredBase != nullptr && !Loaded->IsChildOf(InRequiredBase))
		{
			OutError = FString::Printf(
				TEXT("`%s` 不是 `%s` 的子类"),
				*InClassReference,
				*InRequiredBase->GetName());
			return nullptr;
		}

		return Loaded;
	}

	// ② 按类名兜底：UnrealSharp 的 C# 类是托管程序集在运行时创建的、不在包资产里，
	// `LoadObject` 对它们的命中取决于加载时机；遍历一次已加载类把这份不确定性收掉。
	if (UClass* Found = FindLoadedClassByName(ExtractShortName(InClassReference), InRequiredBase))
	{
		return Found;
	}

	// ③ 诊断：把"路径没写对"和"类还没加载"分开说，避免作者对着一个其实没错的路径反复改。
	if (InClassReference.StartsWith(TEXT("/Script/UnrealSharp")))
	{
		OutError = FString::Printf(
			TEXT("找不到类 `%s`：它是 UnrealSharp 的 C# 类，需要等 C# 程序集编译并加载之后才能解析")
			TEXT("（先在编辑器里完成一次 C# 编译/热重载，再重试）"),
			*InClassReference);
	}
	else
	{
		OutError = FString::Printf(TEXT("找不到类 `%s`"), *InClassReference);
	}

	return nullptr;
}

UClass* FForgeClassResolver::ResolveShortName(
	const FString& InShortName,
	const TArray<FString>& InPrefixes,
	UClass* InRequiredBase,
	FString& OutError)
{
	OutError.Reset();

	if (InShortName.IsEmpty())
	{
		OutError = TEXT("类名为空");
		return nullptr;
	}

	for (const FString& Prefix : InPrefixes)
	{
		UClass* Loaded = LoadObject<UClass>(nullptr, *(Prefix + InShortName));
		if (Loaded != nullptr && (InRequiredBase == nullptr || Loaded->IsChildOf(InRequiredBase)))
		{
			return Loaded;
		}
	}

	// 兜底：短名也按"已加载类里同名"找一次——C# 控件类（如项目自己的 Button）靠这条。
	if (UClass* Found = FindLoadedClassByName(InShortName, InRequiredBase))
	{
		return Found;
	}

	OutError = FString::Printf(
		TEXT("找不到类 `%s`（短名只在给定前缀与已加载类里查找，其它类请写完整路径）"),
		*InShortName);
	return nullptr;
}

UClass* FForgeClassResolver::FindLoadedClassByName(const FString& InClassName, UClass* InRequiredBase)
{
	if (InClassName.IsEmpty())
	{
		return nullptr;
	}

	UClass* Found = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Candidate = *It;
		if (Candidate->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}

		if (!Candidate->GetName().Equals(InClassName, ESearchCase::IgnoreCase))
		{
			continue;
		}

		if (InRequiredBase != nullptr && !Candidate->IsChildOf(InRequiredBase))
		{
			continue;
		}

		if (Found != nullptr && Found != Candidate)
		{
			// 同名多个：不猜（写全路径是唯一可靠的消歧手段）。日志留痕帮助排查。
			UE_LOG(LogAssetForge, Warning,
				TEXT("[类解析] 找到多个名为 `%s` 的类（如 `%s` 与 `%s`），请改写完整路径"),
				*InClassName, *Found->GetPathName(), *Candidate->GetPathName());
			return nullptr;
		}

		Found = Candidate;
	}

	return Found;
}

FString FForgeClassResolver::ExtractShortName(const FString& InClassReference)
{
	FString ShortName = InClassReference;
	int32 Index = INDEX_NONE;
	if (ShortName.FindLastChar(TEXT('.'), Index))
	{
		ShortName = ShortName.RightChop(Index + 1);
	}
	if (ShortName.FindLastChar(TEXT('/'), Index))
	{
		ShortName = ShortName.RightChop(Index + 1);
	}

	if (ShortName.EndsWith(TEXT("_C")))
	{
		ShortName.LeftChopInline(2);
	}

	return ShortName;
}
