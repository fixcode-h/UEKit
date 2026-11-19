// Copyright Epic Games, Inc. All Rights Reserved.

#include "Service/ForgeEditorUtils.h"

#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Registry/ForgeKindRegistry.h"

namespace ForgeEditorUtils
{
	bool IsGeneratedEntry(const FString& InName)
	{
		// `.` 开头的（`.schema/`）也是插件写的生成物；`.`、`..` 是路径里的上级目录写法，不算。
		if (InName.StartsWith(TEXT(".")))
		{
			return InName != TEXT(".") && InName != TEXT("..");
		}
		return InName.StartsWith(TEXT("_"));
	}

	bool IsGeneratedPath(const FString& InPath)
	{
		// Windows 上路径可能混用分隔符，统一成正斜杠再切段。
		FString Normalized = InPath;
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));

		TArray<FString> Segments;
		Normalized.ParseIntoArray(Segments, TEXT("/"), /*InCullEmpty=*/true);

		for (const FString& Segment : Segments)
		{
			if (IsGeneratedEntry(Segment))
			{
				return true;
			}
		}

		return false;
	}

	bool IsNoiseAssetPath(const FString& InPackagePath)
	{
		// `_schema` 一项是历史遗留：它是**配置目录**的命名约定，用在资产路径上语义可疑
		// （资产不该叫这个名字）。保留是为了不改变既有过滤结果；真要清掉得先确认
		// `Content/` 下没有 `_schema` 目录，那是一次单独的行为变更，不夹在本次重构里。
		return InPackagePath.Contains(TEXT("__External"))
			|| InPackagePath.Contains(TEXT("/Developers/"))
			|| InPackagePath.Contains(TEXT("_schema"));
	}

	FForgeConfigClassification ClassifyConfig(const bool bInParsed, const FForgeDocument& InDocument, const FForgeReport& InReport, const TSharedPtr<FJsonObject>& InRawJson, const TArray<FString>& InSupportedKinds)
	{
		FForgeConfigClassification Result;

		if (bInParsed)
		{
			Result.Class = EForgeConfigClass::Parsed;
			Result.Kind = InDocument.Kind;
			Result.AssetPath = InDocument.Asset;
			Result.ParentClass = InDocument.Parent;
			Result.Revision = InDocument.Revision;
			return Result;
		}

		// 文档没建起来时，再看原始 JSON 有没有一个「别人的 kind」。
		// 原始 JSON 都读不出来（纯语法错误）时 InRawJson 为空，直接算解析失败。
		FString RawKind;
		if (InRawJson.IsValid()
			&& InRawJson->TryGetStringField(TEXT("kind"), RawKind)
			&& !RawKind.IsEmpty()
			&& !InSupportedKinds.Contains(RawKind))
		{
			Result.Class = EForgeConfigClass::ForeignKind;
			Result.ForeignKind = RawKind;
			return Result;
		}

		Result.Class = EForgeConfigClass::ParseFailed;
		if (InReport.Diagnostics.Num() > 0)
		{
			Result.ErrorMessage = InReport.Diagnostics[0].Message;
		}
		return Result;
	}

	FForgeConfigClassification ClassifyConfigAtPath(const FString& InAbsolutePath)
	{
		const TArray<FString> SupportedKinds = ForgeKindRegistry::GetSupportedKinds();

		FForgeDocument Document;
		FForgeReport Report;
		if (ForgePipeline::LoadDocument(InAbsolutePath, Document, Report))
		{
			FForgeConfigClassification Parsed = ClassifyConfig(
				/*bInParsed=*/true, Document, Report, /*InRawJson=*/nullptr, SupportedKinds);
			Parsed.Fragments = Document.IncludedFiles;
			return Parsed;
		}

		// 再读一次原始 JSON：文档建不起来的原因是 kind 不认识，而不是 JSON 坏了。
		// 这一次读只为了拿 `kind` 字段，失败也不影响后面的判定。
		TSharedPtr<FJsonObject> RawJson;
		FString LoadError;
		ForgeJsonText::LoadObject(InAbsolutePath, RawJson, LoadError);

		return ClassifyConfig(
			/*bInParsed=*/false, Document, Report, RawJson, SupportedKinds);
	}

	FString ChooseExportConfigPath(const FString& InExistingPath, const FString& InDefaultPath)
	{
		return InExistingPath.IsEmpty() ? InDefaultPath : InExistingPath;
	}
}
