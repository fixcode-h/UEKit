// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestRefResolve.h"

#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Observe/Compare/GameTestJudge.h"
#include "Observe/GameTestItemKey.h"
#include "Observe/Lua/GameTestLuaObjects.h"
#include "Observe/Ref/GameTestClassFilter.h"
#include "Observe/Ref/GameTestIdentSuggest.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Sources/GameTestObjectRoots.h"
#include "Observe/Sources/GameTestPathText.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ActorComponent.h"
#include "Components/ListView.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Subsystems/Subsystem.h"
#include "UObject/UObjectIterator.h"

namespace GameTestRefResolveImpl
{
	/** in.obj 一层套一层：超过这么多层按成环算（加载时已经查过 ref_cycle，这里只防运行时 $var 绕圈）。 */
	constexpr int32 MaxDepth = 8;
	/** 一次最多收这么多候选（all 扫全部对象时防卡）。 */
	constexpr int32 MaxCandidates = 2000;

	bool IsLive(const UObject* InObject)
	{
		return IsValid(InObject) && !InObject->IsTemplate() && !InObject->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject);
	}

	void Fail(FGameTestRefResult& OutResult, FName InCode, const FString& InError)
	{
		OutResult.Code = InCode;
		OutResult.Error = InError;
	}

	/** 屏上的界面：每个视口根界面，以及它们树里嵌套的 UUserWidget（按树的顺序，深的在后）。 */
	void CollectUserWidgets(UUserWidget* InWidget, const FGameTestClassFilter& InClass, int32 InDepth, TArray<FGameTestRefCandidate>& OutCandidates)
	{
		if (InWidget == nullptr || InDepth > MaxDepth || OutCandidates.Num() >= MaxCandidates)
		{
			return;
		}
		if (InClass.Matches(InWidget))
		{
			FGameTestRefCandidate Candidate;
			Candidate.Object = InWidget;
			OutCandidates.Add(Candidate);
		}
		if (InWidget->WidgetTree == nullptr)
		{
			return;
		}
		TArray<UUserWidget*> Nested;
		InWidget->WidgetTree->ForEachWidget([&Nested](UWidget* Widget)
		{
			if (UUserWidget* User = Cast<UUserWidget>(Widget))
			{
				Nested.Add(User);
			}
		});
		for (UUserWidget* User : Nested)
		{
			CollectUserWidgets(User, InClass, InDepth + 1, OutCandidates);
		}
	}

	void CollectScreen(UWorld* InWorld, const FGameTestClassFilter& InClass, TArray<FGameTestRefCandidate>& OutCandidates)
	{
		TArray<UUserWidget*> Screens;
		GameTestWidgetProbe::ListScreens(InWorld, Screens);
		// ListScreens 按 ZOrder 从大到小、同 ZOrder 新的在前排（最上层在前）；协议第 1 节要 first = 最早加入、last = 最上层，
		// 所以倒过来收：候选从最底层、最早加入的开始，many 的 first / last 直接取两头
		for (int32 Index = Screens.Num() - 1; Index >= 0; --Index)
		{
			CollectUserWidgets(Screens[Index], InClass, 0, OutCandidates);
		}
	}

	void CollectWorld(UWorld* InWorld, const FGameTestClassFilter& InClass, TArray<FGameTestRefCandidate>& OutCandidates)
	{
		for (TActorIterator<AActor> It(InWorld); It && OutCandidates.Num() < MaxCandidates; ++It)
		{
			if (IsLive(*It) && InClass.Matches(*It))
			{
				FGameTestRefCandidate Candidate;
				Candidate.Object = *It;
				OutCandidates.Add(Candidate);
			}
		}
	}

	void CollectAll(UWorld* InWorld, UClass* InClass, TArray<FGameTestRefCandidate>& OutCandidates)
	{
		if (InClass == nullptr)
		{
			return;
		}
		TArray<UObject*> Objects;
		GetObjectsOfClass(InClass, Objects, /*bIncludeDerivedClasses=*/true);
		for (UObject* Object : Objects)
		{
			if (OutCandidates.Num() >= MaxCandidates)
			{
				break;
			}
			// 只要这个世界里的（多客户端时每个世界各有一份）和不属于任何世界的（子系统、数据对象）
			const UWorld* ObjectWorld = Object != nullptr ? Object->GetWorld() : nullptr;
			if (IsLive(Object) && (ObjectWorld == nullptr || ObjectWorld == InWorld))
			{
				FGameTestRefCandidate Candidate;
				Candidate.Object = Object;
				OutCandidates.Add(Candidate);
			}
		}
	}

	/** 绑了 UnLua 的对象（in: lua）：Lua 类时就是继承链对得上的那些，UE 类时问 UELink 再按类过滤。 */
	bool CollectLua(UWorld* InWorld, const FGameTestClassFilter& InClass, TArray<FGameTestRefCandidate>& OutCandidates, FString& OutError)
	{
		TArray<FString> Paths;
		if (InClass.IsLua())
		{
			Paths = InClass.LuaPaths.Array();
		}
		else
		{
			TArray<FGameTestLuaObject> Objects;
			if (!GameTestLuaObjects::List(InWorld, InClass.Class, Objects, OutError))
			{
				return false;
			}
			for (const FGameTestLuaObject& Object : Objects)
			{
				Paths.Add(Object.Path);
			}
		}
		for (const FString& Path : Paths)
		{
			if (OutCandidates.Num() >= MaxCandidates)
			{
				break;
			}
			UObject* Object = GameTestLuaObjects::FindByPath(Path);
			const UWorld* ObjectWorld = Object != nullptr ? Object->GetWorld() : nullptr;
			if (IsLive(Object) && (ObjectWorld == nullptr || ObjectWorld == InWorld) && InClass.Matches(Object))
			{
				FGameTestRefCandidate Candidate;
				Candidate.Object = Object;
				OutCandidates.Add(Candidate);
			}
		}
		return true;
	}

	/** Lua 类按 all 找：继承链对得上的对象就是全部（不能按 UClass 扫）。 */
	void CollectAllOf(UWorld* InWorld, const FGameTestClassFilter& InClass, UClass* InUClass, TArray<FGameTestRefCandidate>& OutCandidates)
	{
		if (InClass.IsLua())
		{
			FString Ignored;
			CollectLua(InWorld, InClass, OutCandidates, Ignored);
			return;
		}
		CollectAll(InWorld, InUClass, OutCandidates);
	}

	/** 数组写法的一处。 */
	bool CollectOne(EGameTestRefIn InWhere, UWorld* InWorld, const FGameTestClassFilter& InClass, TArray<FGameTestRefCandidate>& OutCandidates, FString& OutError)
	{
		switch (InWhere)
		{
		case EGameTestRefIn::Screen:
			CollectScreen(InWorld, InClass, OutCandidates);
			return true;
		case EGameTestRefIn::World:
			CollectWorld(InWorld, InClass, OutCandidates);
			return true;
		case EGameTestRefIn::Lua:
			return CollectLua(InWorld, InClass, OutCandidates, OutError);
		default:
			return true;
		}
	}

	void AddIfMatches(UObject* InObject, const FGameTestClassFilter& InClass, TArray<FGameTestRefCandidate>& OutCandidates)
	{
		if (IsLive(InObject) && InClass.Matches(InObject))
		{
			FGameTestRefCandidate Candidate;
			Candidate.Object = InObject;
			OutCandidates.Add(Candidate);
		}
	}

	bool CollectSource(const FString& InSource, const FGameTestClassFilter& InClass, FGameTestFlowContext& InContext, TArray<FGameTestRefCandidate>& OutCandidates, FString& OutError)
	{
		const FGameTestValue Value = GameTestPathText::ResolveSourceText(InSource, InContext);
		if (Value.IsUnresolved())
		{
			OutError = FString::Printf(TEXT("in 的 %s 取不到：%s"), *InSource, *Value.GetString());
			return false;
		}
		if (UObject* Single = Value.GetObject())
		{
			AddIfMatches(Single, InClass, OutCandidates);
		}
		for (const FGameTestValue& Item : Value.GetArray())
		{
			AddIfMatches(Item.GetObject(), InClass, OutCandidates);
		}
		for (const TPair<FString, FGameTestValue>& Pair : Value.GetMap())
		{
			AddIfMatches(Pair.Value.GetObject(), InClass, OutCandidates);
		}
		return true;
	}

	/** 引用是怎么解析出来的，按 depth 防绕圈。 */
	FGameTestRefResult ResolveAt(const FGameTestRef& InRef, FGameTestFlowContext& InContext, int32 InDepth);
	FGameTestRefResult ResolveValueAt(const TSharedPtr<FJsonValue>& InRef, FGameTestFlowContext& InContext, int32 InDepth);

	bool CollectContainer(const FGameTestRef& InRef, const FGameTestClassFilter& InClass, FGameTestFlowContext& InContext, int32 InDepth, TArray<FGameTestRefCandidate>& OutCandidates, FString& OutError, FName& OutCode)
	{
		const FGameTestRefResult Outer = ResolveValueAt(InRef.InObj, InContext, InDepth + 1);
		if (!Outer.IsOk())
		{
			OutError = FString::Printf(TEXT("外层对象没找到：%s"), *Outer.Error);
			OutCode = Outer.Code.IsNone() ? FName(TEXT("ref_not_found")) : Outer.Code;
			return false;
		}
		if (InRef.In == EGameTestRefIn::Owner)
		{
			AActor* Actor = Cast<AActor>(Outer.Get());
			if (Actor == nullptr)
			{
				OutError = FString::Printf(TEXT("in.obj 找到的 %s 不是 Actor，没有组件；要找它里面的条目时写 part（容器部件名）"), *Outer.Get()->GetClass()->GetName());
				OutCode = TEXT("ref_need_owner");
				return false;
			}
			TInlineComponentArray<UActorComponent*> Components(Actor);
			for (UActorComponent* Component : Components)
			{
				AddIfMatches(Component, InClass, OutCandidates);
			}
			return true;
		}
		FString PartError;
		UWidget* Container = Cast<UWidget>(GameTestPropRead::FindPart(Outer.Get(), InRef.InPart, PartError));
		if (Container == nullptr)
		{
			OutError = PartError.IsEmpty() ? FString::Printf(TEXT("%s 不是控件"), *InRef.InPart) : PartError;
			OutCode = TEXT("part_missing");
			return false;
		}
		TArray<FGameTestKeyEntry> Entries;
		if (!GameTestItemKey::CollectEntries(Container, Entries))
		{
			OutError = FString::Printf(TEXT("%s 不是容器（ListView 族、Panel、DynamicEntryBox）"), *InRef.InPart);
			OutCode = TEXT("part_missing");
			return false;
		}
		for (const FGameTestKeyEntry& Entry : Entries)
		{
			UWidget* Widget = Entry.Widget.Get();
			// 没显示出来的 ListView 条目：只有数据对象，类认不了，先收进来让 @item 过滤
			if (Widget != nullptr && !InClass.Matches(Widget))
			{
				continue;
			}
			if (Widget == nullptr && !Entry.Item.IsValid())
			{
				continue;
			}
			FGameTestRefCandidate Candidate;
			Candidate.Object = Widget;
			Candidate.Item = Entry.Item;
			Candidate.Index = Entry.Index;
			Candidate.Container = Container;
			OutCandidates.Add(Candidate);
		}
		return true;
	}

	FVector LocationOfCandidate(const FGameTestRefCandidate& InCandidate, bool& bOutHas)
	{
		FVector Location = FVector::ZeroVector;
		const UObject* Object = InCandidate.Object.Get();
		if (const AActor* Actor = Cast<AActor>(Object))
		{
			bOutHas = true;
			return Actor->GetActorLocation();
		}
		if (const UActorComponent* Component = Cast<UActorComponent>(Object))
		{
			if (const AActor* Owner = Component->GetOwner())
			{
				bOutHas = true;
				return Owner->GetActorLocation();
			}
		}
		bOutHas = false;
		return Location;
	}

	/** near 的位置：[x,y,z]、$var（向量或对象）、@pawn 这类来源、另一个引用。 */
	bool NearPoint(const FGameTestRef& InRef, FGameTestFlowContext& InContext, int32 InDepth, FVector& OutPoint)
	{
		UWorld* World = GameTestWidgetProbe::WorldOf(InContext);
		FGameTestValue Value;
		if (!InRef.Near.IsValid())
		{
			FString Error;
			const AActor* Pawn = World != nullptr ? Cast<AActor>(GameTestObjectRoots::Player(*World, TEXT("pawn"), Error)) : nullptr;
			if (Pawn == nullptr)
			{
				return false;
			}
			OutPoint = Pawn->GetActorLocation();
			return true;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		FString Text;
		if (InRef.Near->TryGetObject(Object) || (InRef.Near->TryGetString(Text) && GameTestRefForm::IsName(Text)))
		{
			const FGameTestRefResult Other = ResolveValueAt(InRef.Near, InContext, InDepth + 1);
			bool bHas = false;
			OutPoint = LocationOfCandidate(Other.Picked, bHas);
			return bHas;
		}
		if (InRef.Near->TryGetString(Text) && Text.StartsWith(TEXT("@")))
		{
			Value = GameTestPathText::ResolveSourceText(Text, InContext);
		}
		else
		{
			Value = GameTestRefs::EvalJson(InRef.Near, InContext);
		}
		if (Value.Type == EGameTestValueType::Vector)
		{
			OutPoint = Value.GetVector();
			return true;
		}
		const TArray<FGameTestValue>& Items = Value.GetArray();
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (Items.Num() == 3 && Items[0].TryNumber(X) && Items[1].TryNumber(Y) && Items[2].TryNumber(Z))
		{
			OutPoint = FVector(X, Y, Z);
			return true;
		}
		FGameTestRefCandidate Candidate;
		Candidate.Object = Value.GetObject();
		bool bHas = false;
		OutPoint = LocationOfCandidate(Candidate, bHas);
		return bHas;
	}

	/** 歧义时的说明：每个候选的标识值 + 一个能区分它们的属性建议。 */
	FString DescribeAmbiguous(const FGameTestRefResult& InResult, FGameTestFlowContext& InContext)
	{
		TArray<FString> Lines;
		for (int32 Index = 0; Index < InResult.Matches.Num() && Index < 8; ++Index)
		{
			const FGameTestRefCandidate& Candidate = InResult.Matches[Index];
			TArray<FString> Actuals;
			GameTestRefResolve::MatchesIdent(Candidate, InResult.Ref.Ident, InContext, &Actuals);
			const FString Name = GameTestRefResolve::DisplayName(Candidate.Object.Get());
			Lines.Add(Actuals.Num() > 0 ? FString::Printf(TEXT("%s（%s）"), *Name, *FString::Join(Actuals, TEXT("，"))) : Name);
		}
		FString Suggest;
		TArray<FGameTestIdentItem> Extra;
		if (InResult.Matches.Num() > 1 && GameTestIdentSuggest::Pick(InResult.Matches[0], InResult.Matches, InContext, Extra) && Extra.Num() > 0)
		{
			TArray<FString> Props;
			for (const FGameTestIdentItem& Item : Extra)
			{
				Props.Add(Item.Prop);
			}
			Suggest = FString::Printf(TEXT("；可以在 ident 里加 %s 区分"), *FString::Join(Props, TEXT(" + ")));
		}
		return FString::Printf(TEXT("%s 找到 %d 个：%s%s%s（或写 many: first / last / nearest）"), *InResult.Ref.Describe(), InResult.Matches.Num(),
			*FString::Join(Lines, TEXT("、")), InResult.Matches.Num() > 8 ? TEXT("…") : TEXT(""), *Suggest);
	}

	FGameTestRefResult ResolveAt(const FGameTestRef& InRef, FGameTestFlowContext& InContext, int32 InDepth)
	{
		FGameTestRefResult Result;
		Result.Ref = InRef;
		if (InDepth > MaxDepth)
		{
			Fail(Result, TEXT("ref_cycle"), TEXT("引用的 in.obj 套了太多层（可能绕成了环）"));
			return Result;
		}
		TArray<FGameTestRefCandidate> Candidates;
		FString Error;
		FName Code;
		if (!GameTestRefResolve::Collect(InRef, InContext, Candidates, Error, Code))
		{
			Fail(Result, Code.IsNone() ? FName(TEXT("ref_not_found")) : Code, Error);
			return Result;
		}
		Result.ClassCount = Candidates.Num();
		for (const FGameTestRefCandidate& Candidate : Candidates)
		{
			if (InRef.Ident.Num() == 0 || GameTestRefResolve::MatchesIdent(Candidate, InRef.Ident, InContext))
			{
				Result.Matches.Add(Candidate);
			}
		}
		if (Result.Matches.Num() == 0)
		{
			Fail(Result, TEXT("ref_not_found"), Result.ClassCount == 0
				? FString::Printf(TEXT("%s 没找到（同类一个都没有）"), *InRef.Describe())
				: FString::Printf(TEXT("%s 没找到（同类有 %d 个，标识都对不上）"), *InRef.Describe(), Result.ClassCount));
			return Result;
		}
		const FString Many = InRef.Many.IsEmpty() ? InContext.Options.RefMany : InRef.Many;
		if (Result.Matches.Num() == 1)
		{
			Result.Picked = Result.Matches[0];
			Result.By = InRef.Ident.Num() > 0 && Result.ClassCount > 1 ? TEXT("ident") : TEXT("only");
		}
		else if (Many.Equals(TEXT("first"), ESearchCase::IgnoreCase))
		{
			Result.Picked = Result.Matches[0];
			Result.By = TEXT("first");
		}
		else if (Many.Equals(TEXT("last"), ESearchCase::IgnoreCase))
		{
			Result.Picked = Result.Matches.Last();
			Result.By = TEXT("last");
		}
		else if (Many.Equals(TEXT("nearest"), ESearchCase::IgnoreCase))
		{
			FVector Point = FVector::ZeroVector;
			if (!NearPoint(InRef, InContext, InDepth, Point))
			{
				Fail(Result, TEXT("ref_ambiguous"), FString::Printf(TEXT("%s 找到 %d 个，要按 nearest 挑但 near 的位置取不到（没有本地 Pawn 或 near 写错）"), *InRef.Describe(), Result.Matches.Num()));
				return Result;
			}
			// 只在有世界位置的候选（Actor、场景组件）里挑；一个都没有（界面、数据对象）时不悄悄退成 first，按歧义报错
			double Best = TNumericLimits<double>::Max();
			bool bAnyLocation = false;
			for (const FGameTestRefCandidate& Candidate : Result.Matches)
			{
				bool bHas = false;
				const FVector Location = LocationOfCandidate(Candidate, bHas);
				if (!bHas)
				{
					continue;
				}
				const double Distance = FVector::DistSquared(Location, Point);
				if (!bAnyLocation || Distance < Best)
				{
					bAnyLocation = true;
					Best = Distance;
					Result.Picked = Candidate;
				}
			}
			if (!bAnyLocation)
			{
				Fail(Result, TEXT("ref_ambiguous"), FString::Printf(TEXT("%s 找到 %d 个，但它们都没有世界位置（不是 Actor 或场景组件），不能按 nearest 挑：加标识区分，或改用 first / last"), *InRef.Describe(), Result.Matches.Num()));
				return Result;
			}
			Result.By = TEXT("nearest");
		}
		else
		{
			Fail(Result, TEXT("ref_ambiguous"), DescribeAmbiguous(Result, InContext));
			return Result;
		}
		// 挑中的是还没显示出来的 ListView 条目：能改游戏时滚出来，下一帧再找
		if (!Result.Picked.Object.IsValid() && Result.Picked.Item.IsValid())
		{
			UListView* List = Cast<UListView>(Result.Picked.Container.Get());
			if (List != nullptr && !InContext.IsReadOnly() && Result.Picked.Index != INDEX_NONE)
			{
				List->ScrollIndexIntoView(Result.Picked.Index);
			}
			Fail(Result, TEXT("ref_not_found"), FString::Printf(TEXT("%s：条目第 %d 项还没显示出来（列表没滚到）"), *InRef.Describe(), Result.Picked.Index));
		}
		return Result;
	}

	FGameTestRefResult ResolveValueAt(const TSharedPtr<FJsonValue>& InRef, FGameTestFlowContext& InContext, int32 InDepth)
	{
		FGameTestRefResult Result;
		FGameTestRef Ref;
		UObject* Direct = nullptr;
		FString Error;
		if (!GameTestRefResolve::ToRef(InRef, InContext, Ref, Direct, Error))
		{
			Result.Ref = Ref;
			Fail(Result, TEXT("ref_not_found"), Error);
			return Result;
		}
		if (Direct != nullptr || Ref.Class.IsEmpty())
		{
			Result.Ref = Ref;
			Result.Picked.Object = Direct;
			Result.Picked.Item = GameTestPropRead::ListItemOf(Direct);
			Result.By = TEXT("var");
			Result.ClassCount = Direct != nullptr ? 1 : 0;
			if (Direct == nullptr || !IsValid(Direct))
			{
				Fail(Result, TEXT("ref_not_found"), FString::Printf(TEXT("$%s 里的对象已经没了（界面关了或 Actor 销毁了），重新 obj.find"), *Ref.Name));
				Result.Picked = FGameTestRefCandidate();
			}
			else
			{
				Result.Matches.Add(Result.Picked);
			}
			return Result;
		}
		return ResolveAt(Ref, InContext, InDepth);
	}
}

//~ 公开接口 ---------------------------------------------------------------------

UWidget* FGameTestTargetResult::GetWidget() const
{
	return Cast<UWidget>(Part.Get());
}

bool GameTestRefResolve::ToRef(const TSharedPtr<FJsonValue>& InRef, FGameTestFlowContext& InContext, FGameTestRef& OutRef, UObject*& OutDirect, FString& OutError)
{
	OutRef = FGameTestRef();
	OutDirect = nullptr;
	FString Text;
	TSharedPtr<FJsonObject> Inline;
	switch (GameTestRefForm::Classify(InRef, Text, Inline))
	{
	case EGameTestRefForm::Named:
	{
		// objects 里写成来源字符串的（"@pawn"）：现取，和 $var 一样当直接给的对象
		if (const FString* Source = InContext.GetScope().ObjectSources.Find(Text))
		{
			OutRef.Name = Text;
			const FGameTestValue Value = GameTestPathText::ResolveSourceText(*Source, InContext);
			OutDirect = Value.GetObject();
			if (OutDirect == nullptr)
			{
				OutError = Value.IsUnresolved() ? FString::Printf(TEXT("「%s」（%s）取不到：%s"), *Text, **Source, *Value.GetString())
					: FString::Printf(TEXT("「%s」（%s）不是对象（是 %s）"), *Text, **Source, *Value.TypeName());
				return false;
			}
			return true;
		}
		const TSharedPtr<FJsonObject>* Def = InContext.GetScope().ObjectDefs.Find(Text);
		if (Def == nullptr || !Def->IsValid() || !FGameTestRef::FromJson(**Def, OutRef))
		{
			OutError = FString::Printf(TEXT("objects 里没有「%s」"), *Text);
			return false;
		}
		OutRef.Name = Text;
		return true;
	}
	case EGameTestRefForm::Var:
	{
		OutRef.Name = Text;
		FGameTestValue Value;
		if (!InContext.FindVar(Text, Value))
		{
			OutError = FString::Printf(TEXT("变量 $%s 还没有值（先 obj.find saveAs）"), *Text);
			return false;
		}
		OutDirect = Value.GetObject();
		if (OutDirect == nullptr)
		{
			OutError = Value.IsUnresolved() ? Value.GetString() : FString::Printf(TEXT("$%s 不是对象（是 %s）或对象已经没了"), *Text, *Value.TypeName());
			return false;
		}
		return true;
	}
	case EGameTestRefForm::Inline:
		if (!FGameTestRef::FromJson(*Inline, OutRef))
		{
			OutError = TEXT("对象引用要写 class");
			return false;
		}
		return true;
	case EGameTestRefForm::Selector:
		OutError = FString::Printf(TEXT("「%s」是旧的选择器写法，v4 不认（selector_removed）"), *Text.Left(80));
		return false;
	default:
		OutError = TEXT("对象引用要写 objects 里的名字、\"$变量\" 或 {\"class\": …}");
		return false;
	}
}

bool GameTestRefResolve::Collect(const FGameTestRef& InRef, FGameTestFlowContext& InContext, TArray<FGameTestRefCandidate>& OutCandidates, FString& OutError, FName& OutCode)
{
	using namespace GameTestRefResolveImpl;
	OutCandidates.Reset();
	UWorld* World = GameTestWidgetProbe::WorldOf(InContext);
	if (World == nullptr)
	{
		OutError = TEXT("没有游戏世界（游戏没开？）");
		OutCode = TEXT("ref_not_found");
		return false;
	}
	FGameTestClassFilter Filter;
	if (!Filter.Build(InRef.Class, World, OutError, OutCode))
	{
		return false;
	}
	UClass* Class = Filter.IsLua() ? nullptr : GameTestObjectRoots::FindClass(InRef.Class);
	switch (InRef.In)
	{
	case EGameTestRefIn::Screen:
		CollectScreen(World, Filter, OutCandidates);
		return true;
	case EGameTestRefIn::World:
		CollectWorld(World, Filter, OutCandidates);
		return true;
	case EGameTestRefIn::Lua:
		if (!CollectLua(World, Filter, OutCandidates, OutError))
		{
			OutCode = TEXT("ref_not_found");
			return false;
		}
		return true;
	case EGameTestRefIn::List:
	{
		// 几处的并集：同一个对象（比如绑了 Lua 的界面）只算一次，按数组的顺序收
		TArray<FGameTestRefCandidate> Part;
		TSet<const UObject*> Seen;
		for (const EGameTestRefIn Where : InRef.InList)
		{
			Part.Reset();
			if (!CollectOne(Where, World, Filter, Part, OutError))
			{
				OutCode = TEXT("ref_not_found");
				return false;
			}
			for (const FGameTestRefCandidate& Candidate : Part)
			{
				const UObject* Object = Candidate.Object.Get();
				if (Object != nullptr && !Seen.Contains(Object))
				{
					Seen.Add(Object);
					OutCandidates.Add(Candidate);
				}
			}
		}
		return true;
	}
	case EGameTestRefIn::All:
		CollectAllOf(World, Filter, Class, OutCandidates);
		return true;
	case EGameTestRefIn::Source:
		return CollectSource(InRef.InSource, Filter, InContext, OutCandidates, OutError);
	case EGameTestRefIn::Container:
	case EGameTestRefIn::Owner:
		return CollectContainer(InRef, Filter, InContext, 0, OutCandidates, OutError, OutCode);
	default:
		break;
	}
	// in 省略：Lua 类就在绑了 Lua 的对象里找；UE 类按类推（类还没加载时按名字：先屏上再关卡）
	if (Filter.IsLua())
	{
		return CollectLua(World, Filter, OutCandidates, OutError);
	}
	if (Class == nullptr)
	{
		CollectScreen(World, Filter, OutCandidates);
		if (OutCandidates.Num() == 0)
		{
			CollectWorld(World, Filter, OutCandidates);
		}
		return true;
	}
	if (Class->IsChildOf(UUserWidget::StaticClass()))
	{
		CollectScreen(World, Filter, OutCandidates);
		return true;
	}
	if (Class->IsChildOf(AActor::StaticClass()))
	{
		CollectWorld(World, Filter, OutCandidates);
		return true;
	}
	if (Class->IsChildOf(UActorComponent::StaticClass()))
	{
		OutError = FString::Printf(TEXT("%s 是组件类：要写 in: {\"obj\": 它所在 Actor 的引用}"), *InRef.Class);
		OutCode = TEXT("ref_need_owner");
		return false;
	}
	if (Class->IsChildOf(USubsystem::StaticClass()))
	{
		FString Error;
		if (UObject* Subsystem = GameTestObjectRoots::Subsystem(*World, InRef.Class, Error))
		{
			FGameTestRefCandidate Candidate;
			Candidate.Object = Subsystem;
			OutCandidates.Add(Candidate);
		}
		return true;
	}
	CollectAll(World, Class, OutCandidates);
	return true;
}

bool GameTestRefResolve::MatchesIdent(const FGameTestRefCandidate& InCandidate, const TArray<FGameTestIdentItem>& InIdent, FGameTestFlowContext& InContext, TArray<FString>* OutActuals)
{
	bool bAll = true;
	for (const FGameTestIdentItem& Item : InIdent)
	{
		const FGameTestJudgeResult Judged = GameTestJudge::Test(InCandidate.Object.Get(), Item, InContext, InCandidate.Item.Get());
		if (OutActuals != nullptr)
		{
			OutActuals->Add(FString::Printf(TEXT("%s=%s"), *Item.Prop, Judged.Read.IsResolved() ? *Judged.Read.Value.ToDisplay(40) : TEXT("读不到")));
		}
		if (!Judged.bHolds)
		{
			bAll = false;
			if (OutActuals == nullptr)
			{
				return false;
			}
		}
	}
	return bAll;
}

FGameTestRefResult GameTestRefResolve::ResolveRef(const FGameTestRef& InRef, FGameTestFlowContext& InContext)
{
	return GameTestRefResolveImpl::ResolveAt(InRef, InContext, 0);
}

FGameTestRefResult GameTestRefResolve::Resolve(const TSharedPtr<FJsonValue>& InRef, FGameTestFlowContext& InContext)
{
	return GameTestRefResolveImpl::ResolveValueAt(InRef, InContext, 0);
}

FGameTestTargetResult GameTestRefResolve::ResolveTarget(const TSharedPtr<FJsonValue>& InTarget, FGameTestFlowContext& InContext)
{
	FGameTestTargetResult Result;
	if (!FGameTestTarget::FromJson(InTarget, Result.Target))
	{
		FString Text;
		Result.Code = InTarget.IsValid() && InTarget->TryGetString(Text) ? FName(TEXT("selector_removed")) : FName(TEXT("bad_type"));
		Result.Error = Result.Code == TEXT("selector_removed")
			? FString::Printf(TEXT("「%s」是旧的选择器写法，v4 不认：写 {\"obj\": 引用, \"part\": \"部件名\"}"), *Text.Left(80))
			: FString(TEXT("target 要写 {\"obj\": 引用, \"part\": \"部件名\"} 或直接写一个引用"));
		return Result;
	}
	// part / slate 里可以写 `$变量`（被 call 的子脚本按参数点不同的部件）：按文字替换，规则同协议 1.1 节
	for (FString* Text : { &Result.Target.Part, &Result.Target.Slate })
	{
		if (Text->Contains(TEXT("$")))
		{
			FString Expanded;
			FString EvalError;
			if (!GameTestRefs::EvalText(*Text, InContext, Expanded, EvalError))
			{
				Result.Code = TEXT("unresolved");
				Result.Error = FString::Printf(TEXT("「%s」取不到值：%s"), **Text, *EvalError);
				return Result;
			}
			*Text = Expanded;
		}
	}
	Result.Ref = Resolve(Result.Target.Obj, InContext);
	if (!Result.Ref.IsOk())
	{
		Result.Code = Result.Ref.Code;
		Result.Error = Result.Ref.Error;
		return Result;
	}
	FString PartError;
	UObject* Part = GameTestPropRead::FindPart(Result.Ref.Get(), Result.Target.Part, PartError);
	if (Part == nullptr)
	{
		Result.Code = TEXT("part_missing");
		Result.Error = PartError;
		return Result;
	}
	Result.Part = Part;
	return Result;
}

FString GameTestRefResolve::DisplayName(const UObject* InObject)
{
	return InObject != nullptr ? GameTestObjectRoots::DisplayName(InObject) : FString(TEXT("（未显示的条目）"));
}

TSharedPtr<FJsonObject> GameTestRefResolve::CandidateToJson(const FGameTestRefCandidate& InCandidate, const TArray<FGameTestIdentItem>& InIdent, FGameTestFlowContext& InContext)
{
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("name"), DisplayName(InCandidate.Object.Get()));
	if (InCandidate.Index != INDEX_NONE)
	{
		Out->SetNumberField(TEXT("index"), InCandidate.Index);
	}
	TArray<TSharedPtr<FJsonValue>> Ident;
	for (const FGameTestIdentItem& Item : InIdent)
	{
		const FGameTestPropRead Read = GameTestPropRead::ReadText(InCandidate.Object.Get(), Item.Prop.IsEmpty() ? FString(TEXT("@:exists")) : Item.Prop, InContext, InCandidate.Item.Get());
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("prop"), Item.Prop);
		const TSharedPtr<FJsonValue> Actual = Read.IsResolved() ? Read.Value.ToJson() : nullptr;
		Entry->SetField(TEXT("actual"), Actual.IsValid() ? Actual : MakeShared<FJsonValueNull>());
		Ident.Add(MakeShared<FJsonValueObject>(Entry));
	}
	if (Ident.Num() > 0)
	{
		Out->SetArrayField(TEXT("ident"), Ident);
	}
	return Out;
}

void GameTestRefResolve::WritePayload(const FGameTestRefResult& InResult, FGameTestFlowContext& InContext)
{
	if (!InContext.StepPayload.IsValid())
	{
		return;
	}
	const TSharedRef<FJsonObject> Ref = MakeShared<FJsonObject>();
	if (!InResult.Ref.Name.IsEmpty())
	{
		Ref->SetStringField(TEXT("name"), InResult.Ref.Name);
	}
	if (!InResult.Ref.Class.IsEmpty())
	{
		Ref->SetStringField(TEXT("class"), InResult.Ref.Class);
	}
	if (InResult.IsOk())
	{
		Ref->SetStringField(TEXT("picked"), DisplayName(InResult.Get()));
		Ref->SetStringField(TEXT("by"), InResult.By);
	}
	else
	{
		Ref->SetStringField(TEXT("error"), InResult.Error);
		Ref->SetStringField(TEXT("code"), InResult.Code.ToString());
	}
	Ref->SetNumberField(TEXT("candidates"), InResult.ClassCount);
	if (InResult.IsOk() && InResult.Ref.Ident.Num() > 0)
	{
		const TSharedPtr<FJsonObject> Picked = CandidateToJson(InResult.Picked, InResult.Ref.Ident, InContext);
		const TArray<TSharedPtr<FJsonValue>>* Ident = nullptr;
		if (Picked.IsValid() && Picked->TryGetArrayField(TEXT("ident"), Ident) && Ident != nullptr)
		{
			Ref->SetArrayField(TEXT("ident"), *Ident);
		}
	}
	InContext.StepPayload->SetObjectField(TEXT("ref"), Ref);
}
