// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Sources/GameTestObjectLocator.h"

#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Core/Script/GameTestValue.h"
#include "Observe/Lua/GameTestLuaBridge.h"
#include "Observe/Sources/GameTestPathText.h"
#include "Observe/Sources/GameTestObjectRoots.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectIterator.h"

namespace
{
	/** 报歧义时最多列几个候选。 */
	constexpr int32 MaxListedCandidates = 6;
	/** in=all 时最多看多少个对象：TObjectIterator 很贵，场景写错类名时不至于卡住游戏。 */
	constexpr int32 MaxAllScan = 20000;

	const TArray<FString>& KnownFields()
	{
		static const TArray<FString> Fields = { TEXT("class"), TEXT("in"), TEXT("key"), TEXT("component"), TEXT("near"), TEXT("client") };
		return Fields;
	}

	bool IsLive(const UObject* InObject)
	{
		return InObject != nullptr && IsValid(InObject) && !InObject->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject);
	}

	/** 固定顺序：关卡里放的（从包里读出来的）在前，运行时生成的在后，同类再按名字——同样的场景每次挑到同一个。 */
	void SortStable(TArray<UObject*>& InOutObjects)
	{
		InOutObjects.Sort([](const UObject& A, const UObject& B)
		{
			const bool bPlacedA = A.HasAnyFlags(RF_WasLoaded);
			const bool bPlacedB = B.HasAnyFlags(RF_WasLoaded);
			if (bPlacedA != bPlacedB)
			{
				return bPlacedA;
			}
			return A.GetName() < B.GetName();
		});
	}

	/** `in` 是来源路径或变量时：取出值，把里面的对象摊平（单个对象、对象数组、Map 的值）。 */
	void Flatten(const FGameTestValue& InValue, TArray<UObject*>& OutObjects, int32 InDepth = 0)
	{
		if (InDepth > 2)
		{
			return;
		}
		if (UObject* Object = InValue.GetObject())
		{
			OutObjects.Add(Object);
			return;
		}
		for (const FGameTestValue& Item : InValue.GetArray())
		{
			Flatten(Item, OutObjects, InDepth + 1);
		}
		for (const TPair<FString, FGameTestValue>& Pair : InValue.GetMap())
		{
			Flatten(Pair.Value, OutObjects, InDepth + 1);
		}
	}

	bool Collect(const FGameTestObjectLocator& InLocator, FGameTestFlowContext& InContext, TArray<UObject*>& OutObjects, FString& OutError)
	{
		const FString In = InLocator.In.IsEmpty() ? TEXT("world") : InLocator.In;
		if (In.StartsWith(TEXT("@")) || In.StartsWith(TEXT("$")))
		{
			const FGameTestValue Container = In.StartsWith(TEXT("@"))
				? GameTestPathText::ResolveSourceText(In, InContext)
				: GameTestRefs::EvalJson(MakeShared<FJsonValueString>(In), InContext);
			if (Container.IsUnresolved())
			{
				OutError = Container.GetString();
				return false;
			}
			TArray<UObject*> FromSource;
			Flatten(Container, FromSource);
			for (UObject* Object : FromSource)
			{
				if (IsLive(Object) && GameTestObjectRoots::ClassMatches(Object->GetClass(), InLocator.Class))
				{
					OutObjects.Add(Object);
				}
			}
			return true;
		}
		UWorld* World = InContext.World;
		if (World == nullptr)
		{
			OutError = TEXT("没有游戏世界（游戏没开？）");
			return false;
		}
		if (In == TEXT("world"))
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (IsLive(*It) && GameTestObjectRoots::ClassMatches(It->GetClass(), InLocator.Class))
				{
					OutObjects.Add(*It);
				}
			}
			return true;
		}
		UClass* Class = GameTestObjectRoots::FindClass(InLocator.Class);
		if (In == TEXT("widgets"))
		{
			if (Class == nullptr)
			{
				// 界面类还没加载：屏上肯定没有它，算 0 个（等它出现）
				return true;
			}
			if (!Class->IsChildOf(UUserWidget::StaticClass()))
			{
				OutError = FString::Printf(TEXT("%s 不是界面（UUserWidget 子类），in=widgets 只找界面"), *InLocator.Class);
				return false;
			}
			TArray<UUserWidget*> Widgets;
			UWidgetBlueprintLibrary::GetAllWidgetsOfClass(World, Widgets, Class, /*TopLevelOnly=*/false);
			for (UUserWidget* Widget : Widgets)
			{
				if (IsLive(Widget))
				{
					OutObjects.Add(Widget);
				}
			}
			return true;
		}
		if (In == TEXT("all"))
		{
			if (Class == nullptr)
			{
				return true;
			}
			TArray<UObject*> Objects;
			GetObjectsOfClass(Class, Objects, /*bIncludeDerivedClasses=*/true);
			for (UObject* Object : Objects)
			{
				if (OutObjects.Num() >= MaxAllScan)
				{
					break;
				}
				// 只要这个世界里的（多客户端时每个世界各有一份）和不属于任何世界的（子系统、数据对象）
				const UWorld* ObjectWorld = Object != nullptr ? Object->GetWorld() : nullptr;
				if (IsLive(Object) && (ObjectWorld == nullptr || ObjectWorld == World))
				{
					OutObjects.Add(Object);
				}
			}
			return true;
		}
		OutError = FString::Printf(TEXT("in 写成了 %s：只能是 world、widgets、all 或 @来源路径"), *In);
		return false;
	}

	bool MatchesKey(UObject* InObject, const TArray<TPair<FString, FGameTestValue>>& InKey, FGameTestFlowContext& InContext)
	{
		for (const TPair<FString, FGameTestValue>& Pair : InKey)
		{
			const FGameTestValue Have = GameTestObjectLocate::ReadKey(InObject, Pair.Key, InContext);
			if (Have.IsUnresolved() || !Have.LooseEquals(Pair.Value))
			{
				return false;
			}
		}
		return true;
	}

	/** 报歧义时每个候选的标识值（「NpcId=1001、NpcId=1001」一眼看出该加哪个字段）。 */
	FString DescribeKeys(UObject* InObject, const TArray<TPair<FString, FGameTestValue>>& InKey, FGameTestFlowContext& InContext)
	{
		TArray<FString> Parts;
		for (const TPair<FString, FGameTestValue>& Pair : InKey)
		{
			Parts.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key, *GameTestObjectLocate::ReadKey(InObject, Pair.Key, InContext).ToDisplay(40)));
		}
		return FString::Printf(TEXT("%s（%s）"), *GameTestObjectRoots::DisplayName(InObject), *FString::Join(Parts, TEXT("，")));
	}

	FVector LocationOf(const UObject* InObject)
	{
		if (const AActor* Actor = Cast<AActor>(InObject))
		{
			return Actor->GetActorLocation();
		}
		if (const UActorComponent* Component = Cast<UActorComponent>(InObject))
		{
			if (const AActor* Owner = Component->GetOwner())
			{
				return Owner->GetActorLocation();
			}
		}
		return FVector::ZeroVector;
	}

	UObject* PickComponent(UObject* InObject, const FString& InComponent, FString& OutError)
	{
		AActor* Actor = Cast<AActor>(InObject);
		if (Actor == nullptr)
		{
			OutError = FString::Printf(TEXT("%s 不是 Actor，取不了组件 %s"), *InObject->GetName(), *InComponent);
			return nullptr;
		}
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (Component != nullptr && Component->GetName().Equals(InComponent, ESearchCase::IgnoreCase))
			{
				return Component;
			}
		}
		for (UActorComponent* Component : Components)
		{
			if (Component != nullptr && GameTestObjectRoots::ClassMatches(Component->GetClass(), InComponent))
			{
				return Component;
			}
		}
		OutError = FString::Printf(TEXT("%s 上没有组件 %s（按名字和类名都找过）"), *GameTestObjectRoots::DisplayName(Actor), *InComponent);
		return nullptr;
	}

	/** key 的值先求好（$var、表达式）：同一次定位里每个候选比的是同一个值。 */
	bool ResolveKey(const FGameTestObjectLocator& InLocator, FGameTestFlowContext& InContext,
		TArray<TPair<FString, FGameTestValue>>& OutKey, FString& OutError)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : InLocator.Key)
		{
			const FGameTestValue Value = GameTestRefs::EvalJson(Pair.Value, InContext);
			if (Value.IsUnresolved())
			{
				OutError = FString::Printf(TEXT("标识 %s 的值取不到：%s"), *Pair.Key, *Value.GetString());
				return false;
			}
			OutKey.Emplace(Pair.Key, Value);
		}
		return true;
	}

	/** near 的位置：[x, y, z]、向量、Actor（取它的位置）、$var、@pawn。 */
	bool ResolveNear(const TSharedPtr<FJsonValue>& InNear, FGameTestFlowContext& InContext, FVector& OutPoint, FString& OutError)
	{
		FGameTestValue Value;
		FString Text;
		if (InNear->TryGetString(Text) && Text.StartsWith(TEXT("@")))
		{
			Value = GameTestPathText::ResolveSourceText(Text, InContext);
		}
		else
		{
			Value = GameTestRefs::EvalJson(InNear, InContext);
		}
		if (Value.IsUnresolved())
		{
			OutError = Value.GetString();
			return false;
		}
		if (Value.Type == EGameTestValueType::Vector)
		{
			OutPoint = Value.GetVector();
			return true;
		}
		if (UObject* Object = Value.GetObject())
		{
			OutPoint = LocationOf(Object);
			return true;
		}
		const TArray<FGameTestValue>& Coords = Value.GetArray();
		double X = 0, Y = 0, Z = 0;
		if (Coords.Num() == 3 && Coords[0].TryNumber(X) && Coords[1].TryNumber(Y) && Coords[2].TryNumber(Z))
		{
			OutPoint = FVector(X, Y, Z);
			return true;
		}
		OutError = FString::Printf(TEXT("near 要是 [x, y, z]、向量或 Actor，现在是 %s"), *Value.ToDisplay(60));
		return false;
	}
}

bool FGameTestObjectLocator::Parse(const FJsonObject& InJson, FGameTestObjectLocator& OutLocator, TArray<FString>& OutErrors)
{
	OutLocator = FGameTestObjectLocator();
	for (const auto& Pair : InJson.Values)
	{
		const FString Name(*Pair.Key);
		if (!KnownFields().Contains(Name))
		{
			OutErrors.Add(FString::Printf(TEXT("定位器不认识字段 %s（能用：%s）"), *Name, *FString::Join(KnownFields(), TEXT("、"))));
		}
	}
	if (!InJson.TryGetStringField(TEXT("class"), OutLocator.Class) || OutLocator.Class.TrimStartAndEnd().IsEmpty())
	{
		OutErrors.Add(TEXT("定位器要写 class（类名或路径）"));
	}
	FString InText;
	if (InJson.TryGetStringField(TEXT("in"), InText) && !InText.IsEmpty())
	{
		OutLocator.In = InText;
		if (InText != TEXT("world") && InText != TEXT("widgets") && InText != TEXT("all") && !InText.StartsWith(TEXT("@")) && !InText.StartsWith(TEXT("$")))
		{
			OutErrors.Add(FString::Printf(TEXT("in 写成了 %s：只能是 world、widgets、all 或 @来源路径"), *InText));
		}
	}
	const TSharedPtr<FJsonObject>* KeyJson = nullptr;
	if (InJson.TryGetObjectField(TEXT("key"), KeyJson) && KeyJson != nullptr && (*KeyJson).IsValid())
	{
		for (const auto& Pair : (*KeyJson)->Values)
		{
			OutLocator.Key.Emplace(FString(*Pair.Key), Pair.Value);
		}
	}
	InJson.TryGetStringField(TEXT("component"), OutLocator.Component);
	const TSharedPtr<FJsonValue> NearJson = InJson.TryGetField(TEXT("near"));
	if (NearJson.IsValid() && !NearJson->IsNull())
	{
		OutLocator.Near = NearJson;
	}
	const TSharedPtr<FJsonValue> ClientJson = InJson.TryGetField(TEXT("client"));
	if (ClientJson.IsValid() && !ClientJson->IsNull())
	{
		double Number = 0;
		OutLocator.Client = ClientJson->TryGetNumber(Number) ? FString::FromInt((int32)Number) : ClientJson->AsString();
	}
	return OutErrors.Num() == 0;
}

TSharedRef<FJsonObject> FGameTestObjectLocator::ToJson() const
{
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("class"), Class);
	if (!In.IsEmpty() && In != TEXT("world"))
	{
		Out->SetStringField(TEXT("in"), In);
	}
	if (Key.Num() > 0)
	{
		const TSharedRef<FJsonObject> KeyJson = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Key)
		{
			KeyJson->SetField(Pair.Key, Pair.Value);
		}
		Out->SetObjectField(TEXT("key"), KeyJson);
	}
	if (!Component.IsEmpty())
	{
		Out->SetStringField(TEXT("component"), Component);
	}
	if (Near.IsValid())
	{
		Out->SetField(TEXT("near"), Near);
	}
	if (!Client.IsEmpty())
	{
		Out->SetStringField(TEXT("client"), Client);
	}
	return Out;
}

FString FGameTestObjectLocator::Describe() const
{
	TArray<FString> Parts;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Key)
	{
		FString Text;
		if (Pair.Value.IsValid())
		{
			Text = Pair.Value->Type == EJson::String ? Pair.Value->AsString() : FString::Printf(TEXT("%g"), Pair.Value->AsNumber());
			if (Pair.Value->Type == EJson::Boolean)
			{
				Text = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
			}
		}
		Parts.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key, *Text));
	}
	return Parts.Num() > 0 ? FString::Printf(TEXT("%s（%s）"), *Class, *FString::Join(Parts, TEXT("，"))) : Class;
}

TSharedRef<FJsonObject> FGameTestLocateResult::ToJson(const FString& InName, const FGameTestObjectLocator& InLocator) const
{
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	if (!InName.IsEmpty())
	{
		Out->SetStringField(TEXT("name"), InName);
	}
	Out->SetStringField(TEXT("class"), InLocator.Class);
	if (Picked != nullptr)
	{
		Out->SetStringField(TEXT("picked"), GameTestObjectRoots::DisplayName(Picked));
		Out->SetStringField(TEXT("path"), Picked->GetPathName());
	}
	Out->SetStringField(TEXT("by"), By);
	Out->SetNumberField(TEXT("candidates"), Candidates);
	if (bByFirst)
	{
		Out->SetBoolField(TEXT("byFirst"), true);
	}
	return Out;
}

FGameTestValue GameTestObjectLocate::ReadKey(UObject* InObject, const FString& InPath, FGameTestFlowContext& InContext)
{
	if (InObject == nullptr)
	{
		return FGameTestValue::Unresolved(TEXT("对象已经不在了"));
	}
	FString Path = InPath.TrimStartAndEnd();
	if (Path.StartsWith(TEXT("@lua."), ESearchCase::IgnoreCase))
	{
		// 只读绑的 Lua 实例表：同名时不让 C++ 属性抢先（作者写 @lua 就是要 Lua 那份）
		FGameTestLuaValue Lua;
		FString Error;
		if (!GameTestLuaBridge::Read(InObject->GetWorld(), InObject, GameTestLuaBridge::SplitPath(Path.RightChop(5)), Lua, Error))
		{
			return FGameTestValue::Unresolved(Error);
		}
		return Lua.IsNil() ? FGameTestValue::Unresolved(FString::Printf(TEXT("Lua 实例表上没有 %s"), *Path.RightChop(5)))
						   : FGameTestValue::FromJson(Lua.ToPlain());
	}
	if (Path.StartsWith(TEXT("@self."), ESearchCase::IgnoreCase))
	{
		Path.RightChopInline(6);
	}
	TArray<FGameTestPathSegment> Segments;
	FString Error;
	if (!GameTestPathText::Parse(Path, Segments, Error))
	{
		return FGameTestValue::Unresolved(Error, TEXT("expr_syntax"));
	}
	return GameTestPath::Walk(FGameTestValue::FromObject(InObject), Segments, InContext, GameTestObjectRoots::DisplayName(InObject));
}

bool GameTestObjectLocate::FindAll(const FGameTestObjectLocator& InLocator, FGameTestFlowContext& InContext, TArray<UObject*>& OutObjects, FString& OutError)
{
	OutObjects.Reset();
	TArray<TPair<FString, FGameTestValue>> Key;
	if (!ResolveKey(InLocator, InContext, Key, OutError))
	{
		return false;
	}
	TArray<UObject*> Candidates;
	if (!Collect(InLocator, InContext, Candidates, OutError))
	{
		return false;
	}
	SortStable(Candidates);
	for (UObject* Candidate : Candidates)
	{
		if (Key.Num() == 0 || MatchesKey(Candidate, Key, InContext))
		{
			OutObjects.Add(Candidate);
		}
	}
	return true;
}

EGameTestLocate GameTestObjectLocate::Locate(const FGameTestObjectLocator& InLocator, FGameTestFlowContext& InContext, FGameTestLocateResult& OutResult)
{
	OutResult = FGameTestLocateResult();
	TArray<UObject*> Hits;
	if (!FindAll(InLocator, InContext, Hits, OutResult.Message))
	{
		return EGameTestLocate::Error;
	}
	OutResult.Candidates = Hits.Num();
	if (Hits.Num() == 0)
	{
		OutResult.Message = FString::Printf(TEXT("还没找到 %s"), *InLocator.Describe());
		return EGameTestLocate::NotFound;
	}

	UObject* Picked = nullptr;
	if (Hits.Num() == 1)
	{
		Picked = Hits[0];
		OutResult.By = InLocator.Key.Num() > 0 ? TEXT("key") : TEXT("only");
	}
	else if (InLocator.Near.IsValid())
	{
		FVector Point = FVector::ZeroVector;
		if (!ResolveNear(InLocator.Near, InContext, Point, OutResult.Message))
		{
			OutResult.Message = TEXT("near 取不到位置：") + OutResult.Message;
			return EGameTestLocate::Error;
		}
		double Best = TNumericLimits<double>::Max();
		for (UObject* Hit : Hits)
		{
			const double Distance = FVector::DistSquared(LocationOf(Hit), Point);
			if (Distance < Best)
			{
				Best = Distance;
				Picked = Hit;
			}
		}
		OutResult.By = TEXT("near");
	}
	else if (InLocator.Key.Num() == 0)
	{
		// 没写标识：取第一个（顺序固定），报告里标出来，让人知道这里其实有好几个
		Picked = Hits[0];
		OutResult.bByFirst = true;
		OutResult.By = TEXT("first");
	}
	else
	{
		TArray<TPair<FString, FGameTestValue>> Key;
		FString Ignore;
		ResolveKey(InLocator, InContext, Key, Ignore);
		TArray<FString> Lines;
		for (int32 Index = 0; Index < Hits.Num() && Index < MaxListedCandidates; ++Index)
		{
			Lines.Add(DescribeKeys(Hits[Index], Key, InContext));
		}
		OutResult.Message = FString::Printf(TEXT("%s 按标识还命中 %d 个：%s%s；再加一个能区分它们的字段，或写 near"),
			*InLocator.Describe(), Hits.Num(), *FString::Join(Lines, TEXT("、")), Hits.Num() > MaxListedCandidates ? TEXT("…") : TEXT(""));
		return EGameTestLocate::Ambiguous;
	}

	if (!InLocator.Component.IsEmpty())
	{
		Picked = PickComponent(Picked, InLocator.Component, OutResult.Message);
		if (Picked == nullptr)
		{
			return EGameTestLocate::NotFound;
		}
	}
	OutResult.Picked = Picked;
	return EGameTestLocate::Found;
}

FGameTestValue GameTestObjectLocate::ResolveNamed(const FString& InName, FGameTestFlowContext& InContext)
{
	FGameTestScriptScope& Scope = InContext.GetScope();
	if (const TWeakObjectPtr<UObject>* Cached = Scope.ObjectCache.Find(InName))
	{
		// 同一次运行里找到过、对象还活着：直接用（对象被销毁后弱引用失效，往下重找）
		if (UObject* Object = Cached->Get())
		{
			return FGameTestValue::FromObject(Object);
		}
	}
	const TSharedPtr<FJsonObject>* Def = Scope.ObjectDefs.Find(InName);
	if (Def == nullptr || !Def->IsValid())
	{
		TArray<FString> Names;
		Scope.ObjectDefs.GetKeys(Names);
		return FGameTestValue::Unresolved(FString::Printf(TEXT("objects 里没有 %s（有：%s）"), *InName,
			Names.Num() > 0 ? *FString::Join(Names, TEXT("、")) : TEXT("没有")), TEXT("var_undefined"));
	}
	FGameTestObjectLocator Locator;
	TArray<FString> Errors;
	if (!FGameTestObjectLocator::Parse(**Def, Locator, Errors))
	{
		return FGameTestValue::Unresolved(FString::Printf(TEXT("objects.%s 写得不对：%s"), *InName, *FString::Join(Errors, TEXT("；"))), TEXT("bad_type"));
	}
	FGameTestLocateResult Result;
	switch (Locate(Locator, InContext, Result))
	{
	case EGameTestLocate::Found:
		Scope.ObjectCache.Add(InName, Result.Picked);
		if (InContext.StepPayload.IsValid())
		{
			InContext.StepPayload->SetObjectField(TEXT("object"), Result.ToJson(InName, Locator));
		}
		return FGameTestValue::FromObject(Result.Picked);
	case EGameTestLocate::Ambiguous:
		return FGameTestValue::Unresolved(Result.Message, TEXT("object_ambiguous"));
	case EGameTestLocate::NotFound:
		return FGameTestValue::Unresolved(Result.Message, TEXT("object_not_found"));
	default:
		return FGameTestValue::Unresolved(Result.Message, TEXT("bad_type"));
	}
}
