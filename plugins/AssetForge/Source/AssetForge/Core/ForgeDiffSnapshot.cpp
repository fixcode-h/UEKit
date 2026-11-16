// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ForgeDiffSnapshot.h"
#include "AssetForgeCompat.h"

#include "Core/ForgePropertyCodec.h"
#include "Core/ForgeTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace ForgeDiffSnapshot
{
	bool IsTransientObject(const UObject* InObject)
	{
		if (InObject == nullptr)
		{
			return false;
		}

		const UPackage* OuterPackage = InObject->GetOutermost();
		return OuterPackage != nullptr && OuterPackage->HasAnyFlags(RF_Transient);
	}

	UObject* MakeConfiguredCopy(
		UObject* InTarget,
		const FForgeDocument& InDocument,
		const ForgeKindRegistry::FForgeDiffSupport& InSupport)
	{
		if (InTarget == nullptr)
		{
			return nullptr;
		}

		// 瞬态 Outer：副本不进任何包、不被资产注册表看到、不会被存盘。
		// 用 StaticDuplicateObject 而不是 `DuplicateObject<UObject>`：后者按 `UObject::StaticClass()`
		// 复制，会丢掉真实类型（面板就没属性可显示了）。
		UObject* Copy = StaticDuplicateObject(InTarget, GetTransientPackage());
		if (Copy == nullptr)
		{
			return nullptr;
		}

		Copy->ClearFlags(RF_Public | RF_Standalone);
		Copy->SetFlags(RF_Transient);

		// 副本的包必须是瞬态包。不是就说明复制落到了别的 Outer 上，那可能在保存真实资产时
		// 被一并写盘——宁可放弃也不要冒这个险。
		if (!IsTransientObject(Copy))
		{
			return nullptr;
		}

		TArray<FForgeDiagnostic> Diagnostics;

		// 编解码用的 struct：蓝图写的是相对父类的增量，所以要用父类；
		// 其余 kind 用对象自己的类。
		const UStruct* Struct = Copy->GetClass();
		if (InSupport.bUseParentStruct && !InDocument.Parent.IsEmpty())
		{
			if (UClass* ParentClass = LoadObject<UClass>(nullptr, *InDocument.Parent))
			{
				Struct = ParentClass;
			}
			// 父类读不出来时退回对象自己的类：范围可能偏大，但不会写错对象。
		}

		if (InSupport.bResetBeforeApply)
		{
			ForgePropertyCodec::ResetStructToDefaults(Struct, Copy);
		}

		ForgePropertyCodec::ImportStructProperties(
			Struct,
			Copy,
			InDocument.Root.Props,
			Diagnostics,
			TEXT("root.props"),
			Copy);

		return Copy;
	}

	UObject* ResolveTarget(const FString& InAssetPath, const ForgeKindRegistry::FForgeDiffSupport& InSupport)
	{
		using ForgeKindRegistry::EForgeDiffObjectSource;

		switch (InSupport.Source)
		{
		case EForgeDiffObjectSource::AssetItself:
			return LoadObject<UObject>(nullptr, *InAssetPath);

		case EForgeDiffObjectSource::GeneratedClassDefaultObject:
			// 配置改的是类默认值：比生成类的 CDO，没编过的蓝图退回父类的 CDO
			if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadObject<UObject>(nullptr, *InAssetPath)))
			{
				if (Blueprint->GeneratedClass != nullptr)
				{
					return Blueprint->GeneratedClass->GetDefaultObject();
				}
				if (Blueprint->ParentClass != nullptr)
				{
					return Blueprint->ParentClass->GetDefaultObject();
				}
			}
			return nullptr;

		case EForgeDiffObjectSource::WorldSettings:
		{
			UWorld* World = LoadObject<UWorld>(nullptr, *InAssetPath);
			return World != nullptr ? World->GetWorldSettings(/*bCheckStreamingPersistent=*/false, /*bChecked=*/false) : nullptr;
		}

		default:
			return nullptr;
		}
	}

	namespace
	{
		TSharedRef<FJsonObject> Unsupported(const FString& InReason)
		{
			const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetBoolField(TEXT("supported"), false);
			Out->SetStringField(TEXT("reason"), InReason);
			return Out;
		}

		/** 编解码用的结构：蓝图写的是相对父类的增量，用父类；其余用对象自己的类（和 MakeConfiguredCopy 一致）。 */
		const UStruct* CodecStruct(const UObject* InObject, const FForgeDocument& InDocument, const ForgeKindRegistry::FForgeDiffSupport& InSupport)
		{
			if (InSupport.bUseParentStruct && !InDocument.Parent.IsEmpty())
			{
				if (UClass* ParentClass = LoadObject<UClass>(nullptr, *InDocument.Parent))
				{
					return ParentClass;
				}
			}
			return InObject->GetClass();
		}
	}

	TSharedRef<FJsonObject> DiffProperties(
		const FString& InAssetPath,
		const FForgeDocument& InCurrent,
		const FForgeDocument* InBaseline,
		const int32 InMaxChanges)
	{
		const ForgeKindRegistry::FForgeDiffSupport Support = ForgeKindRegistry::GetPropertyDiffSupport(InCurrent.Kind);
		if (!Support.bSupported)
		{
			return Unsupported(FString::Printf(TEXT("这个资产类型不做对象级属性差异：%s。结构上的改动看「计划」。"), *Support.Reason));
		}
		if (!FPackageName::DoesPackageExist(InAssetPath))
		{
			return Unsupported(FString::Printf(TEXT("目标资产还不存在（%s），导入会全量创建，没有可比较的现状。"), *InAssetPath));
		}
		UObject* Target = ResolveTarget(InAssetPath, Support);
		if (Target == nullptr)
		{
			return Unsupported(FString::Printf(TEXT("读不到 %s 里要比较的对象（蓝图没有生成类、地图没有世界设置）。"), *InAssetPath));
		}

		// 两边都从同一份真实资产复制，差异纯粹来自配置：没有基线时左边就是真实资产本身
		UObject* After = MakeConfiguredCopy(Target, InCurrent, Support);
		UObject* Before = InBaseline != nullptr ? MakeConfiguredCopy(Target, *InBaseline, Support) : Target;
		if (After == nullptr || Before == nullptr)
		{
			return Unsupported(TEXT("没法造出用来比较的临时副本，已放弃属性差异（不能冒险改到真实资产）。"));
		}

		const UStruct* Struct = CodecStruct(After, InCurrent, Support);
		TSharedPtr<FJsonObject> AfterJson = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> BeforeJson = MakeShared<FJsonObject>();
		TArray<FForgeDiagnostic> Diagnostics;
		// 各导出一份「和对方不同的属性」：键相同，值分别是之后和之前
		ForgePropertyCodec::ExportStructDelta(Struct, After, Before, AfterJson, Diagnostics, TEXT("props"));
		ForgePropertyCodec::ExportStructDelta(Struct, Before, After, BeforeJson, Diagnostics, TEXT("props"));

		TArray<FString> Keys;
		for (const auto& Pair : AfterJson->Values)
		{
			Keys.AddUnique(FString(ASSETFORGE_JSON_KEY(Pair.Key)));
		}
		for (const auto& Pair : BeforeJson->Values)
		{
			Keys.AddUnique(FString(ASSETFORGE_JSON_KEY(Pair.Key)));
		}
		Keys.Sort();

		TArray<TSharedPtr<FJsonValue>> Changes;
		for (const FString& Key : Keys)
		{
			if (Changes.Num() >= InMaxChanges)
			{
				break;
			}
			const TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("property"), Key);
			const TSharedPtr<FJsonValue> BeforeValue = BeforeJson->TryGetField(Key);
			const TSharedPtr<FJsonValue> AfterValue = AfterJson->TryGetField(Key);
			Change->SetField(TEXT("before"), BeforeValue.IsValid() ? BeforeValue : MakeShared<FJsonValueNull>());
			Change->SetField(TEXT("after"), AfterValue.IsValid() ? AfterValue : MakeShared<FJsonValueNull>());
			Changes.Add(MakeShared<FJsonValueObject>(Change));
		}

		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("supported"), true);
		Out->SetStringField(TEXT("baseline"), InBaseline != nullptr ? TEXT("config") : TEXT("asset"));
		Out->SetArrayField(TEXT("changes"), Changes);
		Out->SetNumberField(TEXT("total"), Keys.Num());
		if (Keys.Num() > Changes.Num())
		{
			Out->SetBoolField(TEXT("truncated"), true);
		}
		if (!Support.Reason.IsEmpty())
		{
			Out->SetStringField(TEXT("note"), Support.Reason);
		}
		return Out;
	}
}
