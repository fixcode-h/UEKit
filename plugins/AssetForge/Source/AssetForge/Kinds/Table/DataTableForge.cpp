// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Table/DataTableForge.h"
#include "Core/ForgeDiff.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgePropertyCodec.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "DataTableEditorUtils.h"
#include "Engine/DataTable.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectGlobals.h"

namespace DataTableForge
{

namespace
{
	const TCHAR* const FieldRowStruct = TEXT("rowStruct");

	UDataTable* FindDataTable(const FString& InAssetPath)
	{
		if (InAssetPath.IsEmpty())
		{
			return nullptr;
		}

		return Cast<UDataTable>(LoadObject<UObject>(nullptr, *InAssetPath));
	}

	UScriptStruct* ResolveRowStruct(const FString& InPath, FString& OutError)
	{
		if (InPath.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("缺少 `props.%s`：DataTable 必须指明行结构体（如 /Script/MyGame.FDamageRow）"),
				FieldRowStruct);
			return nullptr;
		}

		UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *InPath);
		if (Struct == nullptr)
		{
			OutError = FString::Printf(TEXT("找不到行结构体 `%s`"), *InPath);
			return nullptr;
		}

		if (!Struct->IsChildOf(FTableRowBase::StaticStruct()))
		{
			OutError = FString::Printf(TEXT("`%s` 不是 FTableRowBase 的派生结构体"), *InPath);
			return nullptr;
		}

		return Struct;
	}

	/** 行的数据指针；不存在时返回 nullptr。 */
	uint8* FindRowData(UDataTable* InTable, const FName InRowName)
	{
		return InTable->GetRowMap().FindRef(InRowName);
	}

	/** 把一行读成节点。 */
	void ReadRowInto(
		UDataTable* InTable,
		const UScriptStruct* InRowStruct,
		const FName InRowName,
		FForgeNode& OutNode,
		TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		OutNode.ClassName = RowClassPlaceholder;
		OutNode.Name = InRowName.ToString();

		if (const uint8* RowData = FindRowData(InTable, InRowName))
		{
			// 行结构体没有 CDO，现造一份默认实例当 delta 基准——否则每行都要把全部字段写出来。
			const FStructOnScope DefaultRow(InRowStruct);

			ForgePropertyCodec::ExportStructDelta(
				InRowStruct,
				RowData,
				DefaultRow.GetStructMemory(),
				OutNode.Props,
				OutDiagnostics,
				FString::Printf(TEXT("root/%s.props"), *InRowName.ToString()));
		}
	}

	bool SaveAssetPackage(UObject* InAsset, FString& OutError)
	{
		UPackage* Package = InAsset->GetOutermost();
		Package->MarkPackageDirty();

		const FString PackageFileName = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()));

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, InAsset, *PackageFileName, SaveArgs))
		{
			OutError = FString::Printf(TEXT("保存资产失败：%s"), *PackageFileName);
			return false;
		}

		return true;
	}
}

bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	UDataTable* Table = FindDataTable(InAssetPath);
	if (Table == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("找不到 DataTable：%s"), *InAssetPath));
		return false;
	}

	const UScriptStruct* RowStruct = Table->GetRowStruct();
	if (RowStruct == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("DataTable %s 没有行结构体"), *InAssetPath));
		return false;
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;
	OutDocument.Parent = DataTableClassPath;

	OutDocument.Root.ClassName = DataTableClassPath;
	OutDocument.Root.Name = Table->GetName();
	OutDocument.Root.Props = MakeShared<FJsonObject>();
	OutDocument.Root.Props->SetStringField(FieldRowStruct, RowStruct->GetPathName());

	// 行按名字排序写出：`GetRowMap()` 是 TMap，迭代顺序不定，而配置文件要能稳定 diff。
	TArray<FName> RowNames = Table->GetRowNames();
	RowNames.Sort([](const FName& InLeft, const FName& InRight)
	{
		return InLeft.ToString() < InRight.ToString();
	});

	OutDocument.Root.Children.Reserve(RowNames.Num());
	for (const FName& RowName : RowNames)
	{
		FForgeNode RowNode;
		ReadRowInto(Table, RowStruct, RowName, RowNode, OutReport.Diagnostics);
		OutDocument.Root.Children.Add(MoveTemp(RowNode));
	}

	OutReport.bSucceeded = true;
	return true;
}

bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport)
{
	OutReport.bDryRun = bDryRun;
	OutReport.SourcePath = InDocument.SourcePath;
	OutReport.AssetPath = InDocument.Asset;

	// rowStruct 不是 DataTable 上的可编辑属性（它是 VisibleAnywhere），所以校验前先摘出来，
	// 由本处理器单独处理；留着它会被判成「类型上没有名为 rowStruct 的属性」。
	FString RowStructPath;
	if (InDocument.Root.Props.IsValid())
	{
		InDocument.Root.Props->TryGetStringField(FieldRowStruct, RowStructPath);
		InDocument.Root.Props->RemoveField(FieldRowStruct);
	}

	FString StructError;
	UScriptStruct* RowStruct = ResolveRowStruct(RowStructPath, StructError);
	if (RowStruct == nullptr)
	{
		OutReport.AddError(FString::Printf(TEXT("root.props.%s"), FieldRowStruct), StructError);
		return false;
	}

	if (InDocument.Root.ClassName != DataTableClassPath)
	{
		OutReport.AddError(
			TEXT("root.class"),
			FString::Printf(
				TEXT("DataTable 的 root.class 固定写 `%s`，实际写的是 `%s`"),
				DataTableClassPath,
				*InDocument.Root.ClassName));
		return false;
	}

	UDataTable* Table = FindDataTable(InDocument.Asset);

	// 逐行校验与归一化：行不写 class，所以只看 props。
	for (FForgeNode& RowNode : InDocument.Root.Children)
	{
		const FString RowPath = FString::Printf(TEXT("root/%s"), *RowNode.Name);

		if (RowNode.Name.IsEmpty())
		{
			OutReport.AddError(RowPath, TEXT("行缺少 name：它就是 RowName，不能为空"));
			continue;
		}

		ForgePropertyCodec::ValidateProperties(
			RowStruct,
			RowNode.Props,
			OutReport.Diagnostics,
			FString::Printf(TEXT("%s.props"), *RowPath));
		ForgePropertyCodec::PruneDefaults(RowStruct, RowNode.Props);
	}

	// 计划：按 RowName 匹配，与 UMG 按控件名匹配是同一套。
	// 当前状态里 root 的 props 刻意留空：配置那侧已经摘掉了 rowStruct，两边得对称，
	// 否则光是 root 自身就会被判成「已更新」。
	FForgeNode CurrentRoot;
	CurrentRoot.ClassName = DataTableClassPath;
	CurrentRoot.Name = InDocument.Root.Name;

	if (Table != nullptr)
	{
		TArray<FName> RowNames = Table->GetRowNames();
		RowNames.Sort([](const FName& InLeft, const FName& InRight)
		{
			return InLeft.ToString() < InRight.ToString();
		});

		CurrentRoot.Children.Reserve(RowNames.Num());
		for (const FName& RowName : RowNames)
		{
			FForgeNode RowNode;
			ReadRowInto(Table, RowStruct, RowName, RowNode, OutReport.Diagnostics);
			CurrentRoot.Children.Add(MoveTemp(RowNode));
		}
	}

	ForgeDiff::ComputeRootPlan(InDocument.Root, CurrentRoot, OutReport.Actions);
	OutReport.CountActions();

	if (bDryRun)
	{
		OutReport.bSucceeded = !OutReport.HasErrors();
		return OutReport.bSucceeded;
	}

	if (OutReport.HasErrors())
	{
		return false;
	}

	if (Table == nullptr)
	{
		UPackage* Package = CreatePackage(*InDocument.Asset);
		if (Package == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("无法创建包：%s"), *InDocument.Asset));
			return false;
		}

		FString PackagePath;
		FString AssetName;
		InDocument.Asset.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (AssetName.IsEmpty())
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(
				TEXT("资产路径 `%s` 不合法，应形如 /Game/Data/DT_Xxx"), *InDocument.Asset));
			return false;
		}

		Table = NewObject<UDataTable>(Package, FName(*AssetName), RF_Public | RF_Standalone);
		if (Table == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("创建 DataTable 失败：%s"), *InDocument.Asset));
			return false;
		}

		Table->RowStruct = RowStruct;
		FAssetRegistryModule::AssetCreated(Table);
		Package->MarkPackageDirty();
	}
	else if (Table->GetRowStruct() != RowStruct)
	{
		// 换行结构体等于换掉整张表的数据布局，不能悄悄做。
		OutReport.AddError(
			TEXT("root.props.rowStruct"),
			FString::Printf(
				TEXT("现有表的行结构体是 `%s`，与配置里的 `%s` 不一致；换结构体会丢掉所有行，请人工处理"),
				*Table->GetRowStruct()->GetPathName(),
				*RowStruct->GetPathName()));
		return false;
	}

	if (OutReport.Actions.Num() == 0)
	{
		OutReport.bSucceeded = true;
		return true;
	}

	// 增 / 改 / 删，与 UMG 的同步策略一致：配置里有而表里没有的建、都有的改、表里多的删。
	TSet<FString> TargetRowNames;
	for (const FForgeNode& RowNode : InDocument.Root.Children)
	{
		TargetRowNames.Add(RowNode.Name);
	}

	for (const FForgeNode& RowNode : InDocument.Root.Children)
	{
		const FName RowName(*RowNode.Name);
		uint8* RowData = FindRowData(Table, RowName);

		if (RowData == nullptr)
		{
			RowData = FDataTableEditorUtils::AddRow(Table, RowName);
			if (RowData == nullptr)
			{
				OutReport.AddError(
					FString::Printf(TEXT("root/%s"), *RowNode.Name),
					TEXT("添加行失败"));
				continue;
			}
		}

		// 先回默认再应用配置：配置没写的字段不该留着上一版的值。
		ForgePropertyCodec::ResetStructToDefaults(RowStruct, RowData);
		ForgePropertyCodec::ImportStructProperties(
			RowStruct,
			RowData,
			RowNode.Props,
			OutReport.Diagnostics,
			FString::Printf(TEXT("root/%s.props"), *RowNode.Name));
	}

	TArray<FName> ExistingRowNames = Table->GetRowNames();
	for (const FName& RowName : ExistingRowNames)
	{
		if (!TargetRowNames.Contains(RowName.ToString()))
		{
			FDataTableEditorUtils::RemoveRow(Table, RowName);
		}
	}

	if (OutReport.HasErrors())
	{
		return false;
	}

	FString SaveError;
	if (!SaveAssetPackage(Table, SaveError))
	{
		OutReport.AddError(TEXT("asset"), SaveError);
		return false;
	}

	OutReport.bSucceeded = true;
	return true;
}

} // namespace DataTableForge
