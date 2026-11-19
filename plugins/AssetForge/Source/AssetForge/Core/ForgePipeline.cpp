// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ForgePipeline.h"
#include "AssetForge.h"

#include "Core/ForgeAssetHealth.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeDocSource.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgeSections.h"
#include "Registry/ForgeKindRegistry.h"
#include "Registry/ForgeKindSchema.h"
#include "Settings/AssetForgeSettings.h"

#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"

namespace ForgePipeline
{

namespace
{
	const TCHAR* const FieldSucceeded = TEXT("succeeded");
	const TCHAR* const FieldDryRun = TEXT("dryRun");
	const TCHAR* const FieldSourcePath = TEXT("sourcePath");
	const TCHAR* const FieldAssetPath = TEXT("assetPath");
	const TCHAR* const FieldCreated = TEXT("created");
	const TCHAR* const FieldUpdated = TEXT("updated");
	const TCHAR* const FieldDeleted = TEXT("deleted");
	const TCHAR* const FieldActions = TEXT("actions");
	const TCHAR* const FieldDiagnostics = TEXT("diagnostics");
	const TCHAR* const FieldSeverity = TEXT("severity");
	const TCHAR* const FieldFieldPath = TEXT("fieldPath");
	const TCHAR* const FieldMessage = TEXT("message");

	bool ResolveRefs(FForgeNode& InOutNode, const FString& InNodePath, const FString& InFromConfigPath, TSet<FString>& InVisiting, FForgeReport& OutReport);

	/** 包名 → 对象路径（`/Game/A/B` → `/Game/A/B.B`），已带对象名的原样返回。 */
	FString AsObjectPath(const FString& InAssetPath)
	{
		if (InAssetPath.IsEmpty() || InAssetPath.Contains(TEXT(".")))
		{
			return InAssetPath;
		}
		return InAssetPath + TEXT(".") + FPackageName::GetShortName(InAssetPath);
	}

	/**
	 * 把节点上的 `ref` 解析成 `class`。
	 *
	 * 嵌套子界面在 UMG 里不是「一个 UserWidget 控件 + 指向某个类的属性」——`UUserWidget`
	 * 根本没有这样的属性。**子界面的生成类本身就是那个控件的类**，所以 ref 要做的事只有一件：
	 * 读出被引用配置的目标资产，加生成类后缀 `_C`，写进 class。
	 */
	bool ResolveRefs(FForgeNode& InOutNode, const FString& InNodePath, const FString& InFromConfigPath, TSet<FString>& InVisiting, FForgeReport& OutReport)
	{
		if (!InOutNode.Ref.IsEmpty())
		{
			// 两个来源冲突时报错而不是挑一个：挑错的那次会静默建出一个「没有界面的 UserWidget」，
			// 导入还报成功——这种错必须在解析阶段拦住。
			if (!InOutNode.ClassName.IsEmpty())
			{
				OutReport.AddError(
					FString::Printf(TEXT("%s.ref"), *InNodePath),
					FString::Printf(
						TEXT("节点 `%s` 同时指定了 class 与 ref；类名由 ref 推导，两者只能留一个"),
						*InOutNode.Name));
				return false;
			}

			const FString RefPath = ResolveRefPath(InOutNode.Ref, InFromConfigPath);
			if (!FPaths::FileExists(RefPath))
			{
				OutReport.AddError(
					FString::Printf(TEXT("%s.ref"), *InNodePath),
					FString::Printf(TEXT("引用的配置不存在：%s"), *RefPath));
				return false;
			}

			if (InVisiting.Contains(RefPath))
			{
				OutReport.AddError(
					FString::Printf(TEXT("%s.ref"), *InNodePath),
					FString::Printf(TEXT("配置引用形成环：%s"), *InOutNode.Ref));
				return false;
			}

			InVisiting.Add(RefPath);
			{
				FForgeDocument Referenced;
				FForgeReport ReferenceReport;
				if (!LoadDocument(RefPath, Referenced, ReferenceReport))
				{
					OutReport.AddError(
						FString::Printf(TEXT("%s.ref"), *InNodePath),
						FString::Printf(TEXT("引用的配置无法解析：%s"), *InOutNode.Ref));
					return false;
				}

				// 生成类的完整对象路径是 `/包路径/资产名.资产名_C`——只在末尾拼 `_C` 得到的是
				// `/包路径/资产名_C`，那不是任何对象的名字，LoadClass 找不到它。
				const FString AssetName = FPaths::GetBaseFilename(Referenced.Asset);
				InOutNode.ClassName = FString::Printf(TEXT("%s.%s_C"), *Referenced.Asset, *AssetName);
			}
			InVisiting.Remove(RefPath);
		}

		bool bOk = true;
		for (FForgeNode& Child : InOutNode.Children)
		{
			const FString ChildPath = InNodePath.IsEmpty()
				? Child.Name
				: FString::Printf(TEXT("%s/%s"), *InNodePath, *Child.Name);
			bOk &= ResolveRefs(Child, ChildPath, InFromConfigPath, InVisiting, OutReport);
		}

		return bOk;
	}

	const TCHAR* SeverityToString(EForgeSeverity InSeverity)
	{
		switch (InSeverity)
		{
		case EForgeSeverity::Error:
			return TEXT("error");
		case EForgeSeverity::Warning:
			return TEXT("warning");
		default:
			return TEXT("info");
		}
	}

	const TCHAR* ActionToString(EForgeAction InAction)
	{
		switch (InAction)
		{
		case EForgeAction::Create:
			return TEXT("create");
		case EForgeAction::Update:
			return TEXT("update");
		case EForgeAction::Delete:
			return TEXT("delete");
		case EForgeAction::Reorder:
			return TEXT("reorder");
		default:
			return TEXT("unchanged");
		}
	}

}

FString GetConfigRootDir()
{
	// 由项目设置决定（默认跟着项目的文档根），不写死在代码里。
	return GetDefault<UAssetForgeSettings>()->ResolveConfigRootDir();
}

FString GetKindConfigDir(const FString& InKind)
{
	// kind 名带横线（`umg-widget`），目录用短名——短名在登记表里。
	return FPaths::Combine(GetConfigRootDir(), ForgeKindRegistry::GetConfigDirName(InKind));
}

FString MakeDefaultConfigPath(const FString& InAssetPath, const FString& InKind)
{
	// `/Game/UI/Framework/WBP_UIRoot` → `<配置根>/umg/UI/Framework/WBP_UIRoot.json`
	FString RelativePath = FPackageName::ObjectPathToPackageName(InAssetPath);
	RelativePath.RemoveFromStart(TEXT("/Game/"));
	RelativePath.RemoveFromStart(TEXT("/"));

	return FPaths::Combine(GetKindConfigDir(InKind), RelativePath + TEXT(".json"));
}

FString ResolveConfigPath(const FString& InConfigPath)
{
	if (InConfigPath.IsEmpty() || !FPaths::IsRelative(InConfigPath))
	{
		return InConfigPath;
	}

	const FString ProjectRelative = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectDir(), InConfigPath));
	if (FPaths::FileExists(ProjectRelative))
	{
		return ProjectRelative;
	}

	const FString ConfigRelative = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(GetConfigRootDir(), InConfigPath));
	if (FPaths::FileExists(ConfigRelative))
	{
		return ConfigRelative;
	}

	// 两者都不存在时返回项目相对路径：报错信息里带上它比带引擎目录更有用。
	return ProjectRelative;
}

FString ResolveRefPath(const FString& InRef, const FString& InFromConfigPath)
{
	// 先按引用它的那份配置所在的目录找：父界面和子界面的配置放在一起时写 `./pages/WBP_X.json` 就行，
	// 整个目录拷到别处（示例界面装进配置根的 Test/）也不用改路径。找不到再按项目根、配置根找（老写法）。
	if (!InRef.IsEmpty() && FPaths::IsRelative(InRef) && !InFromConfigPath.IsEmpty())
	{
		const FString Sibling = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::GetPath(InFromConfigPath), InRef));
		if (FPaths::FileExists(Sibling))
		{
			return Sibling;
		}
	}
	return ResolveConfigPath(InRef);
}

namespace
{
	void CollectRefsOfNode(const FForgeNode& InNode, const FString& InFromConfigPath, TArray<FString>& OutConfigPaths)
	{
		if (!InNode.Ref.IsEmpty())
		{
			OutConfigPaths.AddUnique(ResolveRefPath(InNode.Ref, InFromConfigPath));
		}
		for (const FForgeNode& Child : InNode.Children)
		{
			CollectRefsOfNode(Child, InFromConfigPath, OutConfigPaths);
		}
	}
}

bool CollectRefConfigs(const FString& InConfigPath, TArray<FString>& OutConfigPaths)
{
	// 只读文档、不解析 ref：被引用的配置坏了也要把它列出来，让它在批量里自己报错，而不是父配置这里先吞掉。
	// 先展开 include：子界面的 ref 可能写在片段里。
	ForgeDocSource::FExpanded Expanded;
	TArray<FForgeDiagnostic> Diagnostics;
	if (!ForgeDocSource::LoadFile(InConfigPath, Expanded, Diagnostics))
	{
		return false;
	}
	FForgeDocument Document;
	if (!ForgeDoc::Parse(Expanded.Json, InConfigPath, Document, Diagnostics))
	{
		return false;
	}
	CollectRefsOfNode(Document.Root, InConfigPath, OutConfigPaths);
	return true;
}

namespace
{
	/** 文件和文本两条入口共用：字段校验、schema 版本门禁、ref 解析（ref 按 InConfigPath 所在目录找）。 */
	bool ParseLoadedDocument(const TSharedPtr<FJsonObject>& InJson, const FString& InConfigPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
	{
		TArray<FForgeDiagnostic> Diagnostics;
		const bool bParsed = ForgeDoc::Parse(InJson, InConfigPath, OutDocument, Diagnostics);
		OutReport.Diagnostics.Append(MoveTemp(Diagnostics));
		if (!bParsed)
		{
			return false;
		}

		TSet<FString> Visiting;
		Visiting.Add(FPaths::ConvertRelativePathToFull(InConfigPath));
		return ResolveRefs(OutDocument.Root, TEXT("root"), InConfigPath, Visiting, OutReport);
	}
}

namespace
{
	/** 展开 include 后的文档 → 解析；解析出的诊断按片段加上文件名。 */
	bool ParseExpanded(const bool bInExpanded, const ForgeDocSource::FExpanded& InExpanded, TArray<FForgeDiagnostic>& InExpandDiagnostics, const FString& InConfigPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
	{
		// 展开时的诊断不加文件名：它们指的是 include 所在的位置，消息里已经写了片段文件。
		OutReport.Diagnostics.Append(MoveTemp(InExpandDiagnostics));
		if (!bInExpanded)
		{
			return false;
		}
		const int32 FirstDiagnostic = OutReport.Diagnostics.Num();
		const bool bOk = ParseLoadedDocument(InExpanded.Json, InConfigPath, OutDocument, OutReport);
		OutDocument.IncludedFiles = InExpanded.Fragments;
		OutDocument.IncludeOrigins = InExpanded.Origins;
		ForgeDocSource::LabelDiagnostics(InExpanded.Origins, OutReport.Diagnostics, FirstDiagnostic);
		return bOk;
	}
}

bool LoadDocument(const FString& InConfigPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.SourcePath = InConfigPath;

	ForgeDocSource::FExpanded Expanded;
	TArray<FForgeDiagnostic> Diagnostics;
	const bool bExpanded = ForgeDocSource::LoadFile(InConfigPath, Expanded, Diagnostics);
	return ParseExpanded(bExpanded, Expanded, Diagnostics, InConfigPath, OutDocument, OutReport);
}

bool LoadDocumentFromText(const FString& InText, const FString& InConfigPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.SourcePath = InConfigPath;

	ForgeDocSource::FExpanded Expanded;
	TArray<FForgeDiagnostic> Diagnostics;
	const bool bExpanded = ForgeDocSource::LoadText(InText, InConfigPath, Expanded, Diagnostics);
	return ParseExpanded(bExpanded, Expanded, Diagnostics, InConfigPath, OutDocument, OutReport);
}

FString GetSchemaDir()
{
	return FPaths::Combine(GetConfigRootDir(), TEXT(".schema"));
}

FString MakeSchemaRef(const FString& InFilePath, const FString& InSchemaName)
{
	FString Target = FPaths::ConvertRelativePathToFull(FPaths::Combine(GetSchemaDir(), InSchemaName + TEXT(".schema.json")));
	Target.ReplaceInline(TEXT("\\"), TEXT("/"));
	FString FromDir = FPaths::ConvertRelativePathToFull(FPaths::GetPath(InFilePath));
	FromDir.ReplaceInline(TEXT("\\"), TEXT("/"));
	// 相对路径：整个配置根拷到别处（或别人机器上路径不同）时编辑器照样找得到；跨盘时退回绝对路径。
	FPaths::MakePathRelativeTo(Target, *(FromDir + TEXT("/")));
	return Target;
}

bool SaveDocument(const FString& InConfigPath, const FForgeDocument& InDocument, FForgeReport& OutReport)
{
	OutReport.SourcePath = InConfigPath;

	// 按原来的 include 结构拆回各文件；每个文件写前：补 $schema、顶层按段重排（内容不动）、和磁盘上一样就不写。
	TArray<ForgeDocSource::FFileWrite> Writes;
	TArray<FForgeDiagnostic> Diagnostics;
	const bool bCollapsed = ForgeDocSource::Collapse(ForgeDoc::ToJson(InDocument), InConfigPath, ForgeDocSource::DiskLoader(), Writes, Diagnostics);
	OutReport.Diagnostics.Append(MoveTemp(Diagnostics));
	if (!bCollapsed)
	{
		return false;
	}

	const TArray<FString> BodySections = ForgeKindSchema::GetBodySections(InDocument.Kind);
	for (const ForgeDocSource::FFileWrite& Write : Writes)
	{
		if (!Write.Json.IsValid())
		{
			continue;
		}
		Write.Json->SetStringField(TEXT("$schema"), MakeSchemaRef(Write.Path, Write.bFragment ? FString(TEXT("fragment")) : InDocument.Kind));
		// 片段只有 $schema、//、fragment 和内容：不给大段名单，按头字段规则排即可。
		const TSharedPtr<FJsonObject> Ordered = ForgeSections::Reorder(Write.Json, Write.bFragment ? TArray<FString>() : BodySections);

		bool bWritten = false;
		FString Error;
		if (!ForgeDocSource::WriteIfChanged(Write.Path, Ordered, bWritten, Error))
		{
			OutReport.AddError(FString(), Error);
			return false;
		}
	}

	return true;
}

bool Import(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport)
{
	OutReport.bDryRun = bDryRun;
	OutReport.SourcePath = InDocument.SourcePath;
	OutReport.AssetPath = InDocument.Asset;
	const int32 FirstDiagnostic = OutReport.Diagnostics.Num();

	bool bSucceeded = false;

	if (const ForgeKindRegistry::FForgeKindInfo* Info = ForgeKindRegistry::Find(InDocument.Kind))
	{
		bSucceeded = Info->Import(InDocument, bDryRun, OutReport);
	}
	else
	{
		OutReport.AddError(
			TEXT("kind"),
			FString::Printf(
				TEXT("不支持的资产种类 `%s`；配置里的 `kind` 要写已接入的类型之一"),
				*InDocument.Kind));
	}

	// 导入成功 ≠ 资产可用：把资产自己的报错（蓝图编译、材质编译）收进报告——
	// 「写进去了但坏了」按失败处理。
	if (bSucceeded && !bDryRun)
	{
		if (UObject* ImportedAsset = LoadObject<UObject>(nullptr, *AsObjectPath(InDocument.Asset)))
		{
			const int32 DiagnosticsBefore = OutReport.Diagnostics.Num();
			ForgeAssetHealth::CheckAsset(ImportedAsset, OutReport.Diagnostics);

			for (int32 Index = DiagnosticsBefore; Index < OutReport.Diagnostics.Num(); ++Index)
			{
				const FForgeDiagnostic& Diagnostic = OutReport.Diagnostics[Index];
				if (Diagnostic.Severity == EForgeSeverity::Error)
				{
					bSucceeded = false;
				}
				UE_LOG(LogAssetForge, Warning, TEXT("[资产健康] %s：%s"), *Diagnostic.FieldPath, *Diagnostic.Message);
			}
		}
	}

	// 导入的诊断按展开后的字段路径给：来自片段的加上片段文件名，用户才知道去哪个文件改。
	ForgeDocSource::LabelDiagnostics(InDocument.IncludeOrigins, OutReport.Diagnostics, FirstDiagnostic);

	// 清单记录不在这里做：它属于「一次用户操作」的语义，由 ForgeService 统一处理
	// （单元测试直接调 Import 时也就不会污染项目清单）。
	OutReport.bSucceeded = bSucceeded && !OutReport.HasErrors();
	return OutReport.bSucceeded;
}

bool Export(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	UObject* Asset = LoadObject<UObject>(nullptr, *AsObjectPath(InAssetPath));
	if (Asset == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("找不到资产：%s"), *InAssetPath));
		return false;
	}

	// 类型判定只有一份（登记表）：Actor / 对象蓝图的分流也在表里。
	const FString Kind = ForgeKindRegistry::ClassifyAsset(Asset);
	if (const ForgeKindRegistry::FForgeKindInfo* Info = ForgeKindRegistry::Find(Kind))
	{
		const bool bOk = Info->Export(InAssetPath, OutDocument, OutReport);
		OutReport.bSucceeded = bOk && !OutReport.HasErrors();
		return OutReport.bSucceeded;
	}

	OutReport.AddError(
		TEXT("asset"),
		FString::Printf(
			TEXT("暂不支持导出 `%s`（%s）：这类资产还没有对应的处理器"),
			*Asset->GetClass()->GetName(),
			*InAssetPath));
	return false;
}

TSharedPtr<FJsonObject> ReportToJson(const FForgeReport& InReport)
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(FieldSucceeded, InReport.bSucceeded);
	Json->SetBoolField(FieldDryRun, InReport.bDryRun);
	Json->SetStringField(FieldSourcePath, InReport.SourcePath);
	Json->SetStringField(FieldAssetPath, InReport.AssetPath);
	Json->SetNumberField(FieldCreated, InReport.Created);
	Json->SetNumberField(FieldUpdated, InReport.Updated);
	Json->SetNumberField(FieldDeleted, InReport.Deleted);

	TArray<TSharedPtr<FJsonValue>> Actions;
	Actions.Reserve(InReport.Actions.Num());
	for (const FForgePlanEntry& Entry : InReport.Actions)
	{
		const TSharedPtr<FJsonObject> ActionJson = MakeShared<FJsonObject>();
		ActionJson->SetStringField(TEXT("action"), ActionToString(Entry.Action));
		ActionJson->SetStringField(TEXT("nodePath"), Entry.NodePath);
		ActionJson->SetStringField(TEXT("nodeName"), Entry.NodeName);
		if (!Entry.Detail.IsEmpty())
		{
			ActionJson->SetStringField(TEXT("detail"), Entry.Detail);
		}
		Actions.Add(MakeShared<FJsonValueObject>(ActionJson));
	}
	Json->SetArrayField(FieldActions, Actions);

	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	Diagnostics.Reserve(InReport.Diagnostics.Num());
	for (const FForgeDiagnostic& Diagnostic : InReport.Diagnostics)
	{
		const TSharedPtr<FJsonObject> DiagnosticJson = MakeShared<FJsonObject>();
		DiagnosticJson->SetStringField(FieldSeverity, SeverityToString(Diagnostic.Severity));
		DiagnosticJson->SetStringField(FieldFieldPath, Diagnostic.FieldPath);
		DiagnosticJson->SetStringField(FieldMessage, Diagnostic.Message);
		Diagnostics.Add(MakeShared<FJsonValueObject>(DiagnosticJson));
	}
	Json->SetArrayField(FieldDiagnostics, Diagnostics);

	return Json;
}

FString ReportToText(const FForgeReport& InReport)
{
	TArray<FString> Lines;
	Lines.Add(FString::Printf(
		TEXT("%s%s  %s"),
		InReport.bSucceeded ? TEXT("[成功]") : TEXT("[失败]"),
		InReport.bDryRun ? TEXT(" (dry-run)") : TEXT(""),
		*InReport.AssetPath));
	Lines.Add(FString::Printf(
		TEXT("  创建 %d，更新 %d，删除 %d"),
		InReport.Created,
		InReport.Updated,
		InReport.Deleted));

	for (const FForgePlanEntry& Entry : InReport.Actions)
	{
		Lines.Add(FString::Printf(
			TEXT("    %-8s %s%s"),
			ActionToString(Entry.Action),
			*Entry.NodePath,
			Entry.Detail.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("  (%s)"), *Entry.Detail)));
	}

	for (const FForgeDiagnostic& Diagnostic : InReport.Diagnostics)
	{
		Lines.Add(FString::Printf(
			TEXT("    [%s] %s: %s"),
			SeverityToString(Diagnostic.Severity),
			Diagnostic.FieldPath.IsEmpty() ? TEXT("<文件>") : *Diagnostic.FieldPath,
			*Diagnostic.Message));
	}

	return FString::Join(Lines, TEXT("\n"));
}

const TCHAR* ActionToId(const EForgeAction InAction)
{
	return ActionToString(InAction);
}

const TCHAR* SeverityToId(const EForgeSeverity InSeverity)
{
	return SeverityToString(InSeverity);
}

} // namespace ForgePipeline
