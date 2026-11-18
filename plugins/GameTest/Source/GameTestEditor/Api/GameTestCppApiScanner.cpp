// Copyright Epic Games, Inc. All Rights Reserved.

#include "Api/GameTestCppApiScanner.h"

#include "Api/GameTestApiFile.h"
#include "Api/GameTestParamKind.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestRunConfig.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SourceCodeNavigation.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/** 项目接口名的前缀：脚本里一眼分得清自带接口和项目接口。 */
	const TCHAR* const ProjectPrefix = TEXT("项目.");

	FString NormalizeFolder(FString InFolder)
	{
		InFolder.ReplaceInline(TEXT("\\"), TEXT("/"));
		InFolder.TrimStartAndEndInline();
		while (InFolder.EndsWith(TEXT("/")))
		{
			InFolder.LeftChopInline(1);
		}
		while (InFolder.StartsWith(TEXT("./")))
		{
			InFolder.RightChopInline(2);
		}
		return InFolder;
	}

	FString ToProjectRelative(const FString& InAbsolute)
	{
		FString Full = FPaths::ConvertRelativePathToFull(InAbsolute);
		FString Project = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeFilename(Full);
		FPaths::NormalizeDirectoryName(Project);
		if (FPaths::MakePathRelativeTo(Full, *(Project + TEXT("/"))))
		{
			return Full;
		}
		return Full;
	}

	/**
	 * 函数声明在头文件第几行：UHT 不留行号，自己在文件里找「函数名(」。找不到返回 0（面板就不给跳转）。
	 * 同一个头文件里的函数一起找，文件只读一次。
	 */
	struct FHeaderLines
	{
		TMap<FString, TArray<FString>> Cache;

		int32 Find(const FString& InFile, const FString& InFunction)
		{
			TArray<FString>* Lines = Cache.Find(InFile);
			if (Lines == nullptr)
			{
				Lines = &Cache.Add(InFile);
				FFileHelper::LoadFileToStringArray(*Lines, *InFile);
			}
			const FRegexPattern Pattern(FString::Printf(TEXT("\\b%s\\s*\\("), *InFunction));
			for (int32 Index = 0; Index < Lines->Num(); ++Index)
			{
				FRegexMatcher Matcher(Pattern, (*Lines)[Index]);
				if (Matcher.FindNext())
				{
					return Index + 1;
				}
			}
			return 0;
		}
	};

	FString Meta(const UField& InField, const TCHAR* InKey)
	{
#if WITH_EDITORONLY_DATA
		return InField.GetMetaData(InKey);
#else
		return FString();
#endif
	}

	TSharedRef<FJsonObject> Rejected(const UClass& InClass, const UFunction& InFunction, const FString& InWhy)
	{
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("class"), InClass.GetPathName());
		O->SetStringField(TEXT("function"), InFunction.GetName());
		O->SetStringField(TEXT("why"), InWhy);
		return O;
	}
}

TArray<FString> GameTestCppApiScanner::DefaultFolders()
{
	return { TEXT("Source/**/GameTestApi") };
}

TArray<FString> GameTestCppApiScanner::ReadFolders()
{
	// 直接读 _settings.json 的 api 段，不走完整的配置校验：配置别处写错了，接口扫描照样能做
	FString Error;
	const TSharedPtr<FJsonObject> Root = IFileManager::Get().FileExists(*GameTestRunConfig::GetPath())
		? GameTestJson::ParseObjectFromFile(GameTestRunConfig::GetPath(), Error)
		: nullptr;
	const TSharedPtr<FJsonObject> Api = Root.IsValid() ? GameTestJson::TryGetObject(*Root, TEXT("api")) : nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Cpp = Api.IsValid() ? GameTestJson::TryGetArray(*Api, TEXT("cpp")) : nullptr;
	if (Cpp == nullptr)
	{
		return DefaultFolders();
	}
	TArray<FString> Out;
	for (const TSharedPtr<FJsonValue>& Value : *Cpp)
	{
		FString Folder;
		if (Value.IsValid() && Value->TryGetString(Folder) && !NormalizeFolder(Folder).IsEmpty())
		{
			Out.Add(NormalizeFolder(Folder));
		}
	}
	return Out;
}

bool GameTestCppApiScanner::InFolders(const FString& InProjectRelative, const TArray<FString>& InFolders)
{
	FString Path = InProjectRelative;
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));
	for (const FString& Raw : InFolders)
	{
		const FString Folder = NormalizeFolder(Raw);
		if (Folder.IsEmpty())
		{
			continue;
		}
		// MatchesWildcard 的 * 会跨 /，所以 ** 直接换成 *；「/**/」还要能匹配零层，再试一次去掉它的写法
		const FString Pattern = Folder.Replace(TEXT("**"), TEXT("*")) + TEXT("/*");
		const FString ZeroDepth = Folder.Replace(TEXT("/**/"), TEXT("/")).Replace(TEXT("**"), TEXT("*")) + TEXT("/*");
		if (Path.MatchesWildcard(Pattern, ESearchCase::IgnoreCase) || Path.MatchesWildcard(ZeroDepth, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

FString GameTestCppApiScanner::ShortClassName(const FString& InClassName)
{
	FString Name = InClassName;
	// 类的反射名已经去掉了 U 前缀；从类路径来的再去一次也无妨
	if (Name.Len() > 1 && Name[0] == TEXT('U') && FChar::IsUpper(Name[1]))
	{
		Name.RightChopInline(1);
	}
	for (const TCHAR* Suffix : { TEXT("BlueprintFunctionLibrary"), TEXT("FunctionLibrary"), TEXT("BlueprintLibrary"), TEXT("Library"), TEXT("Lib") })
	{
		if (Name.Len() > FCString::Strlen(Suffix) && Name.EndsWith(Suffix, ESearchCase::CaseSensitive))
		{
			Name.LeftChopInline(FCString::Strlen(Suffix));
			break;
		}
	}
	return Name;
}

TSharedRef<FJsonObject> GameTestCppApiScanner::Scan(const TArray<FString>& InFolders)
{
	TArray<TSharedPtr<FJsonObject>> Items;
	TArray<TSharedPtr<FJsonValue>> RejectedItems;
	TSet<FString> Names;
	FHeaderLines Lines;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		// 只认 C++ 写的函数库：蓝图函数库按约定不认（协议 8.2），热重载留下的旧类（REINST_）跳过
		if (Class == nullptr || !Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass()) || Class == UBlueprintFunctionLibrary::StaticClass()
			|| !Class->HasAnyClassFlags(CLASS_Native) || Class->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated))
		{
			continue;
		}
		FString Header;
		if (!FSourceCodeNavigation::FindClassHeaderPath(Class, Header))
		{
			continue;
		}
		const FString RelHeader = ToProjectRelative(Header);
		if (!InFolders(RelHeader, InFolders))
		{
			continue;
		}

		const FString Short = ShortClassName(Class->GetName());
		for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
		{
			UFunction* Function = *FuncIt;
			if (Function == nullptr || !Function->HasAnyFunctionFlags(FUNC_Static)
				|| !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
			{
				continue;
			}
			FString Why;
			if (!GameTestParamKind::IsCallable(*Function, Why))
			{
				RejectedItems.Add(MakeShared<FJsonValueObject>(Rejected(*Class, *Function, Why)));
				continue;
			}
			const FString Name = FString::Printf(TEXT("%s%s.%s"), ProjectPrefix, *Short, *Function->GetName());
			if (Names.Contains(Name))
			{
				RejectedItems.Add(MakeShared<FJsonValueObject>(Rejected(*Class, *Function,
					FString::Printf(TEXT("接口名 %s 和别的类重了：类名去掉 Library 后缀后相同，改一个类名"), *Name))));
				continue;
			}
			Names.Add(Name);

			const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Name);
			Item->SetStringField(TEXT("class"), Class->GetPathName());
			Item->SetStringField(TEXT("function"), Function->GetName());
			const FString Category = Meta(*Function, TEXT("Category"));
			if (!Category.IsEmpty())
			{
				Item->SetStringField(TEXT("category"), Category);
			}
			const FString Display = Meta(*Function, TEXT("DisplayName"));
			if (!Display.IsEmpty())
			{
				Item->SetStringField(TEXT("display"), Display);
			}
			const FString Note = Meta(*Function, TEXT("ToolTip"));
			if (!Note.IsEmpty())
			{
				Item->SetStringField(TEXT("note"), Note);
			}
			Item->SetBoolField(TEXT("readOnly"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
			Item->SetStringField(TEXT("file"), RelHeader);
			const int32 Line = Lines.Find(Header, Function->GetName());
			if (Line > 0)
			{
				Item->SetNumberField(TEXT("line"), Line);
			}
			Item->SetArrayField(TEXT("args"), GameTestParamKind::ToJson(GameTestParamKind::Describe(*Function)));
			const FString Returns = GameTestParamKind::ReturnType(*Function);
			if (!Returns.IsEmpty())
			{
				Item->SetStringField(TEXT("returns"), Returns);
			}
			Items.Add(Item);
		}
	}

	// 按名字排序：清单进版本库，顺序固定才看得出真正的差异
	Items.Sort([](const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
	{
		return A->GetStringField(TEXT("name")) < B->GetStringField(TEXT("name"));
	});
	TArray<TSharedPtr<FJsonValue>> ItemValues;
	for (const TSharedPtr<FJsonObject>& Item : Items)
	{
		ItemValues.Add(MakeShared<FJsonValueObject>(Item));
	}
	TArray<TSharedPtr<FJsonValue>> FolderValues;
	for (const FString& Folder : InFolders)
	{
		FolderValues.Add(MakeShared<FJsonValueString>(Folder));
	}

	const TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
	Section->SetStringField(TEXT("scannedAt"), FDateTime::UtcNow().ToIso8601());
	Section->SetArrayField(TEXT("folders"), FolderValues);
	Section->SetArrayField(TEXT("items"), ItemValues);
	Section->SetArrayField(TEXT("rejected"), RejectedItems);
	return Section;
}

bool GameTestCppApiScanner::ScanAndWrite(bool bInDryRun, FGameTestCppApiScanResult& OutResult, FString& OutError)
{
	const TSharedRef<FJsonObject> Section = Scan(ReadFolders());
	OutResult.Items = Section->GetArrayField(TEXT("items")).Num();
	OutResult.Rejected = Section->GetArrayField(TEXT("rejected")).Num();
	OutResult.Path = ToProjectRelative(GameTestApiFile::GetPath());
	OutResult.Section = Section;
	return GameTestApiFile::WriteSection(TEXT("cpp"), Section, bInDryRun, OutResult.bChanged, OutError);
}
