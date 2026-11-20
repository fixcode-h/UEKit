// Copyright Epic Games, Inc. All Rights Reserved.

// gt.props（docs/game-test-ref.md 7.3）：一个对象的属性树，挑标识和写判断共用。
// 游戏开着时给值、同类里有几种值、推荐的标识组合；只给 class 时按类列（不开游戏也行：CDO 的反射 + WBP 的设计器树）。

#include "Integration/Ref/GameTestRpcRef.h"

#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Compare/GameTestCompareRegistry.h"
#include "Observe/Sources/GameTestReflectCall.h"
#include "Observe/Ref/GameTestIdentSuggest.h"
#include "Observe/Ref/GameTestPropKind.h"
#include "Observe/Ref/GameTestPropList.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRefFor.h"
#include "Observe/Ref/GameTestRefResolve.h"
#include "Observe/Ref/GameTestState.h"
#include "Observe/Sources/GameTestObjectRoots.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/SlateWrapperTypes.h"
#include "Components/Widget.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

namespace GameTestRpcPropsImpl
{
	constexpr int32 DefaultLimit = 120;
	constexpr int32 MaxLimit = 400;
	constexpr int32 MaxChars = 12000;
	constexpr int32 MaxValueChars = 200;
	/** 算「同类里几种值」最多看这么多个实例（每个都要逐项读一遍）。 */
	constexpr int32 MaxSiblingsRead = 50;

	struct FItem
	{
		FString Group;
		TSharedPtr<FJsonObject> Json;
	};

	struct FGroup
	{
		FString Id;
		FString Title;
		FString Class;
		FString Part;
		bool bUser = false;
		/** 部件组先只给标题，面板展开时再用 part 调一次读它的属性（控件多的界面一次全列会超预算）。 */
		bool bCollapsed = false;
	};

	int32 JsonChars(const TSharedPtr<FJsonObject>& InJson)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(InJson.ToSharedRef(), Writer);
		return Text.Len();
	}

	/** 枚举（含 ESlateVisibility）的全部成员名：面板给下拉（协调方要求，写进 7.3）。 */
	void AddMembers(const FGameTestPropRead& InRead, FJsonObject& OutItem)
	{
		const UEnum* Enum = nullptr;
		if (InRead.Kind == EGameTestKind::Visibility)
		{
			Enum = StaticEnum<ESlateVisibility>();
		}
		else if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(InRead.RawProp))
		{
			Enum = EnumProp->GetEnum();
		}
		else if (const FByteProperty* ByteProp = CastField<FByteProperty>(InRead.RawProp))
		{
			Enum = ByteProp->Enum;
		}
		if (Enum == nullptr)
		{
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Members;
		// 最后一个是自动加的 _MAX，不列
		for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
		{
			Members.Add(MakeShared<FJsonValueString>(Enum->GetNameStringByIndex(Index)));
		}
		OutItem.SetArrayField(TEXT("members"), Members);
	}

	TSharedPtr<FJsonObject> ItemJson(const FString& InProp, const FString& InName, const FGameTestPropRead& InRead, int32 InScore, bool bInValue)
	{
		const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("prop"), InProp);
		Item->SetStringField(TEXT("name"), InName);
		Item->SetStringField(TEXT("src"), InRead.Src.IsEmpty() ? FString(TEXT("cpp")) : InRead.Src);
		Item->SetStringField(TEXT("type"), InRead.Type);
		Item->SetStringField(TEXT("kind"), FGameTestPropKind::Name(InRead.Kind));
		if (bInValue && InRead.IsResolved())
		{
			const TSharedPtr<FJsonValue> Value = InRead.Value.ToJson();
			FString Text;
			if (Value.IsValid() && Value->TryGetString(Text) && Text.Len() > MaxValueChars)
			{
				Item->SetStringField(TEXT("value"), Text.Left(MaxValueChars) + TEXT("…"));
			}
			else if (Value.IsValid())
			{
				Item->SetField(TEXT("value"), Value);
			}
		}
		Item->SetNumberField(TEXT("score"), InScore);
		TArray<TSharedPtr<FJsonValue>> Ops;
		for (const FString& Op : FGameTestCompareRegistry::OpsFor(InRead.Kind))
		{
			Ops.Add(MakeShared<FJsonValueString>(Op));
		}
		Item->SetArrayField(TEXT("ops"), Ops);
		const bool bComparable = FGameTestPropKind::IsComparable(InRead.Kind);
		Item->SetBoolField(TEXT("comparable"), bComparable);
		if (!bComparable)
		{
			Item->SetStringField(TEXT("why"), FString::Printf(TEXT("%s 不能直接比较（展开成员或换一个属性）"), *InRead.Type));
		}
		Item->SetBoolField(TEXT("readOnly"), InRead.bReadOnly);
		AddMembers(InRead, *Item);
		return Item;
	}

	FString GroupOf(const FString& InCollectGroup)
	{
		if (InCollectGroup == TEXT("lua"))
		{
			return TEXT("self");
		}
		if (InCollectGroup == TEXT("itemLua"))
		{
			return TEXT("item");
		}
		return InCollectGroup;
	}

	/** UnLua 绑的模块名（面板按它找 Lua 源文件补 `---@field`，7.5）：对象上有 GetModuleName 就调一次。CDO 上也能调（返回常量）。 */
	FString LuaModuleOf(UObject* InObject)
	{
		UFunction* Function = InObject != nullptr ? InObject->FindFunction(TEXT("GetModuleName")) : nullptr;
		if (Function == nullptr)
		{
			return FString();
		}
		// 不能在只读上下文里调（只读闸门会拒绝调函数）：另开一个
		FGameTestFlowContext Plain;
		GameTestRpcRef::MakeContext(Plain);
		const TMap<FString, FGameTestValue> None;
		GameTestReflectCall::FArgs Args;
		Args.Named = &None;
		const FGameTestValue Result = GameTestReflectCall::Call(InObject, Function, Args, Plain);
		return Result.IsString() ? Result.GetString() : FString();
	}

	/**
	 * 类自己树里的每个部件（不进嵌套 WBP 里面）都是折叠的组，面板展开时再用 part 调一次读它的属性和状态；
	 * 嵌套的 UUserWidget 另标 user。InPrefix 是按部件读时外层部件的路径（`Header.`），给出的 part 从最外层对象写。
	 */
	void AddPartGroups(UWidgetTree* InTree, const UObject* InOwner, const FString& InPrefix, TArray<FGroup>& OutGroups)
	{
		if (InTree == nullptr)
		{
			return;
		}
		TArray<UWidget*> Widgets;
		InTree->GetAllWidgets(Widgets);
		for (UWidget* Widget : Widgets)
		{
			if (Widget == nullptr)
			{
				continue;
			}
			FString Part = Widget->GetName();
			if (InOwner != nullptr)
			{
				GameTestPropRead::PartPathOf(Widget, InOwner, Part);
			}
			Part = InPrefix + Part;
			const FString Id = TEXT("part:") + Part;
			if (OutGroups.ContainsByPredicate([&Id](const FGroup& G) { return G.Id == Id; }))
			{
				continue;
			}
			FGroup Group;
			Group.Id = Id;
			Group.Title = Part;
			Group.Class = Widget->GetClass()->GetName();
			Group.Part = Part;
			Group.bUser = Widget->IsA<UUserWidget>();
			Group.bCollapsed = true;
			OutGroups.Add(Group);
		}
	}
}

bool GameTestRpcRef::Props(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
{
	using namespace GameTestRpcPropsImpl;
	if (!InArgs.IsValid())
	{
		OutError = TEXT("gt.props 要 node、target 或 class");
		return false;
	}
	FGameTestFlowContext Flow;
	MakeContext(Flow);
	FGameTestFlowContext::FReadOnlyScope ReadOnly(Flow);

	// 对象：node / target（游戏开着），或只按类
	UObject* Object = nullptr;
	bool bClassOnly = false;
	FString ClassName;
	if (InArgs->TryGetStringField(TEXT("class"), ClassName) && !ClassName.IsEmpty() && !InArgs->HasField(TEXT("node")) && !InArgs->HasField(TEXT("target")))
	{
		UClass* Class = GameTestObjectRoots::FindClass(ClassName);
		if (Class == nullptr)
		{
			OutError = FString::Printf(TEXT("没有类 %s（类名写错了，或这个类所在的模块 / 资产没加载）"), *ClassName);
			return false;
		}
		Object = Class->GetDefaultObject();
		bClassOnly = true;
	}
	else
	{
		Object = ObjectFromArgs(*InArgs, Flow, OutError);
	}
	if (Object == nullptr)
	{
		return false;
	}
	FString Part;
	if (InArgs->TryGetStringField(TEXT("part"), Part) && !Part.IsEmpty() && !bClassOnly)
	{
		Object = GameTestPropRead::FindPart(Object, Part, OutError);
		if (Object == nullptr)
		{
			OutError = FString::Printf(TEXT("part_missing：%s"), *OutError);
			return false;
		}
	}
	FString Filter;
	InArgs->TryGetStringField(TEXT("filter"), Filter);
	int32 Offset = 0;
	int32 Limit = DefaultLimit;
	bool bSiblings = true;
	InArgs->TryGetNumberField(TEXT("offset"), Offset);
	InArgs->TryGetNumberField(TEXT("limit"), Limit);
	InArgs->TryGetBoolField(TEXT("siblings"), bSiblings);
	Offset = FMath::Max(0, Offset);
	Limit = FMath::Clamp(Limit, 1, MaxLimit);

	// 按部件读时路径都从外层对象写（`@部件.X`），条目数据对象只属于外层对象
	const FString PartPrefix = bClassOnly ? FString() : Part;
	UObject* Item = bClassOnly || !PartPrefix.IsEmpty() ? nullptr : GameTestPropRead::ListItemOf(Object);
	TArray<FGameTestIdentProp> Props;
	GameTestIdentSuggest::Collect(Object, Item, Flow, Props);
	// 文字部件的 :text / :textKey 在展开那个部件时列（部件组都折叠），这里不重复
	Props.RemoveAll([](const FGameTestIdentProp& Prop) { return Prop.Group.StartsWith(TEXT("part:")); });
	// 控件自带的可见性、文字、颜色、勾选这些也要列出来（Collect 只要项目自己的字段，挑标识用）
	GameTestPropList::AddReflected(Object, Flow, Props);

	// 同类：引用（不带 ident）收到的候选，算每项有几种值
	FGameTestRefForResult RefFor;
	TArray<FGameTestRefCandidate> Siblings;
	if (!bClassOnly)
	{
		GameTestRefFor::ForObject(Object, Flow, RefFor);
		FGameTestRef Bare;
		if (RefFor.Ref.IsValid() && FGameTestRef::FromJson(*RefFor.Ref, Bare) && bSiblings)
		{
			Bare.Ident.Reset();
			FString Ignored;
			FName Code;
			GameTestRefResolve::Collect(Bare, Flow, Siblings, Ignored, Code);
		}
	}

	TArray<FItem> Items;
	for (const FGameTestIdentProp& Prop : Props)
	{
		if (!Filter.IsEmpty() && !Prop.Prop.Contains(Filter))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Json = ItemJson(GameTestPropList::Rebase(Prop.Prop, PartPrefix), Prop.Name, Prop.Read, GameTestIdentSuggest::Score10(Prop.Score), !bClassOnly);
		if (Siblings.Num() > 1)
		{
			TSet<FString> Values;
			for (int32 Index = 0; Index < Siblings.Num() && Index < MaxSiblingsRead; ++Index)
			{
				const FGameTestPropRead Other = GameTestPropRead::ReadText(Siblings[Index].Object.Get(), Prop.Prop, Flow, Siblings[Index].Item.Get());
				Values.Add(Other.IsResolved() ? Other.Value.ToDisplay(80) : FString(TEXT("\x1F")));
			}
			Json->SetNumberField(TEXT("distinct"), Values.Num());
		}
		Items.Add({ GroupOf(Prop.Group), Json });
	}
	// 算出状态：适用于这个对象的都列
	const UObject* StateTarget = Object;
	for (const TSharedRef<IGameTestState>& State : FGameTestStateRegistry::Get().GetAll())
	{
		const FString Prop = TEXT(":") + State->GetKey().ToString();
		if (!State->AppliesTo(StateTarget) || (!Filter.IsEmpty() && !Prop.Contains(Filter)))
		{
			continue;
		}
		FGameTestPropRead Read;
		Read.Kind = State->GetKind();
		Read.Src = TEXT("state");
		Read.Type = TEXT("state");
		Read.bReadOnly = true;
		if (!bClassOnly)
		{
			Read = GameTestPropRead::ReadText(Object, Prop, Flow, Item);
			Read.Kind = State->GetKind();
			Read.Src = TEXT("state");
			Read.bReadOnly = true;
		}
		const TSharedPtr<FJsonObject> Json = ItemJson(GameTestPropList::Rebase(Prop, PartPrefix), State->GetSummary(), Read, State->IsStable() ? 3 : 0, !bClassOnly);
		Items.Add({ TEXT("states"), Json });
	}

	// 组：self、item、各部件（WidgetTree 顺序）、嵌套界面（折叠）、states
	TArray<FGroup> Groups;
	Groups.Add({ TEXT("self"), PartPrefix.IsEmpty() ? FString(TEXT("自己")) : FString(TEXT("属性")), Object->GetClass()->GetName(), FString(), false });
	if (Item != nullptr)
	{
		Groups.Add({ TEXT("item"), TEXT("条目的数据对象"), Item->GetClass()->GetName(), FString(), false });
	}
	for (const FItem& One : Items)
	{
		if (One.Group.StartsWith(TEXT("part:")) && !Groups.ContainsByPredicate([&One](const FGroup& G) { return G.Id == One.Group; }))
		{
			const FString PartName = One.Group.Mid(5);
			Groups.Add({ One.Group, PartName, FString(), PartName, false, false });
		}
	}
	UWidgetTree* Tree = nullptr;
	if (const UUserWidget* User = Cast<UUserWidget>(Object))
	{
		Tree = User->WidgetTree;
	}
	if (Tree == nullptr)
	{
		if (UWidgetBlueprintGeneratedClass* WidgetClass = Cast<UWidgetBlueprintGeneratedClass>(Object->GetClass()))
		{
			// 只按类：设计器树在生成类上（不开游戏也有）
			Tree = WidgetClass->GetWidgetTreeArchetype();
		}
	}
	AddPartGroups(Tree, bClassOnly ? nullptr : Object, PartPrefix.IsEmpty() ? FString() : PartPrefix + TEXT("."), Groups);
	Groups.Add({ TEXT("states"), TEXT("算出状态"), FString(), FString(), false });

	// 分页 + 字数预算：按组的顺序数项
	TArray<FItem> Ordered;
	for (const FGroup& Group : Groups)
	{
		for (const FItem& One : Items)
		{
			if (One.Group == Group.Id)
			{
				Ordered.Add(One);
			}
		}
	}
	int32 Chars = 0;
	int32 Taken = 0;
	bool bTruncated = false;
	TMap<FString, TArray<TSharedPtr<FJsonValue>>> PageByGroup;
	for (int32 Index = Offset; Index < Ordered.Num() && Taken < Limit; ++Index)
	{
		const int32 Size = JsonChars(Ordered[Index].Json);
		if (Taken > 0 && Chars + Size > MaxChars)
		{
			bTruncated = true;
			break;
		}
		Chars += Size;
		++Taken;
		PageByGroup.FindOrAdd(Ordered[Index].Group).Add(MakeShared<FJsonValueObject>(Ordered[Index].Json));
	}
	TArray<TSharedPtr<FJsonValue>> GroupJson;
	for (const FGroup& Group : Groups)
	{
		const TSharedRef<FJsonObject> One = MakeShared<FJsonObject>();
		One->SetStringField(TEXT("id"), Group.Id);
		One->SetStringField(TEXT("title"), Group.Title);
		if (!Group.Class.IsEmpty())
		{
			One->SetStringField(TEXT("class"), Group.Class);
		}
		if (!Group.Part.IsEmpty())
		{
			One->SetStringField(TEXT("part"), Group.Part);
		}
		if (Group.bUser)
		{
			One->SetBoolField(TEXT("user"), true);
		}
		if (Group.bCollapsed)
		{
			One->SetBoolField(TEXT("collapsed"), true);
		}
		const TArray<TSharedPtr<FJsonValue>>* Page = PageByGroup.Find(Group.Id);
		One->SetArrayField(TEXT("items"), Page != nullptr ? *Page : TArray<TSharedPtr<FJsonValue>>());
		GroupJson.Add(MakeShared<FJsonValueObject>(One));
	}

	OutResult = MakeShared<FJsonObject>();
	OutResult->SetStringField(TEXT("class"), Object->GetClass()->GetName());
	const FString LuaModule = LuaModuleOf(Object);
	if (!LuaModule.IsEmpty())
	{
		OutResult->SetStringField(TEXT("luaModule"), LuaModule);
	}
	if (RefFor.Ref.IsValid())
	{
		OutResult->SetObjectField(TEXT("ref"), RefFor.Ref);
		FString In;
		if (RefFor.Ref->TryGetStringField(TEXT("in"), In))
		{
			OutResult->SetStringField(TEXT("in"), In);
		}
	}
	if (!bClassOnly)
	{
		OutResult->SetNumberField(TEXT("siblings"), Siblings.Num());
		TArray<TSharedPtr<FJsonValue>> Suggest;
		for (const FGameTestIdentItem& Ident : RefFor.Ident)
		{
			Suggest.Add(MakeShared<FJsonValueString>(GameTestPropList::Rebase(Ident.Prop, PartPrefix)));
		}
		OutResult->SetArrayField(TEXT("suggest"), Suggest);
		OutResult->SetNumberField(TEXT("matches"), RefFor.Matches);
	}
	OutResult->SetArrayField(TEXT("groups"), GroupJson);
	OutResult->SetNumberField(TEXT("total"), Ordered.Num());
	const int32 Next = Offset + Taken;
	OutResult->SetBoolField(TEXT("hasMore"), Next < Ordered.Num());
	if (Next < Ordered.Num())
	{
		OutResult->SetNumberField(TEXT("nextOffset"), Next);
	}
	OutResult->SetBoolField(TEXT("truncated"), bTruncated);
	return true;
}
