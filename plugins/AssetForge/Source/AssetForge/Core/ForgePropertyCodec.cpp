// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ForgePropertyCodec.h"
#include "AssetForgeCompat.h"

#include "Core/ForgeJsonText.h"

#include "Dom/JsonValue.h"
#include "Engine/AssetUserData.h"
#include "UObject/Class.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "JsonObjectConverter.h"

namespace ForgePropertyCodec
{

namespace
{
	/**
	 * 结构体的「默认那份数据」。
	 *
	 * UClass 有 CDO 可以直接用；**UScriptStruct 没有 CDO**（`GetDefaultInstance` 也不存在），
	 * 只能现造一份——`FStructOnScope` 的构造会 `InitializeStruct`，成员各自拿到默认值。
	 * 造出来的那份挂在本对象上，所以这个对象必须活到用完为止，不能只接 `Get()` 的指针。
	 */
	struct FDefaultDataScope
	{
		explicit FDefaultDataScope(const UStruct* InStruct)
		{
			if (const UClass* Class = Cast<UClass>(InStruct))
			{
				Data = Class->GetDefaultObject();
			}
			else if (InStruct != nullptr)
			{
				Scope = MakeUnique<FStructOnScope>(InStruct);
				Data = Scope->GetStructMemory();
			}
		}

		const void* Get() const
		{
			return Data;
		}

	private:
		const void* Data = nullptr;
		TUniquePtr<FStructOnScope> Scope;
	};

	/** 把单个值包成对象再取规范串——比较两个 JSON 值的唯一可靠办法。 */
	FString ValueToKey(const TSharedPtr<FJsonValue>& InValue)
	{
		const TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetField(TEXT("v"), InValue);
		return ForgeJsonText::ToCanonicalString(Wrapper);
	}
}

bool IsCodecProperty(const FProperty* InProperty)
{
	if (InProperty == nullptr)
	{
		return false;
	}

	// 「编辑器里能配的」就是 CPF_Edit——这正是细节面板用来决定显示哪些字段的标志。
	if (!InProperty->HasAnyPropertyFlags(CPF_Edit))
	{
		return false;
	}
	// **AssetUserData 族的对象数组**（`AssetUserData`、`AssetUserDataEditorOnly`……）是引擎与
	// 插件挂在资产上的附加数据（动画修饰器、编辑器扩展……），不是作者意图的一部分；导入端
	// 只会把它们丢成 None——「导出 → 导入」因此不收敛（2026-09-15 实测：动画序列每次导入都
	// 写盘，差异就在它；SKEL/SK 的 `AssetUserDataEditorOnly` 同理）。判据按**元素类型**而不是
	// 属性名：挂这种数组的属性名不止一个，按名堵不住下一个。
	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(InProperty))
	{
		if (const FObjectPropertyBase* InnerObject = CastField<FObjectPropertyBase>(ArrayProperty->Inner))
		{
			if (InnerObject->PropertyClass != nullptr
				&& InnerObject->PropertyClass->IsChildOf(UAssetUserData::StaticClass()))
			{
				return false;
			}
		}
	}
	// 瞬态与仅编辑器内部状态不该进配置：它们不是作者意图的一部分。
	if (InProperty->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_TextExportTransient))
	{
		return false;
	}

	// 「单指针的 Instanced 对象属性」是这一类：`TObjectPtr<X>` 上带 `Instanced`，指向引擎/
	// 编辑器为这个对象内建并维护的内嵌子对象——UMG 的 `Slot` 与 `Navigation`、WorldSettings 的
	// `NavigationSystemConfig`、材质的 `EditorOnlyData` 都是它。
	//
	// 三条理由，每条单独都足够把它挡在配置之外：
	//   ① 导出会把它当普通对象递归展开：`NavigationSystemConfig` 会带出 31 个 agent 开关，
	//      `EditorOnlyData` 更把整个材质图塞进 props（表达式反向引用材质，展开还会咬回自己）；
	//   ② `ResetToDefaults` 用 `CopyCompleteValue` 复制 CDO 的**指针**——实例从此指向 CDO 的子对象，
	//      那是跨对象共享的脏状态，保存时还可能把引擎 CDO 拖进资产；
	//   ③ 它们的合法形态由引擎构造与子系统维护，配置里没有表达它们的正确写法。
	//
	// **数组形式的 Instanced 不在此列**（输入系统的修饰器 / 触发器数组就是这种）：
	// 那些是可配置内容，所以判据只看「属性自身是不是 Instanced 的对象指针」。
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(InProperty))
	{
		if (ObjectProperty->HasAnyPropertyFlags(CPF_InstancedReference))
		{
			return false;
		}
	}

	// 到此为止就是「细节面板可见即可配」：不额外按标签排除。曾经这里挡掉过带
	// `BlueprintProtected` 的属性——它标记的是"别在事件图表里依赖/改它"，不是"配置不能写它的值"；
	// 而配置恰恰是这类属性该走的那条路（值相对默认值改过就导出，导入时按值写回）。

	return true;
}

FProperty* FindCodecProperty(const UStruct* InStruct, const FString& InName)
{
	if (InStruct == nullptr || InName.IsEmpty())
	{
		return nullptr;
	}

	// FName 查找本身大小写不敏感，所以 `visibility` 与 `Visibility` 等价。
	if (FProperty* Exact = FindFProperty<FProperty>(InStruct, *InName))
	{
		return Exact;
	}

	if (!InName.StartsWith(TEXT("b")))
	{
		const FString WithPrefix = FString::Printf(TEXT("b%s"), *InName);
		if (FProperty* Prefixed = FindFProperty<FProperty>(InStruct, *WithPrefix))
		{
			return Prefixed;
		}

		// 也接受相反方向：配置写 `bIsEnabled`、属性的实际名字是 `IsEnabled`。
		if (InName.Len() > 1 && FChar::IsUpper(InName[1]))
		{
			const FString WithoutPrefix = InName.RightChop(1);
			if (FProperty* Stripped = FindFProperty<FProperty>(InStruct, *WithoutPrefix))
			{
				return Stripped;
			}
		}
	}

	return nullptr;
}

void ValidateProperties(
	const UStruct* InStruct,
	const TSharedPtr<FJsonObject>& InJson,
	TArray<FForgeDiagnostic>& OutDiagnostics,
	const FString& InFieldPath,
	bool bInStructRecursion)
{
	if (InStruct == nullptr || !InJson.IsValid())
	{
		return;
	}

	for (const auto& Pair : InJson->Values)
	{
		const FString KeyString(ASSETFORGE_JSON_KEY(Pair.Key));
		const FString PropertyPath = FString::Printf(TEXT("%s.%s"), *InFieldPath, *KeyString);

		FProperty* Property = FindCodecProperty(InStruct, KeyString);
		if (Property == nullptr)
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				PropertyPath,
				FString::Printf(TEXT("类型 %s 上没有名为 `%s` 的属性"), *InStruct->GetName(), *KeyString));
			continue;
		}

		if (!IsCodecProperty(Property))
		{
			// 结构体内部不要求可配——结构体是整体导出的，里面天然带引擎的无 `Edit` 字段
			// （`FLightmassWorldInfoSettings::EmissiveBoost` 就是这样）；顶层字段仍然要求可配，
			// 否则「导出的东西导入不了」。不报错也不递归：不可配的字段整支都不归配置管。
			if (!bInStructRecursion)
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Error,
					PropertyPath,
					FString::Printf(TEXT("属性 `%s` 不可通过配置设置"), *KeyString));
			}
			continue;
		}

		// 结构体递归：只写部分字段是允许的（未写的保持原值），但写了不存在的字段名一定是笔误——
		// 关掉严格模式后，这里是发现它的唯一防线。
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			const TSharedPtr<FJsonObject>* NestedObject = nullptr;
			if (Pair.Value.IsValid() && Pair.Value->TryGetObject(NestedObject) && NestedObject->IsValid())
			{
				ValidateProperties(
					StructProperty->Struct,
					*NestedObject,
					OutDiagnostics,
					PropertyPath,
					/*bInStructRecursion=*/true);
			}
		}
	}
}

void PruneDefaults(const UStruct* InStruct, const TSharedPtr<FJsonObject>& InOutJson)
{
	const FDefaultDataScope Default(InStruct);
	const void* DefaultData = Default.Get();
	if (DefaultData == nullptr || !InOutJson.IsValid())
	{
		return;
	}

	// **不递归进结构体**：导出方向对结构体是整体导出的，不做结构体内部的 delta——
	// 递归删成员反而会造出一份与导出形态对不上的配置。
	TArray<FString> FieldsToRemove;
	for (const auto& Pair : InOutJson->Values)
	{
		const FString KeyString(ASSETFORGE_JSON_KEY(Pair.Key));

		FProperty* Property = FindCodecProperty(InStruct, KeyString);
		if (Property == nullptr)
		{
			continue;
		}

		const TSharedPtr<FJsonValue> DefaultValue = FJsonObjectConverter::UPropertyToJsonValue(
			Property,
			Property->ContainerPtrToValuePtr<const void>(DefaultData));

		if (DefaultValue.IsValid() && ValueToKey(DefaultValue) == ValueToKey(Pair.Value))
		{
			FieldsToRemove.Add(KeyString);
		}
	}

	for (const FString& Key : FieldsToRemove)
	{
		InOutJson->RemoveField(Key);
	}
}

void ResetStructToDefaults(const UStruct* InStruct, void* InData)
{
	const FDefaultDataScope Default(InStruct);
	const void* DefaultData = Default.Get();
	if (DefaultData == nullptr || InData == nullptr)
	{
		return;
	}

	for (TFieldIterator<FProperty> It(InStruct); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsCodecProperty(Property))
		{
			continue;
		}

		Property->CopyCompleteValue(
			Property->ContainerPtrToValuePtr<void>(InData),
			Property->ContainerPtrToValuePtr<const void>(DefaultData));
	}
}

TSharedPtr<FJsonValue> ShortenFloatValue(float InValue)
{
	// 9 位有效数字是 float 的保证上限；从短到长试，取第一个能精确还原的。
	for (int32 Digits = 1; Digits <= 9; ++Digits)
	{
		const FString Text = FString::Printf(TEXT("%.*g"), Digits, InValue);
		if (static_cast<float>(FCString::Atof(*Text)) == InValue)
		{
			return MakeShared<FJsonValueNumber>(FCString::Atod(*Text));
		}
	}
	return MakeShared<FJsonValueNumber>(InValue);
}

/**
 * 递归把 float 字段的 JSON 值改写成「转回 float 后仍相同」的最短十进制。
 *
 * float 在 JSON 里只能是 double，0.6f 于是导成 0.6000000238418579：配置里全是噪声，
 * 更麻烦的是它与手写的 0.6 **判不相等**——每次导入都报同一个差异、资产永远不收敛。
 * 反射导出走这里；直接手写 JSON 的处理器（如 Niagara 的可写参数）走 `ShortenFloatValue`。
 */
TSharedPtr<FJsonValue> ShortenFloatFields(const FProperty* InProperty, const TSharedPtr<FJsonValue>& InValue)
{
	if (InProperty == nullptr || !InValue.IsValid())
	{
		return InValue;
	}

	if (InProperty->IsA<FFloatProperty>() && InValue->Type == EJson::Number)
	{
		return ShortenFloatValue(static_cast<float>(InValue->AsNumber()));
	}

	if (const FStructProperty* StructProperty = CastField<FStructProperty>(InProperty))
	{
		const TSharedPtr<FJsonObject> Object = InValue->Type == EJson::Object ? InValue->AsObject() : nullptr;
		if (Object.IsValid())
		{
			// 就地改结构体的成员字段（Object 本身是刚转出来的，改它安全）。
			for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
			{
				const FProperty* ChildProperty = *It;
				const FString Key = ChildProperty->GetName();
				if (Object->HasField(Key))
				{
					Object->SetField(Key, ShortenFloatFields(ChildProperty, Object->TryGetField(Key)));
				}
			}
		}

		return InValue;
	}

	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(InProperty))
	{
		if (InValue->Type != EJson::Array)
		{
			return InValue;
		}

		const TArray<TSharedPtr<FJsonValue>>& Items = InValue->AsArray();
		TArray<TSharedPtr<FJsonValue>> Shortened;
		Shortened.Reserve(Items.Num());
		for (const TSharedPtr<FJsonValue>& Item : Items)
		{
			Shortened.Add(ShortenFloatFields(ArrayProperty->Inner, Item));
		}

		return MakeShared<FJsonValueArray>(Shortened);
	}

	return InValue;
}

void ExportStructDelta(
	const UStruct* InStruct,
	const void* InData,
	const void* InDefaultData,
	TSharedPtr<FJsonObject>& OutJson,
	TArray<FForgeDiagnostic>& OutDiagnostics,
	const FString& InFieldPath,
	const TArray<FName>* InExcludedPropertyNames)
{
	if (InStruct == nullptr || InData == nullptr)
	{
		return;
	}

	if (!OutJson.IsValid())
	{
		OutJson = MakeShared<FJsonObject>();
	}

	if (InDefaultData == nullptr)
	{
		OutDiagnostics.Emplace(
			EForgeSeverity::Warning,
			InFieldPath,
			FString::Printf(TEXT("类型 %s 没有默认实例，无法做 delta 导出，本次写全量"), *InStruct->GetName()));
	}

	for (TFieldIterator<FProperty> It(InStruct); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsCodecProperty(Property))
		{
			continue;
		}

		// kind 级排除（如 UFont::CompositeFont）：不进配置，也不产生"无法转换"的噪音。
		if (InExcludedPropertyNames != nullptr && InExcludedPropertyNames->Contains(Property->GetFName()))
		{
			continue;
		}

		const void* ValuePtr = Property->ContainerPtrToValuePtr<const void>(InData);

		if (InDefaultData != nullptr)
		{
			const void* DefaultPtr = Property->ContainerPtrToValuePtr<const void>(InDefaultData);
			if (Property->Identical(ValuePtr, DefaultPtr, /*PortFlags=*/0))
			{
				continue;
			}
		}

		const TSharedPtr<FJsonValue> JsonValue = FJsonObjectConverter::UPropertyToJsonValue(
			Property,
			ValuePtr,
			/*CheckFlags=*/0,
			/*SkipFlags=*/0,
			/*ExportCb=*/nullptr,
			/*OuterProperty=*/nullptr,
			EJsonObjectConversionFlags::None);

		if (!JsonValue.IsValid())
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Warning,
				FString::Printf(TEXT("%s.%s"), *InFieldPath, *Property->GetName()),
				TEXT("该属性的值无法转换为 JSON，已跳过"));
			continue;
		}

		OutJson->SetField(Property->GetName(), ShortenFloatFields(Property, JsonValue));
	}

	// 一个属性都没写出来时把空对象还原成 nullptr。
	// 否则「导出出来的空 props」与「配置里根本没写 props」会看起来不一样，
	// 增量比较就会把每个节点都误报成「已更新」。
	if (OutJson.IsValid() && OutJson->Values.Num() == 0)
	{
		OutJson.Reset();
	}
}

void ExportForcedProperties(
	const UObject* InObject,
	const TArray<FName>& InPropertyNames,
	TSharedPtr<FJsonObject>& InOutJson,
	TArray<FForgeDiagnostic>& OutDiagnostics,
	const FString& InFieldPath)
{
	if (InObject == nullptr || InPropertyNames.Num() == 0)
	{
		return;
	}

	if (!InOutJson.IsValid())
	{
		InOutJson = MakeShared<FJsonObject>();
	}

	const UStruct* Struct = InObject->GetClass();
	for (const FName& PropertyName : InPropertyNames)
	{
		FProperty* Property = FindCodecProperty(Struct, PropertyName.ToString());
		if (Property == nullptr)
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Warning,
				FString::Printf(TEXT("%s.%s"), *InFieldPath, *PropertyName.ToString()),
				TEXT("强制导出的属性没找到（引擎版本改名了？），已跳过"));
			continue;
		}

		const void* ValuePtr = Property->ContainerPtrToValuePtr<const void>(InObject);
		const TSharedPtr<FJsonValue> JsonValue = FJsonObjectConverter::UPropertyToJsonValue(
			Property,
			ValuePtr,
			/*CheckFlags=*/0,
			/*SkipFlags=*/0,
			/*ExportCb=*/nullptr,
			/*OuterProperty=*/nullptr,
			EJsonObjectConversionFlags::None);

		if (!JsonValue.IsValid())
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Warning,
				FString::Printf(TEXT("%s.%s"), *InFieldPath, *Property->GetName()),
				TEXT("该属性的值无法转换为 JSON，已跳过"));
			continue;
		}

		InOutJson->SetField(Property->GetName(), ShortenFloatFields(Property, JsonValue));
	}
}

/**
 * 写「元素带 `Instanced` 的对象数组」——GE 的组件（`GEComponents`）、输入映射的修饰器
 * 这类。它们**不能交给 `FJsonObjectConverter`**：那个转换器按**属性声明的类**建对象，
 * 而声明往往是抽象基类（`UGameplayEffectComponent`），建出来的 Outer 还是包而不是宿主
 * ——引擎的 `ClassWithin` 检查会当场 `ensure` 失败（2026-09-19 实测：GE 组件被建在
 * `/Script/CoreUObject.Package` 下）。这里按元素里的 `_ClassName` 自己建，Outer 指向宿主。
 *
 * **按索引复用同类的旧对象**：每次导入都换一批新对象，资产就永远处于"刚被改过"的状态，
 * 导出→导入的回环不再是空计划。
 */
void ImportInstancedObjectArray(
	UObject* InOwner,
	const FArrayProperty& InArrayProperty,
	const FObjectPropertyBase& InInnerObject,
	void* InArrayPtr,
	const TArray<TSharedPtr<FJsonValue>>& InJsonArray,
	TArray<FForgeDiagnostic>& OutDiagnostics,
	const FString& InFieldPath)
{
	if (InOwner == nullptr)
	{
		OutDiagnostics.Emplace(
			EForgeSeverity::Error,
			InFieldPath,
			TEXT("Instanced 对象数组要以宿主对象做 Outer，当前上下文没有（DataTable 行这种纯结构体写不了它）"));
		return;
	}

	FScriptArrayHelper ArrayHelper(&InArrayProperty, InArrayPtr);

	// 先把旧元素记下来：同索引、同类的复用，避免每次导入都重建。
	TArray<UObject*> Reusable;
	Reusable.Reserve(ArrayHelper.Num());
	for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
	{
		Reusable.Add(InInnerObject.GetObjectPropertyValue(ArrayHelper.GetRawPtr(Index)));
	}

	ArrayHelper.EmptyValues();

	for (int32 Index = 0; Index < InJsonArray.Num(); ++Index)
	{
		const FString ElementPath = FString::Printf(TEXT("%s[%d]"), *InFieldPath, Index);

		const TSharedPtr<FJsonObject>* ElementJson = nullptr;
		if (!InJsonArray[Index].IsValid()
			|| !InJsonArray[Index]->TryGetObject(ElementJson)
			|| ElementJson == nullptr)
		{
			OutDiagnostics.Emplace(EForgeSeverity::Error, ElementPath, TEXT("元素必须是 JSON 对象"));
			continue;
		}

		FString ClassString;
		if (!(*ElementJson)->TryGetStringField(TEXT("_ClassName"), ClassString) || ClassString.IsEmpty())
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				ElementPath,
				TEXT("缺少 `_ClassName`——Instanced 数组的每个元素都要写清它是哪个类的实例"));
			continue;
		}

		UClass* ElementClass = FindObject<UClass>(nullptr, *ClassString);
		if (ElementClass == nullptr)
		{
			ElementClass = LoadObject<UClass>(nullptr, *ClassString);
		}

		if (ElementClass == nullptr)
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error, ElementPath, FString::Printf(TEXT("找不到类 `%s`"), *ClassString));
			continue;
		}

		if (!ElementClass->IsChildOf(InInnerObject.PropertyClass))
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				ElementPath,
				FString::Printf(
					TEXT("`%s` 不是 `%s` 的子类"),
					*ClassString, *InInnerObject.PropertyClass->GetName()));
			continue;
		}

		UObject* Element = (Index < Reusable.Num()) ? Reusable[Index] : nullptr;
		if (Element == nullptr || Element->GetClass() != ElementClass)
		{
			Element = NewObject<UObject>(
				InOwner, ElementClass, NAME_None, RF_Transactional | RF_PropagateToSubObjects);
		}

		if (Element == nullptr)
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				ElementPath,
				FString::Printf(TEXT("创建 `%s` 的实例失败"), *ClassString));
			continue;
		}

		const int32 NewIndex = ArrayHelper.AddValue();
		InInnerObject.SetObjectPropertyValue(ArrayHelper.GetRawPtr(NewIndex), Element);

		// 元素自己的属性按同一个宿主链递归写：`InOwner` 继续往下传，深层还有 Instanced 数组也成立。
		ImportStructProperties(ElementClass, Element, *ElementJson, OutDiagnostics, ElementPath, Element);
	}
}

void ImportStructProperties(
	const UStruct* InStruct,
	void* InData,
	const TSharedPtr<FJsonObject>& InJson,
	TArray<FForgeDiagnostic>& OutDiagnostics,
	const FString& InFieldPath,
	UObject* InOwner)
{
	if (InStruct == nullptr || InData == nullptr || !InJson.IsValid())
	{
		return;
	}

	for (const auto& Pair : InJson->Values)
	{
		const FString KeyString(ASSETFORGE_JSON_KEY(Pair.Key));

		// `_ClassName` 是 Instanced 对象数组**元素**的元数据（见 ImportInstancedObjectArray），
		// 不是属性名——它在元素那一层就被消费掉了，落到这里说明写错了位置。
		if (KeyString == TEXT("_ClassName"))
		{
			continue;
		}

		const FString PropertyPath = FString::Printf(TEXT("%s.%s"), *InFieldPath, *KeyString);

		FProperty* Property = FindCodecProperty(InStruct, KeyString);
		if (Property == nullptr)
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				PropertyPath,
				FString::Printf(TEXT("类型 %s 上没有名为 `%s` 的属性"), *InStruct->GetName(), *KeyString));
			continue;
		}

		if (!IsCodecProperty(Property))
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				PropertyPath,
				FString::Printf(TEXT("属性 `%s` 不可通过配置设置"), *KeyString));
			continue;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(InData);

		// Instanced 对象数组走自己的路（见上面那个函数的说明）——交给 FJsonObjectConverter
		// 会以错误的 Outer 建对象，撞引擎的 ClassWithin 检查。
		if (const FArrayProperty* InstancedArray = CastField<FArrayProperty>(Property))
		{
			if (const FObjectPropertyBase* InnerObject = CastField<FObjectPropertyBase>(InstancedArray->Inner))
			{
				// `UPROPERTY(Instanced)` 落在数组上时，标志的位置随引擎版本变过：内层对象属性带、
				// 或数组自身带。两处都认——`CPF_InstancedReference` 只有 Instanced 声明才会有，
				// 不会误纳普通对象数组。
				const bool bInstanced =
					InnerObject->HasAnyPropertyFlags(CPF_InstancedReference)
					|| InstancedArray->HasAnyPropertyFlags(CPF_InstancedReference);
				if (bInstanced
					&& Pair.Value.IsValid()
					&& Pair.Value->Type == EJson::Array)
				{
					ImportInstancedObjectArray(
						InOwner, *InstancedArray, *InnerObject, ValuePtr,
						Pair.Value->AsArray(), OutDiagnostics, PropertyPath);
					continue;
				}
			}
		}

		FText FailReason;
		// bStrictMode = false：严格模式要求结构体字段写全（FMargin 少一个 Left 就失败），
		// 而「只写想改的字段」才是配置该有的用法。字段名笔误由 ValidateProperties 在
		// 计划阶段拦住，所以这里的放宽不会让错误溜过去。
		if (!FJsonObjectConverter::JsonValueToUProperty(
			Pair.Value,
			Property,
			ValuePtr,
			/*CheckFlags=*/0,
			/*SkipFlags=*/0,
			/*bStrictMode=*/false,
			&FailReason))
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				PropertyPath,
				FString::Printf(
					TEXT("写入属性 `%s` 失败：%s"),
					*KeyString,
					FailReason.IsEmpty() ? TEXT("值类型与属性不匹配") : *FailReason.ToString()));
		}
	}
}

// ---------------------------------------------------------------- UObject 包装

void ResetToDefaults(UObject* InObject)
{
	if (InObject != nullptr)
	{
		ResetStructToDefaults(InObject->GetClass(), InObject);
	}
}

void ExportDeltaProperties(
	const UObject* InObject,
	TSharedPtr<FJsonObject>& OutJson,
	TArray<FForgeDiagnostic>& OutDiagnostics,
	const FString& InFieldPath,
	const TArray<FName>* InExcludedPropertyNames)
{
	if (InObject != nullptr)
	{
		ExportStructDelta(
			InObject->GetClass(),
			InObject,
			InObject->GetClass()->GetDefaultObject(),
			OutJson,
			OutDiagnostics,
			InFieldPath,
			InExcludedPropertyNames);
	}
}

void ImportProperties(
	UObject* InObject,
	const TSharedPtr<FJsonObject>& InJson,
	TArray<FForgeDiagnostic>& OutDiagnostics,
	const FString& InFieldPath)
{
	if (InObject != nullptr)
	{
		ImportStructProperties(InObject->GetClass(), InObject, InJson, OutDiagnostics, InFieldPath, InObject);
	}
}

} // namespace ForgePropertyCodec
