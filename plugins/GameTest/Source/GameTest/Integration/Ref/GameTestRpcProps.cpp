// Copyright Epic Games, Inc. All Rights Reserved.

// gt.props（docs/game-test-ref.md 7.3）：一个对象的属性树，挑标识和写判断共用。
// 游戏开着时给值、同类里有几种值、推荐的标识组合；只给 class 时按类列（不开游戏也行：CDO 的反射 + WBP 的设计器树）。

#include "Integration/Ref/GameTestRpcRef.h"

#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Api/GameTestParamKind.h"
#include "Observe/Compare/GameTestCompareRegistry.h"
#include "Observe/Sources/GameTestReflectCall.h"
#include "Observe/Sources/GameTestReflectValue.h"
#include "Observe/Ref/GameTestClassFilter.h"
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
#include "Components/ActorComponent.h"
#include "Components/Widget.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
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

	/**
	 * 属性在 UE 细节面板里的分类（UPROPERTY 的 Category，如 `Behavior`、`Appearance|Font`）：面板照 UE 的样子按分类分组。
	 * Category 是元数据，只在带元数据的构建里有（编辑器、未烘焙的游戏），没有时不给，面板不分组。
	 */
	FString CategoryOf(const FProperty* InProperty)
	{
#if WITH_METADATA
		if (InProperty != nullptr && InProperty->HasMetaData(TEXT("Category")))
		{
			return InProperty->GetMetaData(TEXT("Category"));
		}
#endif
		return FString();
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
		const FString Category = CategoryOf(InRead.RawProp);
		if (!Category.IsEmpty())
		{
			Item->SetStringField(TEXT("cat"), Category);
		}
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
	/** 函数页最多列这么多个（Actor 继承来的可调函数有一两百个，再多面板也翻不过来）。 */
	constexpr int32 MaxFunctions = 300;

	/** 类名去掉蓝图生成类的 `_C`（和类树、引用里的写法一致）。 */
	FString ShortClassName(const UClass* InClass)
	{
		FString Name = InClass != nullptr ? InClass->GetName() : FString();
		if (Cast<UBlueprintGeneratedClass>(InClass) != nullptr)
		{
			Name.RemoveFromEnd(TEXT("_C"));
		}
		return Name;
	}

	/**
	 * 类的公有函数（用户 2026-11-21：方法页是空的）：反射里蓝图能调的（BlueprintCallable / BlueprintPure），含继承来的，
	 * 自己的在前；不开游戏也能读。项的形状同 Lua 类的 functions 组：{name, params, ret?, from?, src}，面板只列出来看。
	 */
	void AddFunctions(const UClass* InClass, const FString& InFilter, TArray<FItem>& OutItems)
	{
		if (InClass == nullptr)
		{
			return;
		}
		int32 Count = 0;
		TSet<FName> Seen;
		for (TFieldIterator<UFunction> It(InClass, EFieldIteratorFlags::IncludeSuper); It && Count < MaxFunctions; ++It)
		{
			const UFunction* Function = *It;
			if (Function == nullptr || !Function->HasAnyFunctionFlags(FUNC_Public) || !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure) || Function->HasAnyFunctionFlags(FUNC_Delegate))
			{
				continue;
			}
			const FString Name = Function->GetName();
			if (Seen.Contains(Function->GetFName()) || (!InFilter.IsEmpty() && !Name.Contains(InFilter)))
			{
				continue;
			}
			Seen.Add(Function->GetFName());
			const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("name"), Name);
			TArray<TSharedPtr<FJsonValue>> Params;
			FString Ret;
			// 2.1.6：参数都是数字 / 布尔 / 文字 / 枚举时，路径里能写常量参数 `@.名字(0, "")`：按参数顺序给个零值当起点，面板里再改
			TArray<FString> ArgDefaults;
			bool bArgsWritable = true;
			TSet<FString> AutoParams;
			for (const FGameTestParamInfo& Info : GameTestParamKind::Describe(*Function))
			{
				if (Info.Kind == EGameTestParamKind::WorldContext)
				{
					AutoParams.Add(Info.Name);
				}
			}
			for (TFieldIterator<FProperty> Param(Function); Param && Param->HasAnyPropertyFlags(CPF_Parm); ++Param)
			{
				if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					Ret = Param->GetCPPType();
					continue;
				}
				const bool bOut = Param->HasAnyPropertyFlags(CPF_OutParm) && !Param->HasAnyPropertyFlags(CPF_ConstParm | CPF_ReferenceParm);
				Params.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s%s %s"), bOut ? TEXT("out ") : TEXT(""), *Param->GetCPPType(), *Param->GetName())));
				if (bOut || AutoParams.Contains(Param->GetName()))
				{
					continue;
				}
				if (CastField<FBoolProperty>(*Param) != nullptr)
				{
					ArgDefaults.Add(TEXT("false"));
				}
				else if (CastField<FNumericProperty>(*Param) != nullptr || CastField<FEnumProperty>(*Param) != nullptr)
				{
					ArgDefaults.Add(TEXT("0"));
				}
				else if (CastField<FStrProperty>(*Param) != nullptr || CastField<FNameProperty>(*Param) != nullptr || CastField<FTextProperty>(*Param) != nullptr)
				{
					ArgDefaults.Add(TEXT("\"\""));
				}
				else
				{
					bArgsWritable = false;
				}
			}
			Json->SetArrayField(TEXT("params"), Params);
			if (!Ret.IsEmpty())
			{
				Json->SetStringField(TEXT("ret"), Ret);
			}
			const UClass* Owner = Function->GetOwnerClass();
			if (Owner != nullptr && Owner != InClass)
			{
				Json->SetStringField(TEXT("from"), ShortClassName(Owner));
			}
			const bool bBlueprint = Cast<UBlueprintGeneratedClass>(Owner) != nullptr;
			Json->SetStringField(TEXT("src"), bBlueprint ? TEXT("bp") : TEXT("cpp"));
			if (Function->HasAnyFunctionFlags(FUNC_BlueprintPure))
			{
				Json->SetBoolField(TEXT("pure"), true);
			}
			// 有返回值的取值函数（Pure 或 const）能写进路径 `@.名字(参数)` 当唯一标识的条件（用户 2026-11-21）：给 path 和 kind；
			// 参数只能写常量（2.1.6），有对象、结构体这类参数的不给 path
			const FProperty* Return = Function->GetReturnProperty();
			if (bArgsWritable && Return != nullptr && GameTestReflectValue::IsReadOnlyFunction(*Function))
			{
				const EGameTestKind Kind = FGameTestPropKind::Of(Return);
				Json->SetStringField(TEXT("path"), FGameTestPropPath::OfProp(Name) + TEXT("(") + FString::Join(ArgDefaults, TEXT(", ")) + TEXT(")"));
				if (ArgDefaults.Num() > 0)
				{
					Json->SetNumberField(TEXT("args"), ArgDefaults.Num());
				}
				Json->SetStringField(TEXT("kind"), FGameTestPropKind::Name(Kind));
				if (FGameTestPropKind::IsComparable(Kind))
				{
					TArray<TSharedPtr<FJsonValue>> Ops;
					for (const FString& Op : FGameTestCompareRegistry::OpsFor(Kind))
					{
						Ops.Add(MakeShared<FJsonValueString>(Op));
					}
					Json->SetArrayField(TEXT("ops"), Ops);
				}
			}
			OutItems.Add({ TEXT("functions"), Json });
			++Count;
		}
	}

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

	/** Actor 的组件：一个组件一组（折叠），展开时用 part = 组件名再调。按类读时只有 C++ 默认子对象（蓝图 SCS 加的组件要开游戏）。 */
	void AddComponentGroups(AActor* InActor, TArray<FGroup>& OutGroups)
	{
		if (InActor == nullptr)
		{
			return;
		}
		TInlineComponentArray<UActorComponent*> Components(InActor);
		for (const UActorComponent* Component : Components)
		{
			if (Component == nullptr)
			{
				continue;
			}
			const FString Name = Component->GetName();
			const FString Id = TEXT("part:") + Name;
			if (OutGroups.ContainsByPredicate([&Id](const FGroup& G) { return G.Id == Id; }))
			{
				continue;
			}
			FGroup Group;
			Group.Id = Id;
			Group.Title = Name;
			Group.Class = Component->GetClass()->GetName();
			Group.Part = Name;
			Group.bCollapsed = true;
			OutGroups.Add(Group);
		}
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
	FString LuaClass;
	if (InArgs->TryGetStringField(TEXT("class"), ClassName) && FGameTestClassFilter::ParseLua(ClassName, LuaClass) && !InArgs->HasField(TEXT("node")) && !InArgs->HasField(TEXT("target")))
	{
		// Lua 类（lua:模块）：读模块表的字段和函数（不开游戏读不到，Lua 环境要在）
		return PropsLua(LuaClass, OutResult, OutError);
	}
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

	// 2.1.0：按部件读时路径相对那个部件写（`@.X`、`@:text`），面板拼上 `/部件`；部件组的 part 仍从外层对象写。
	// 条目数据对象只属于外层对象
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
		// 只按类读时也给值：类默认对象（CDO）上的值就是蓝图 / 类的默认值，和 AssetForge 细节面板一样（用户 2026-11-21：没运行时值全空）
		const TSharedPtr<FJsonObject> Json = ItemJson(Prop.Prop, Prop.Name, Prop.Read, GameTestIdentSuggest::Score10(Prop.Score), true);
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
	AddFunctions(Object->GetClass(), Filter, Items);
	// 算出状态：适用于这个对象的都列
	const UObject* StateTarget = Object;
	for (const TSharedRef<IGameTestState>& State : FGameTestStateRegistry::Get().GetAll())
	{
		const FString Prop = FGameTestPropPath::OfState(State->GetKey().ToString());
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
		const TSharedPtr<FJsonObject> Json = ItemJson(Prop, State->GetSummary(), Read, State->IsStable() ? 3 : 0, !bClassOnly);
		Items.Add({ TEXT("states"), Json });
	}

	// 组：self、item、各部件（WidgetTree 顺序）、嵌套界面（折叠）、states
	TArray<FGroup> Groups;
	Groups.Add({ TEXT("self"), TEXT("属性"), Object->GetClass()->GetName(), FString(), false });
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
	// Actor：每个组件一个部件组（面板的组件树全靠它；part 是组件名，同 target 的 part）
	AddComponentGroups(Cast<AActor>(Object), Groups);
	Groups.Add({ TEXT("states"), TEXT("算出状态"), FString(), FString(), false });
	Groups.Add({ TEXT("functions"), TEXT("函数"), FString(), FString(), false });

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
			Suggest.Add(MakeShared<FJsonValueString>(Ident.Prop));
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
