// Copyright Epic Games, Inc. All Rights Reserved.

#include "Registry/ForgeKindRegistry.h"

#include "Kinds/Actor/ActorBlueprintForge.h"
#include "Kinds/Anim/AnimBlueprintForge.h"
#include "Kinds/Anim/AnimSequenceForge.h"
#include "Kinds/BehaviorTree/BehaviorTreeForge.h"
#include "Kinds/Data/DataAssetForge.h"
#include "Kinds/Font/FontForge.h"
#include "Kinds/Map/MapForge.h"
#include "Kinds/Material/MaterialForge.h"
#include "Kinds/Niagara/NiagaraForge.h"
#include "Kinds/Object/ObjectBlueprintForge.h"
#include "Kinds/Physics/PhysicsAssetForge.h"
#include "Kinds/SkeletalMesh/SkeletalMeshForge.h"
#include "Kinds/Skeleton/SkeletonForge.h"
#include "Kinds/StateTree/StateTreeForge.h"
#include "Kinds/Table/DataTableForge.h"
#include "Kinds/Texture/TextureForge.h"
#include "Kinds/Umg/UmgForge.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetData.h"
#include "BehaviorTree/BehaviorTree.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "Engine/Font.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Misc/PackageName.h"
#include "NiagaraSystem.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "StateTree.h"
#include "UObject/SoftObjectPath.h"
#include "WidgetBlueprint.h"

namespace ForgeKindRegistry
{

namespace
{
	FForgeDiffSupport DiffResetThenApply(EForgeDiffObjectSource InSource)
	{
		FForgeDiffSupport Support;
		Support.bSupported = true;
		Support.Source = InSource;
		Support.bResetBeforeApply = true;
		return Support;
	}

	/** 纳管型：只写配置声明的属性，其余保留原值。 */
	FForgeDiffSupport DiffApplyDeclared()
	{
		FForgeDiffSupport Support;
		Support.bSupported = true;
		Support.Source = EForgeDiffObjectSource::AssetItself;
		Support.bResetBeforeApply = false;
		return Support;
	}

	FForgeDiffSupport DiffUnsupported(const TCHAR* InReason)
	{
		FForgeDiffSupport Support;
		Support.Reason = InReason;
		return Support;
	}

	bool IsActorNativeParent(const UClass* /*InAssetClass*/, const UClass* InNativeParent)
	{
		return InNativeParent != nullptr && InNativeParent->IsChildOf(AActor::StaticClass());
	}

	bool IsNonActorNativeParent(const UClass* InAssetClass, const UClass* InNativeParent)
	{
		return !IsActorNativeParent(InAssetClass, InNativeParent);
	}

	/**
	 * 登记表本体。函数内静态：表里有 FString，放在命名空间作用域会撞上静态初始化顺序。
	 *
	 * `DiffSupport` 的 `bResetBeforeApply` / `bUseParentStruct` 照着真实导入代码抄，
	 * 一致性由 `AssetForge.Registry` 与 `AssetForge.Editor.DiffSnapshot` 用例守着。
	 */
	const TArray<FForgeKindInfo>& Table()
	{
		static const TArray<FForgeKindInfo> Kinds = []()
		{
			TArray<FForgeKindInfo> Out;

			auto Add = [&Out](
				const TCHAR* InKind,
				const TCHAR* InDisplayName,
				const TCHAR* InDir,
				FAssetClassFn InClass,
				FAcceptsFn InAccepts,
				FImportDocumentFn InImport,
				FExportAssetFn InExport,
				FForgeDiffSupport InDiff)
			{
				FForgeKindInfo& Info = Out.AddDefaulted_GetRef();
				Info.Kind = InKind;
				Info.DisplayName = InDisplayName;
				Info.ConfigDirName = InDir;
				Info.GetAssetClass = InClass;
				Info.Accepts = InAccepts;
				Info.Import = InImport;
				Info.Export = InExport;
				Info.DiffSupport = MoveTemp(InDiff);
			};

			Add(UmgForge::Kind, TEXT("UMG 控件蓝图"), TEXT("umg"),
				[]() -> UClass* { return UWidgetBlueprint::StaticClass(); }, nullptr,
				&UmgForge::ImportDocument, &UmgForge::ExportAsset,
				DiffUnsupported(TEXT("控件节点能映射到 UWidget，但需要先选定「比哪个控件」——逐节点差异是后续工作。")));

			Add(DataAssetForge::Kind, TEXT("数据资产"), TEXT("data"),
				[]() -> UClass* { return UDataAsset::StaticClass(); }, nullptr,
				&DataAssetForge::ImportDocument, &DataAssetForge::ExportAsset,
				DiffResetThenApply(EForgeDiffObjectSource::AssetItself));

			Add(DataTableForge::Kind, TEXT("数据表"), TEXT("table"),
				[]() -> UClass* { return UDataTable::StaticClass(); }, nullptr,
				&DataTableForge::ImportDocument, &DataTableForge::ExportAsset,
				DiffUnsupported(TEXT("数据表的每一行是「uint8* + UScriptStruct」而不是 UObject，属性细节面板没有对象可设。")));

			Add(ActorBlueprintForge::Kind, TEXT("Actor 蓝图"), TEXT("actor"),
				[]() -> UClass* { return UBlueprint::StaticClass(); }, &IsActorNativeParent,
				&ActorBlueprintForge::ImportDocument, &ActorBlueprintForge::ExportAsset,
				DiffUnsupported(TEXT("组件节点能映射到 ComponentTemplate，同样需要先选定「比哪个组件」——逐节点差异是后续工作。")));

			Add(BehaviorTreeForge::Kind, TEXT("行为树"), TEXT("bt"),
				[]() -> UClass* { return UBehaviorTree::StaticClass(); }, nullptr,
				&BehaviorTreeForge::ImportDocument, &BehaviorTreeForge::ExportAsset,
				DiffUnsupported(TEXT("行为树节点能映射到 UBTNode，同样需要先选定「比哪个节点」——逐节点差异是后续工作。")));

			Add(StateTreeForge::Kind, TEXT("状态树"), TEXT("state-tree"),
				[]() -> UClass* { return UStateTree::StaticClass(); }, nullptr,
				&StateTreeForge::ImportDocument, &StateTreeForge::ExportAsset,
				DiffUnsupported(TEXT("状态与节点是层级对象，同样需要先选定「比哪个状态/节点」——逐节点差异是后续工作。")));

			Add(AnimBlueprintForge::Kind, TEXT("动画蓝图"), TEXT("anim"),
				[]() -> UClass* { return UAnimBlueprint::StaticClass(); }, nullptr,
				&AnimBlueprintForge::ImportDocument, &AnimBlueprintForge::ExportAsset,
				DiffUnsupported(TEXT("动画蓝图的状态机是图数据、序列播放器的属性住在 FAnimNode_SequencePlayer（结构体），都不是反射属性。")));

			Add(AnimSequenceForge::Kind, TEXT("动画序列（含蒙太奇）"), TEXT("anim-seq"),
				[]() -> UClass* { return UAnimSequenceBase::StaticClass(); }, nullptr,
				&AnimSequenceForge::ImportDocument, &AnimSequenceForge::ExportAsset,
				DiffApplyDeclared());

			Add(PhysicsAssetForge::Kind, TEXT("物理资产"), TEXT("physics-asset"),
				[]() -> UClass* { return UPhysicsAsset::StaticClass(); }, nullptr,
				&PhysicsAssetForge::ImportDocument, &PhysicsAssetForge::ExportAsset,
				DiffApplyDeclared());

			Add(SkeletonForge::Kind, TEXT("骨骼"), TEXT("skeleton"),
				[]() -> UClass* { return USkeleton::StaticClass(); }, nullptr,
				&SkeletonForge::ImportDocument, &SkeletonForge::ExportAsset,
				DiffApplyDeclared());

			Add(SkeletalMeshForge::Kind, TEXT("骨骼网格体"), TEXT("skeletal-mesh"),
				[]() -> UClass* { return USkeletalMesh::StaticClass(); }, nullptr,
				&SkeletalMeshForge::ImportDocument, &SkeletalMeshForge::ExportAsset,
				DiffApplyDeclared());

			// `UMaterial` 不覆盖材质实例（UMaterialInstanceConstant 走 UMaterialInterface 支线）。
			Add(MaterialForge::Kind, TEXT("材质"), TEXT("material"),
				[]() -> UClass* { return UMaterial::StaticClass(); }, nullptr,
				&MaterialForge::ImportDocument, &MaterialForge::ExportAsset,
				DiffUnsupported(TEXT("材质图的节点与连线写在配置的 extra.nodes 里，不是属性；根 UMaterial 的属性又覆盖不到图。")));

			Add(MapForge::Kind, TEXT("地图（世界设置）"), TEXT("map"),
				[]() -> UClass* { return UWorld::StaticClass(); }, nullptr,
				&MapForge::ImportDocument, &MapForge::ExportAsset,
				DiffResetThenApply(EForgeDiffObjectSource::WorldSettings));

			{
				FForgeDiffSupport ObjectDiff = DiffResetThenApply(EForgeDiffObjectSource::GeneratedClassDefaultObject);
				ObjectDiff.bUseParentStruct = true;
				Add(ObjectBlueprintForge::Kind, TEXT("对象蓝图（非 Actor）"), TEXT("object"),
					[]() -> UClass* { return UBlueprint::StaticClass(); }, &IsNonActorNativeParent,
					&ObjectBlueprintForge::ImportDocument, &ObjectBlueprintForge::ExportAsset,
					MoveTemp(ObjectDiff));
			}

			Add(NiagaraForge::Kind, TEXT("Niagara 系统"), TEXT("niagara"),
				[]() -> UClass* { return UNiagaraSystem::StaticClass(); }, nullptr,
				&NiagaraForge::ImportDocument, &NiagaraForge::ExportAsset,
				DiffUnsupported(TEXT("Niagara 的参数值住在 FNiagaraParameterStore，不是反射属性；发射器句柄也是结构体。")));

			Add(TextureForge::Kind, TEXT("纹理贴图"), TEXT("texture"),
				[]() -> UClass* { return UTexture2D::StaticClass(); }, nullptr,
				&TextureForge::ImportDocument, &TextureForge::ExportAsset,
				DiffResetThenApply(EForgeDiffObjectSource::AssetItself));

			// 字体：刻意不认 UFontFace——它是 UFont 的附属资产，单独导出没有可回写的配置形态。
			Add(FontForge::Kind, TEXT("字体"), TEXT("font"),
				[]() -> UClass* { return UFont::StaticClass(); }, nullptr,
				&FontForge::ImportDocument, &FontForge::ExportAsset,
				DiffUnsupported(TEXT("字体的内容（字体面列表与源文件）在 children 里、不在 UFont 属性上；逐字段面板会把结构差异显示成「无差异」。")));

			return Out;
		}();

		return Kinds;
	}

	int32 ClassDepth(const UClass* InClass)
	{
		int32 Depth = 0;
		for (const UClass* Class = InClass; Class != nullptr; Class = Class->GetSuperClass())
		{
			++Depth;
		}
		return Depth;
	}

	const UClass* FindNativeClass(const UClass* InClass)
	{
		const UClass* Class = InClass;
		while (Class != nullptr && !Class->HasAnyClassFlags(CLASS_Native))
		{
			Class = Class->GetSuperClass();
		}
		return Class;
	}
}

TConstArrayView<FForgeKindInfo> GetAll()
{
	return Table();
}

const FForgeKindInfo* Find(const FString& InKind)
{
	for (const FForgeKindInfo& Info : Table())
	{
		if (InKind == Info.Kind)
		{
			return &Info;
		}
	}
	return nullptr;
}

TArray<FString> GetSupportedKinds()
{
	TArray<FString> Kinds;
	Kinds.Reserve(Table().Num());
	for (const FForgeKindInfo& Info : Table())
	{
		Kinds.Add(Info.Kind);
	}
	return Kinds;
}

bool IsSupportedKind(const FString& InKind)
{
	return Find(InKind) != nullptr;
}

FString GetDisplayName(const FString& InKind)
{
	const FForgeKindInfo* Info = Find(InKind);
	return Info != nullptr ? FString(Info->DisplayName) : InKind;
}

FString GetConfigDirName(const FString& InKind)
{
	const FForgeKindInfo* Info = Find(InKind);
	return Info != nullptr ? FString(Info->ConfigDirName) : InKind;
}

FString ClassifyAssetClass(const UClass* InAssetClass, const UClass* InNativeParent)
{
	if (InAssetClass == nullptr)
	{
		return FString();
	}

	const FForgeKindInfo* Best = nullptr;
	int32 BestDepth = -1;

	for (const FForgeKindInfo& Info : Table())
	{
		const UClass* KindClass = Info.GetAssetClass != nullptr ? Info.GetAssetClass() : nullptr;
		if (KindClass == nullptr || !InAssetClass->IsChildOf(KindClass))
		{
			continue;
		}

		if (Info.Accepts != nullptr && !Info.Accepts(InAssetClass, InNativeParent))
		{
			continue;
		}

		const int32 Depth = ClassDepth(KindClass);
		if (Depth > BestDepth)
		{
			Best = &Info;
			BestDepth = Depth;
		}
	}

	return Best != nullptr ? FString(Best->Kind) : FString();
}

FString ClassifyAsset(const UObject* InAsset)
{
	if (InAsset == nullptr)
	{
		return FString();
	}

	const UClass* NativeParent = nullptr;
	if (const UBlueprint* Blueprint = Cast<UBlueprint>(InAsset))
	{
		NativeParent = FindNativeClass(Blueprint->ParentClass);
	}

	return ClassifyAssetClass(InAsset->GetClass(), NativeParent);
}

FString ClassifyAssetData(const FAssetData& InAssetData)
{
	const UClass* AssetClass = InAssetData.GetClass();
	if (AssetClass == nullptr)
	{
		return FString();
	}

	const UClass* NativeParent = nullptr;
	if (AssetClass->IsChildOf(UBlueprint::StaticClass()))
	{
		// 资产注册表标签是导出文本形式：`/Script/CoreUObject.Class'/Script/Engine.Actor'`。
		FString TagValue;
		if (InAssetData.GetTagValue(TEXT("NativeParentClass"), TagValue) && !TagValue.IsEmpty())
		{
			const FSoftClassPath ParentPath(FPackageName::ExportTextPathToObjectPath(TagValue));
			NativeParent = ParentPath.ResolveClass();
		}
	}

	return ClassifyAssetClass(AssetClass, NativeParent);
}

FForgeDiffSupport GetPropertyDiffSupport(const FString& InKind)
{
	if (const FForgeKindInfo* Info = Find(InKind))
	{
		return Info->DiffSupport;
	}

	FForgeDiffSupport Result;
	Result.Reason = FString::Printf(
		TEXT("kind `%s` 未在 ForgeKindRegistry 登记，无法判断属性差异支持范围。"),
		*InKind);
	return Result;
}

} // namespace ForgeKindRegistry
