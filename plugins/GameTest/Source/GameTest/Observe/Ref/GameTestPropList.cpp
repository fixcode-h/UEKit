// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestPropList.h"

#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Ref/GameTestPropKind.h"
#include "Observe/Ref/GameTestPropPath.h"

#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace GameTestPropListImpl
{
	/** 一个对象最多补这么多条（UUserWidget 编辑器可见的属性有几十个，够用且不撑爆 12000 字预算）。 */
	constexpr int32 MaxReflected = 80;

	/** 常用的排前面：控件最常拿来判断和区分的几个。 */
	int32 Priority(const FString& InName)
	{
		static const TCHAR* const Common[] = {
			TEXT("Visibility"), TEXT("Text"), TEXT("CheckedState"), TEXT("bIsEnabled"), TEXT("ColorAndOpacity"), TEXT("RenderOpacity"),
			TEXT("Percent"), TEXT("Value"), TEXT("SelectedOption"), TEXT("ActiveWidgetIndex"), TEXT("ToolTipText"), TEXT("HintText"),
		};
		const int32 Count = static_cast<int32>(UE_ARRAY_COUNT(Common));
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (InName.Equals(Common[Index], ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		return Count;
	}

	/** 排序档：蓝图自己的变量 0、项目 C++ 1、引擎 / UMG 2（项目的东西排前面，引擎自带的跟在后面）。 */
	int32 OwnerTier(const FProperty* InProperty)
	{
		const UStruct* Owner = InProperty->GetOwnerStruct();
		if (Owner == nullptr)
		{
			return 2;
		}
		const FString Package = Owner->GetOutermost()->GetName();
		if (!Package.StartsWith(TEXT("/Script/")))
		{
			return 0;
		}
		static const TCHAR* const EnginePackages[] = {
			TEXT("/Script/CoreUObject"), TEXT("/Script/Engine"), TEXT("/Script/UMG"), TEXT("/Script/CommonUI"),
			TEXT("/Script/Slate"), TEXT("/Script/SlateCore"), TEXT("/Script/InputCore"), TEXT("/Script/AIModule"),
			TEXT("/Script/NavigationSystem"), TEXT("/Script/GameplayAbilities"), TEXT("/Script/EnhancedInput"),
		};
		for (const TCHAR* Engine : EnginePackages)
		{
			if (Package.Equals(Engine, ESearchCase::IgnoreCase))
			{
				return 2;
			}
		}
		return 1;
	}

	/** 只要编辑器或蓝图看得见的；委托、过时、只在编辑器里有的不要（不是控件此刻的状态）。 */
	bool Listable(const FProperty* InProperty)
	{
		if (InProperty == nullptr || !InProperty->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
		{
			return false;
		}
		if (InProperty->HasAnyPropertyFlags(CPF_Deprecated | CPF_EditorOnly))
		{
			return false;
		}
		return !InProperty->IsA<FMulticastDelegateProperty>() && !InProperty->IsA<FDelegateProperty>();
	}
}

void GameTestPropList::AddReflected(UObject* InObject, FGameTestFlowContext& InContext, TArray<FGameTestIdentProp>& InOutProps)
{
	using namespace GameTestPropListImpl;
	if (!IsValid(InObject))
	{
		return;
	}
	TSet<FString> Seen;
	for (const FGameTestIdentProp& Prop : InOutProps)
	{
		Seen.Add(Prop.Prop.ToLower());
	}
	TArray<TPair<int32, FGameTestIdentProp>> Added;
	for (TFieldIterator<FProperty> It(InObject->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		const FProperty* Property = *It;
		if (!Listable(Property))
		{
			continue;
		}
		// 蓝图变量的 GetName 带 GUID 后缀，路径用声明时的名字（和 Collect 一样）
		const FString Name = Property->GetAuthoredName();
		const FString Path = FGameTestPropPath::OfProp(Name);
		if (Seen.Contains(Path.ToLower()))
		{
			continue;
		}
		Seen.Add(Path.ToLower());
		FGameTestIdentProp Prop;
		Prop.Prop = Path;
		Prop.Name = Name;
		Prop.Group = TEXT("self");
		Prop.Read = GameTestPropRead::ReadText(InObject, Path, InContext);
		// 读不到、不能比较的（笔刷、布局插槽这类结构体）列出来也选不了，不占预算
		if (!Prop.Read.IsResolved() || !FGameTestPropKind::IsComparable(Prop.Read.Kind))
		{
			continue;
		}
		// 这里补的多是引擎自带的、对象引用这类，同类之间多半一样，不推荐当标识：给低分（换成 0–10 是 3）
		Prop.Score = 0.0;
		Added.Emplace(OwnerTier(Property), Prop);
	}
	Added.StableSort([](const TPair<int32, FGameTestIdentProp>& A, const TPair<int32, FGameTestIdentProp>& B)
	{
		if (A.Key != B.Key)
		{
			return A.Key < B.Key;
		}
		const int32 PA = Priority(A.Value.Name);
		const int32 PB = Priority(B.Value.Name);
		return PA != PB ? PA < PB : A.Value.Name < B.Value.Name;
	});
	for (int32 Index = 0; Index < Added.Num() && Index < MaxReflected; ++Index)
	{
		InOutProps.Add(Added[Index].Value);
	}
}

FString GameTestPropList::Rebase(const FString& InProp, const FString& InPart)
{
	if (InPart.IsEmpty() || !InProp.StartsWith(TEXT("@")))
	{
		return InProp;
	}
	FGameTestPropPath Path;
	FString Error;
	// 条目数据对象（@item…）不跟着部件走；写法不对的原样返回，交给校验报错
	if (!FGameTestPropPath::Parse(InProp, Path, Error) || Path.bItem)
	{
		return InProp;
	}
	// `@.X` → `@/部件.X`，`@:text` → `@/部件:text`，`@#X` → `@/部件#X`，`@/Txt:text` → `@/部件/Txt:text`
	return TEXT("@") + FGameTestPropPath::PartToPath(InPart) + InProp.Mid(1);
}
