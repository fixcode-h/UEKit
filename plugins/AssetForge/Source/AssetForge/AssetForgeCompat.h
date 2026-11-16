// 引擎版本差异只写在这里（UEKit 约定）：业务代码只用这里定义的宏 / 类型，不直接判断 ENGINE_*_VERSION。
// AssetForge 以 UE 5.4 为基准开发，同时兼容 5.8：默认写 5.4 的写法，只有 5.8 才有的接口在这里用版本判断包住。
// 新接口只在实测编过的 5.8 上用（UE_VERSION_NEWER_THAN(5, 7, 99)），中间版本走 5.4 的写法，不去猜哪一版加的。
// 状态树整块只在 5.8 上编，开关 ASSETFORGE_WITH_STATETREE 在 AssetForge.Build.cs 里按 Target.Version 定义。
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/EngineVersionComparison.h"

#include <type_traits>

struct FExpressionInput;

#if UE_VERSION_OLDER_THAN(5, 4, 0)
	#error "AssetForge 需要 UE 5.4 或更新的版本"
#endif

#ifndef ASSETFORGE_WITH_STATETREE
	#define ASSETFORGE_WITH_STATETREE 0
#endif

/**
 * FJsonObject::Values 的键类型：5.4 是 FString，5.8 换成了 UE::FSharedString（不能和 FString 互相隐式转换）。
 * 直接从 Values 推出来，不按版本号猜；查找时用 FForgeJsonKey(Key) 构造键。
 */
using FForgeJsonKey = std::decay_t<decltype((*DeclVal<FJsonObject&>().Values.begin()).Key)>;

/** 遍历 Values 时把键转成 FString：两种键的 operator* 都给 const TCHAR*。遍历写 `for (const auto& Pair : X->Values)` */
#define ASSETFORGE_JSON_KEY(Key) FString(*(Key))

/** UWorldPartitionSettings 的「新建地图默认开不开世界分区」：5.4 没有这组设置，新建空白关卡不开世界分区 */
#define ASSETFORGE_HAS_NEW_MAP_WORLD_PARTITION_SETTINGS UE_VERSION_NEWER_THAN(5, 7, 99)

// ---- 下面这些只在 5.8 有的编辑器接口，写成模板：名字到调用处才解析，这个头不用 include 各领域的头 ----

/** UMaterialExpression 的输入个数：5.8 有 const 的 CountInputs；5.4 没有，挨个 GetInput 到空为止（各表达式越界都返回空） */
template <typename TExpression>
int32 ForgeCountInputs(const TExpression* InExpression)
{
#if UE_VERSION_NEWER_THAN(5, 7, 99)
	return InExpression->CountInputs();
#else
	TExpression* Mutable = const_cast<TExpression*>(InExpression);
	int32 Count = 0;
	while (Mutable->GetInput(Count) != nullptr)
	{
		++Count;
	}
	return Count;
#endif
}

/** UMaterialExpression::GetInput：5.4 只有非 const 版，只读访问时去掉 const（不改表达式） */
template <typename TExpression>
auto ForgeGetInput(const TExpression* InExpression, int32 InIndex)
{
#if UE_VERSION_NEWER_THAN(5, 7, 99)
	return InExpression->GetInput(InIndex);
#else
	return static_cast<const FExpressionInput*>(const_cast<TExpression*>(InExpression)->GetInput(InIndex));
#endif
}

/** UMaterialEditingLibrary::RecompileMaterial 在 5.8 返回编译错误；5.4 返回 void，错误要等编译完从材质资源上取 */
#define ASSETFORGE_RECOMPILE_RETURNS_ERRORS UE_VERSION_NEWER_THAN(5, 7, 99)

/** UFont 的合成字体：5.8 走 Get(Mutable)InternalCompositeFont；5.4 是公开成员 CompositeFont */
template <typename TFont>
auto& ForgeCompositeFont(TFont* InFont)
{
#if UE_VERSION_NEWER_THAN(5, 7, 99)
	if constexpr (std::is_const_v<TFont>)
	{
		return InFont->GetInternalCompositeFont();
	}
	else
	{
		return InFont->GetMutableInternalCompositeFont();
	}
#else
	return InFont->CompositeFont;
#endif
}

/**
 * 控件蓝图的「变量名 → GUID」表（WidgetVariableNameToGuidMap、OnVariableAdded / OnVariableRemoved）：5.8 才有，
 * 编译时会用 ensure 核对；5.4 没有这张表也没有这项核对，增删控件不用登记
 */
#define ASSETFORGE_HAS_WIDGET_VARIABLE_GUIDS UE_VERSION_NEWER_THAN(5, 7, 99)

template <typename TWidgetBlueprint>
void ForgeWidgetVariableAdded(TWidgetBlueprint& InBlueprint, FName InName)
{
#if ASSETFORGE_HAS_WIDGET_VARIABLE_GUIDS
	InBlueprint.OnVariableAdded(InName);
#endif
}

template <typename TWidgetBlueprint>
void ForgeWidgetVariableRemoved(TWidgetBlueprint& InBlueprint, FName InName)
{
#if ASSETFORGE_HAS_WIDGET_VARIABLE_GUIDS
	InBlueprint.OnVariableRemoved(InName);
#endif
}

/**
 * UPanelWidget::GetSlotClass：5.8 公开，5.4 是 protected。用派生类取成员指针再在基类对象上调用，
 * 不改引擎、照常走虚函数（各面板控件返回自己的槽类）
 */
template <typename TPanel>
struct TForgeSlotClassAccess : TPanel
{
	static UClass* Get(const TPanel* InPanel)
	{
#if UE_VERSION_NEWER_THAN(5, 7, 99)
		return InPanel->GetSlotClass();
#else
		return (InPanel->*(&TForgeSlotClassAccess::GetSlotClass))();
#endif
	}
};

template <typename TPanel>
UClass* ForgeSlotClass(const TPanel* InPanel)
{
	return TForgeSlotClassAccess<TPanel>::Get(InPanel);
}

/** FValidateAssetsResults::ValidatorMessages（跨资产的校验消息）：5.8 才有；5.4 只有逐资产的错误和警告 */
#define ASSETFORGE_HAS_VALIDATOR_MESSAGES UE_VERSION_NEWER_THAN(5, 7, 99)
