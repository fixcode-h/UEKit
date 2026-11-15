// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/ForgePropertyCodec.h"
#include "Core/ForgePropertyProbe.h"

#include "GameFramework/WorldSettings.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#if WITH_AUTOMATION_WORKER

/**
 * 属性通道的守门用例：哪些属性进配置、哪些不进。
 *
 * 这里钉的是一条容易再次出错的边界——**单指针的 Instanced 内嵌对象**（UMG 的 `Slot`、
 * WorldSettings 的 `NavigationSystemConfig`、材质的 `EditorOnlyData` 这一族）必须留在配置之外：
 * 它们由引擎构造与子系统维护，导出会让配置充满引擎噪音，`ResetToDefaults` 还会把 CDO 的
 * 指针复制进资产（跨对象共享）。
 *
 * 而**数组形式的 Instanced**（输入修饰器 / 触发器）是正常可配内容，不能被一起误伤——
 * 判据只看「属性自身是不是 Instanced 的对象指针」，这条用例就是它的反例守卫。
 *
 * 走反射查属性：不加载资产、不建包。
 */
TEST_CLASS_WITH_FLAGS(ForgePropertyCodecTest, "AssetForge.PropertyCodec",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	static FProperty* FindPropertyOnClassPath(const FString& InClassPath, const FString& InPropertyName)
	{
		UClass* Class = LoadClass<UObject>(nullptr, *InClassPath);
		if (Class == nullptr)
		{
			return nullptr;
		}
		return FindFProperty<FProperty>(Class, *InPropertyName);
	}

	TEST_METHOD(单指针_Instanced_的内嵌对象不进配置)
	{
		// WorldSettings 的导航配置：引擎在构造世界时内建，导入时绝不能拿 CDO 的指针去覆盖。
		const FProperty* NavigationSystemConfig =
			FindFProperty<FProperty>(AWorldSettings::StaticClass(), TEXT("NavigationSystemConfig"));
		ASSERT_THAT(IsNotNull(NavigationSystemConfig, TEXT("AWorldSettings 上应有 NavigationSystemConfig")));
		ASSERT_THAT(IsFalse(ForgePropertyCodec::IsCodecProperty(NavigationSystemConfig),
			TEXT("单指针 Instanced 的内嵌对象必须被排除")));

		// UMG 的 Slot 同理（它走节点自己的 slot 字段，不走 props）。
		const FProperty* Slot = FindPropertyOnClassPath(TEXT("/Script/UMG.Widget"), TEXT("Slot"));
		ASSERT_THAT(IsNotNull(Slot, TEXT("UWidget 上应有 Slot")));
		ASSERT_THAT(IsFalse(ForgePropertyCodec::IsCodecProperty(Slot),
			TEXT("Slot 必须被排除")));
	}

	TEST_METHOD(数组形式的_Instanced_仍然可配)
	{
		// 输入资产的修饰器 / 触发器就是 Instanced 数组——它们是正常的可配置内容。
		const FProperty* Modifiers = FindPropertyOnClassPath(TEXT("/Script/EnhancedInput.InputAction"), TEXT("Modifiers"));
		ASSERT_THAT(IsNotNull(Modifiers, TEXT("UInputAction 上应有 Modifiers")));
		ASSERT_THAT(IsTrue(ForgePropertyCodec::IsCodecProperty(Modifiers),
			TEXT("Instanced 数组不能被误伤")));
	}

	TEST_METHOD(AssetUserData_族的数组被按元素类型排除)
	{
		// 附加用户数据（引擎/插件挂在资产上的东西）实测会让导入永远不收敛——动画序列的
		// `AssetUserData`、Skeleton/SkeletalMesh 的 `AssetUserDataEditorOnly` 都是它。
		// 判据按**元素类型**（UAssetUserData 派生）而不是属性名：挂这种数组的属性名不止一个，
		// 按名堵不住下一个。它是「Instanced 对象数组」里唯一被排除的族——输入修饰器那类
		// 正常内容必须放行（上一条用例就是防误伤的）。
		const FProperty* SkeletalMeshUserData =
			FindPropertyOnClassPath(TEXT("/Script/Engine.SkeletalMesh"), TEXT("AssetUserData"));
		ASSERT_THAT(IsNotNull(SkeletalMeshUserData, TEXT("USkeletalMesh 上应有 AssetUserData")));
		ASSERT_THAT(IsFalse(ForgePropertyCodec::IsCodecProperty(SkeletalMeshUserData),
			TEXT("AssetUserData 必须被排除（实测不收敛）")));

		const FProperty* SkeletonEditorOnlyUserData =
			FindPropertyOnClassPath(TEXT("/Script/Engine.Skeleton"), TEXT("AssetUserDataEditorOnly"));
		ASSERT_THAT(IsNotNull(SkeletonEditorOnlyUserData, TEXT("USkeleton 上应有 AssetUserDataEditorOnly")));
		ASSERT_THAT(IsFalse(ForgePropertyCodec::IsCodecProperty(SkeletonEditorOnlyUserData),
			TEXT("AssetUserDataEditorOnly 同样要排（按元素类型才盖得住）")));
	}

	TEST_METHOD(只有带_Edit_标志的属性进配置)
	{
		const FProperty* KillZ = FindFProperty<FProperty>(AWorldSettings::StaticClass(), TEXT("KillZ"));
		ASSERT_THAT(IsNotNull(KillZ, TEXT("AWorldSettings 上应有 KillZ")));
		ASSERT_THAT(IsTrue(ForgePropertyCodec::IsCodecProperty(KillZ),
			TEXT("编辑器里能配的属性（CPF_Edit）进配置")));

		// ActorLabel 是 `UPROPERTY()`（没有 Edit 标志）：它不由细节面板编辑，也就不进配置。
		const FProperty* ActorLabel = FindPropertyOnClassPath(TEXT("/Script/Engine.Actor"), TEXT("ActorLabel"));
		ASSERT_THAT(IsNotNull(ActorLabel, TEXT("AActor 上应有 ActorLabel")));
		ASSERT_THAT(IsFalse(ForgePropertyCodec::IsCodecProperty(ActorLabel),
			TEXT("没有 Edit 标志的属性不进配置")));
	}
	TEST_METHOD(Test_BlueprintProtectedPropertiesAreConfigurable)
	{
		// 这类属性的标记说的是"别在事件图表里依赖/改它"，不是"配置不能写它的值"——
		// 按 spec「细节面板可见即可配」它们必须可配（这条用例钉住那次删除的排除规则）。
		const FProperty* Protected =
			FindFProperty<FProperty>(UForgePropertyProbe::StaticClass(), TEXT("ProtectedValue"));
		ASSERT_THAT(IsNotNull(Protected, TEXT("探针类上应有 ProtectedValue")));
		ASSERT_THAT(IsTrue(ForgePropertyCodec::IsCodecProperty(Protected),
			TEXT("带 BlueprintProtected 的可见属性应可配")));
	}

	TEST_METHOD(Test_UneditableStructInnerFieldsDoNotBlockValidation)
	{
		// 结构体是整体导出的：引擎结构体里天然带无 `Edit` 的内部/废弃字段
		// （`FLightmassWorldInfoSettings::EmissiveBoost` 就是——它已被引擎弃用，声明只剩
		// 一个裸 `UPROPERTY()`）。校验不该因此拒掉配置，否则「导出的东西导入不了」——
		// TestMap 的导入就是这样失败的；这条用例是那次修复的回归守卫。
		// 用真实的两级形态复现：`AWorldSettings.LightmassSettings`（顶层可配）→ 结构体内部。
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Lightmass = MakeShared<FJsonObject>();
		Lightmass->SetNumberField(TEXT("EmissiveBoost"), 1.0);
		Lightmass->SetNumberField(TEXT("DiffuseBoost"), 1.0);
		Json->SetObjectField(TEXT("LightmassSettings"), Lightmass);

		TArray<FForgeDiagnostic> Diagnostics;
		ForgePropertyCodec::ValidateProperties(
			AWorldSettings::StaticClass(),
			Json,
			Diagnostics,
			TEXT("test"));

		ASSERT_THAT(IsTrue(Diagnostics.Num() == 0,
			TEXT("结构体内的无 Edit 字段不该产生诊断")));
	}
};

#endif // WITH_AUTOMATION_WORKER
