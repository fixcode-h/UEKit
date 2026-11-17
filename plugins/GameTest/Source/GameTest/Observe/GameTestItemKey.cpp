// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestItemKey.h"
#include "GameTestCompat.h"

#include "Observe/GameTestItemMatch.h"
#include "Observe/GameTestSlate.h"
#include "Observe/GameTestSnapshot.h"

#include "AgentPanelLua.h"
#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/UserWidget.h"
#include "Components/DynamicEntryBox.h"
#include "Components/GridSlot.h"
#include "Components/ListView.h"
#include "Components/PanelWidget.h"
#include "Components/UniformGridSlot.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/Package.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace GameTestItemKeyDetail
{
	/** 结构体最多展开几层：Data.Info.Id 够用，再深多半是引擎结构。 */
	constexpr int32 MaxStructDepth = 3;
	/** 组合候选只拿前几个不唯一的字段两两试，避免 n² 爆掉。 */
	constexpr int32 ComboPool = 6;
	/** 单个来源拿去算唯一性的字段数：每个字段要在全部条目上读一遍。 */
	constexpr int32 CandidateLeavesPerSource = 120;

	FString NormalizeSource(const FString& InSource)
	{
		const FString Lower = InSource.ToLower();
		if (Lower == TEXT("itemlua"))
		{
			return TEXT("itemLua");
		}
		return Lower;
	}

	bool IsKnownSource(const FString& InSource)
	{
		return InSource == TEXT("item") || InSource == TEXT("self") || InSource == TEXT("lua") || InSource == TEXT("itemLua")
			|| InSource == TEXT("text") || InSource == TEXT("row") || InSource == TEXT("col");
	}

	bool SourceHasPath(const FString& InSource)
	{
		return InSource != TEXT("text") && InSource != TEXT("row") && InSource != TEXT("col");
	}

	/** 引擎和 UMG 自己的属性不要：UUserWidget 上百个属性只会盖住项目里真正的数据字段。 */
	bool IsEngineOwner(const UStruct* InOwner)
	{
		if (InOwner == nullptr)
		{
			return true;
		}
		const FString Package = InOwner->GetOutermost()->GetName();
		static const TCHAR* const EnginePackages[] = {
			TEXT("/Script/CoreUObject"), TEXT("/Script/Engine"), TEXT("/Script/UMG"), TEXT("/Script/CommonUI"),
			TEXT("/Script/Slate"), TEXT("/Script/SlateCore"), TEXT("/Script/InputCore"),
		};
		for (const TCHAR* Engine : EnginePackages)
		{
			if (Package.Equals(Engine, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	FString FieldName(const FProperty& InProperty)
	{
		// 蓝图结构体成员的 GetName 带 GUID 后缀，给人看和写进选择器都用声明时的名字
		return InProperty.GetAuthoredName();
	}

	TSharedPtr<FJsonValue> PropertyValue(const FProperty* InProperty, const void* InValue, int32 InDepth);

	TSharedPtr<FJsonObject> StructValue(const UStruct* InStruct, const void* InData, int32 InDepth)
	{
		const TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(InStruct); It; ++It)
		{
			const FProperty* Property = *It;
			if (Property == nullptr || Property->HasAnyPropertyFlags(CPF_Deprecated))
			{
				continue;
			}
			if (const TSharedPtr<FJsonValue> Value = PropertyValue(Property, Property->ContainerPtrToValuePtr<void>(InData), InDepth))
			{
				Out->SetField(FieldName(*Property), Value);
			}
		}
		return Out;
	}

	/** 一个属性值 → JSON：能当标识的标量和结构体；对象引用、数组、Map、委托都不要。 */
	TSharedPtr<FJsonValue> PropertyValue(const FProperty* InProperty, const void* InValue, int32 InDepth)
	{
		if (InProperty == nullptr || InValue == nullptr || InProperty->ArrayDim != 1)
		{
			return nullptr;
		}
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(InProperty))
		{
			return MakeShared<FJsonValueBoolean>(Bool->GetPropertyValue(InValue));
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(InProperty))
		{
			const FNumericProperty* Underlying = EnumProperty->GetUnderlyingProperty();
			const UEnum* Enum = EnumProperty->GetEnum();
			if (Underlying == nullptr || Enum == nullptr)
			{
				return nullptr;
			}
			const int64 Raw = Underlying->GetSignedIntPropertyValue(InValue);
			return MakeShared<FJsonValueString>(Enum->GetNameStringByValue(Raw));
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(InProperty))
		{
			if (const UEnum* Enum = Numeric->GetIntPropertyEnum())
			{
				return MakeShared<FJsonValueString>(Enum->GetNameStringByValue(Numeric->GetSignedIntPropertyValue(InValue)));
			}
			if (Numeric->IsInteger())
			{
				return MakeShared<FJsonValueNumber>(static_cast<double>(Numeric->GetSignedIntPropertyValue(InValue)));
			}
			return MakeShared<FJsonValueNumber>(Numeric->GetFloatingPointPropertyValue(InValue));
		}
		if (const FStrProperty* Str = CastField<FStrProperty>(InProperty))
		{
			return MakeShared<FJsonValueString>(Str->GetPropertyValue(InValue));
		}
		if (const FNameProperty* Name = CastField<FNameProperty>(InProperty))
		{
			return MakeShared<FJsonValueString>(Name->GetPropertyValue(InValue).ToString());
		}
		if (const FTextProperty* Text = CastField<FTextProperty>(InProperty))
		{
			return MakeShared<FJsonValueString>(Text->GetPropertyValue(InValue).ToString());
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(InProperty))
		{
			if (Struct->Struct == nullptr)
			{
				return nullptr;
			}
			if (Struct->Struct == TBaseStructure<FGuid>::Get())
			{
				return MakeShared<FJsonValueString>(static_cast<const FGuid*>(InValue)->ToString());
			}
			if (InDepth >= MaxStructDepth)
			{
				return nullptr;
			}
			return MakeShared<FJsonValueObject>(StructValue(Struct->Struct, InValue, InDepth + 1));
		}
		return nullptr;
	}

	bool IsScalar(const TSharedPtr<FJsonValue>& InValue)
	{
		return InValue.IsValid() && (InValue->Type == EJson::String || InValue->Type == EJson::Number || InValue->Type == EJson::Boolean);
	}

	/** 路径最后一段，小写：打分看名字。 */
	FString LastSegmentLower(const FString& InPath)
	{
		int32 Dot = INDEX_NONE;
		return (InPath.FindLastChar(TEXT('.'), Dot) ? InPath.RightChop(Dot + 1) : InPath).ToLower();
	}

	bool ContainsAny(const FString& InLower, std::initializer_list<const TCHAR*> InWords)
	{
		for (const TCHAR* Word : InWords)
		{
			if (InLower.Contains(Word))
			{
				return true;
			}
		}
		return false;
	}

	bool EqualsIndex(const FJsonValue& InValue, int32 InIndex)
	{
		if (InValue.Type != EJson::Number)
		{
			return false;
		}
		const double Number = InValue.AsNumber();
		return Number == static_cast<double>(InIndex) || Number == static_cast<double>(InIndex + 1);
	}

	/** 一个来源里条目自己的「路径 → 值」。 */
	void SourceLeaves(const FGameTestKeyEntry& InEntry, bool bInList, const FString& InSource, FGameTestKeyCache& InOutCache,
		int32 InMax, TArray<TPair<FString, TSharedPtr<FJsonValue>>>& OutLeaves)
	{
		OutLeaves.Reset();
		UObject* Item = InEntry.Item.Get();
		UWidget* Widget = InEntry.Widget.Get();
		TSharedPtr<FJsonObject> Object;
		if (InSource == TEXT("item"))
		{
			Object = GameTestItemKey::DumpFields(Item, InOutCache);
		}
		else if (InSource == TEXT("itemLua"))
		{
			Object = GameTestItemKey::DumpLuaFields(Item, InOutCache);
		}
		else if (InSource == TEXT("self"))
		{
			Object = GameTestItemKey::DumpFields(Widget, InOutCache);
		}
		else if (InSource == TEXT("lua"))
		{
			Object = GameTestItemKey::DumpLuaFields(Widget, InOutCache);
		}
		else
		{
			const TSharedPtr<FJsonValue> Value = GameTestItemKey::ReadValue(InEntry, bInList, InSource, FString(), InOutCache);
			if (IsScalar(Value))
			{
				OutLeaves.Emplace(FString(), Value);
			}
			return;
		}
		if (Object.IsValid())
		{
			GameTestItemKey::Flatten(*Object, MaxStructDepth + 1, InMax, OutLeaves);
		}
	}

	/** 这个容器有哪些来源（录制挑候选用）。 */
	TArray<FString> CandidateSources(bool bInList)
	{
		if (bInList)
		{
			// ListView 的条目控件滚动时复用，控件上的字段跟着数据换，不能拿来认（设计稿第 2 节）
			return { TEXT("item"), TEXT("itemLua") };
		}
		return { TEXT("self"), TEXT("lua"), TEXT("text"), TEXT("row"), TEXT("col") };
	}

	int32 CountHits(const TArray<FGameTestKeyEntry>& InEntries, bool bInList, const FString& InSource, const FString& InPath,
		const FJsonValue& InValue, FGameTestKeyCache& InOutCache)
	{
		int32 Hits = 0;
		for (const FGameTestKeyEntry& Entry : InEntries)
		{
			const TSharedPtr<FJsonValue> Other = GameTestItemKey::ReadValue(Entry, bInList, InSource, InPath, InOutCache);
			if (Other.IsValid() && GameTestItemMatch::ValuesEqual(*Other, InValue))
			{
				++Hits;
			}
		}
		return Hits;
	}

	FString SourceTitle(const FString& InSource)
	{
		if (InSource == TEXT("item"))
		{
			return TEXT("数据对象");
		}
		if (InSource == TEXT("itemLua"))
		{
			return TEXT("数据对象的 Lua");
		}
		if (InSource == TEXT("self"))
		{
			return TEXT("条目控件");
		}
		if (InSource == TEXT("lua"))
		{
			return TEXT("条目控件的 Lua");
		}
		return TEXT("文字和位置");
	}
}

bool GameTestItemKey::ParseConds(const FString& InInner, TArray<FGameTestKeyCond>& OutConds, FString& OutError)
{
	using namespace GameTestItemKeyDetail;
	OutConds.Reset();
	const FString Inner = InInner.TrimStartAndEnd();
	if (Inner.IsNumeric())
	{
		return false;
	}

	TArray<FString> Parts;
	Inner.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty=*/true);
	for (const FString& RawPart : Parts)
	{
		FString Left;
		FString Value;
		if (!RawPart.Split(TEXT("="), &Left, &Value))
		{
			OutError = FString::Printf(TEXT("[%s] 看不懂：写 [序号] 或 [字段=值]，多个条件用逗号连"), *Inner);
			OutConds.Reset();
			return false;
		}
		Left.TrimStartAndEndInline();
		Value.TrimStartAndEndInline();

		FGameTestKeyCond Cond;
		Cond.Value = Value;
		if (Left.StartsWith(TEXT("@")))
		{
			const FString Rest = Left.RightChop(1);
			FString Source = Rest;
			FString Path;
			Rest.Split(TEXT("."), &Source, &Path);
			Cond.Source = NormalizeSource(Source.IsEmpty() ? Rest : Source);
			Cond.Path = Path;
			if (!IsKnownSource(Cond.Source))
			{
				OutError = FString::Printf(TEXT("[%s] 里的 @%s 不认识：来源只有 @self、@lua、@item、@itemLua、@text、@row、@col"), *Inner, *Source);
				OutConds.Reset();
				return false;
			}
			if (SourceHasPath(Cond.Source) == Cond.Path.IsEmpty())
			{
				OutError = SourceHasPath(Cond.Source)
					? FString::Printf(TEXT("[%s] 里的 @%s 后面要写字段名（@%s.字段=值）"), *Inner, *Cond.Source, *Cond.Source)
					: FString::Printf(TEXT("[%s] 里的 @%s 后面不写字段（@%s=值）"), *Inner, *Cond.Source, *Cond.Source);
				OutConds.Reset();
				return false;
			}
		}
		else
		{
			Cond.Path = Left;
		}
		if (SourceHasPath(Cond.Source) && Cond.Path.IsEmpty())
		{
			OutError = FString::Printf(TEXT("[%s] 里有条件没写字段名"), *Inner);
			OutConds.Reset();
			return false;
		}
		OutConds.Add(MoveTemp(Cond));
	}
	if (OutConds.Num() == 0)
	{
		OutError = FString::Printf(TEXT("[%s] 里没有条件"), *Inner);
		return false;
	}
	return true;
}

FString GameTestItemKey::FormatBy(const FString& InSource, const FString& InPath, bool bInDefaultSource)
{
	if (bInDefaultSource)
	{
		return InPath;
	}
	return InPath.IsEmpty() ? FString::Printf(TEXT("@%s"), *InSource) : FString::Printf(TEXT("@%s.%s"), *InSource, *InPath);
}

FString GameTestItemKey::FormatValue(const FJsonValue& InValue)
{
	FString Text;
	switch (InValue.Type)
	{
	case EJson::Boolean:
		return InValue.AsBool() ? TEXT("true") : TEXT("false");
	case EJson::Number:
	{
		const double Number = InValue.AsNumber();
		if (FMath::Frac(Number) == 0.0 && FMath::Abs(Number) < 9007199254740992.0)
		{
			return FString::Printf(TEXT("%lld"), static_cast<long long>(Number));
		}
		return FString::SanitizeFloat(Number);
	}
	case EJson::String:
		Text = InValue.AsString().TrimStartAndEnd();
		break;
	default:
		return FString();
	}
	// 这些字符在选择器里有含义（下一层、下一段、下一个条件），写不进去
	for (const TCHAR* Bad : { TEXT("["), TEXT("]"), TEXT(","), TEXT(">"), TEXT("/"), TEXT("=") })
	{
		if (Text.Contains(Bad))
		{
			return FString();
		}
	}
	return Text.Len() > 64 ? FString() : Text;
}

double GameTestItemKey::Score(const FString& InSource, const FString& InPath, const FJsonValue& InValue,
	bool bInUnique, bool bInEqualsIndex, bool bInChanged, bool bInLocalizedText, FString& OutWhy)
{
	using namespace GameTestItemKeyDetail;
	OutWhy.Reset();
	double Score = 0.0;

	if (InSource == TEXT("item"))
	{
		Score += 3.0;
	}
	else if (InSource == TEXT("itemLua") || InSource == TEXT("lua"))
	{
		Score += 2.0;
	}
	else if (InSource == TEXT("self"))
	{
		Score += 1.0;
	}
	else if (InSource == TEXT("row") || InSource == TEXT("col"))
	{
		Score -= 1.0;
		OutWhy = TEXT("按行列，格子布局固定时才可靠");
	}

	switch (InValue.Type)
	{
	case EJson::Number:
		if (FMath::Frac(InValue.AsNumber()) != 0.0)
		{
			OutWhy = TEXT("小数");
			return Score - 50.0;
		}
		Score += 1.0;
		break;
	case EJson::String:
		Score += InValue.AsString().Len() <= 32 ? 1.0 : -1.0;
		break;
	case EJson::Boolean:
		OutWhy = TEXT("布尔值");
		return Score - 50.0;
	default:
		return -100.0;
	}

	const FString Name = InPath.IsEmpty() ? InSource.ToLower() : LastSegmentLower(InPath);
	const bool bIdLike = Name == TEXT("id") || Name.EndsWith(TEXT("id")) || Name == TEXT("key") || Name.EndsWith(TEXT("key"))
		|| ContainsAny(Name, { TEXT("guid"), TEXT("cfg"), TEXT("config"), TEXT("tid"), TEXT("code") })
		|| Name == TEXT("name") || Name.EndsWith(TEXT("name"));
	const bool bPositionLike = bInEqualsIndex
		|| ContainsAny(Name, { TEXT("index"), TEXT("idx"), TEXT("pos"), TEXT("slot"), TEXT("order"), TEXT("sort") })
		|| Name == TEXT("row") || Name == TEXT("col") || Name == TEXT("column");
	if (bIdLike)
	{
		Score += 3.0;
	}
	if (bPositionLike)
	{
		Score -= 4.0;
		OutWhy = TEXT("像位置，顺序变了就不对");
	}
	if (ContainsAny(Name, { TEXT("guid"), TEXT("instanceid"), TEXT("uid") }) && !Name.Contains(TEXT("cfg")))
	{
		Score -= 1.0;
		if (OutWhy.IsEmpty())
		{
			OutWhy = TEXT("可能每局不同（实例编号），配置 Id 更稳");
		}
	}
	if (bInLocalizedText || InSource == TEXT("text"))
	{
		Score -= 2.0;
		if (OutWhy.IsEmpty())
		{
			OutWhy = TEXT("显示的文字，换语言、数量变了就不对");
		}
	}

	int32 Dots = 0;
	for (const TCHAR Char : InPath)
	{
		Dots += Char == TEXT('.') ? 1 : 0;
	}
	Score -= 0.5 * Dots;

	if (bInChanged)
	{
		OutWhy = TEXT("按下和松开时不一样，会变");
		return Score - 20.0;
	}
	if (!bInUnique)
	{
		if (OutWhy.IsEmpty())
		{
			OutWhy = TEXT("不止一个条目是这个值");
		}
		return Score - 100.0;
	}
	return Score;
}

bool GameTestItemKey::IsConfident(const TArray<FGameTestKeyCandidate>& InSorted)
{
	return InSorted.Num() > 0 && InSorted[0].Hits == 1 && !InSorted[0].bChanged && InSorted[0].Score >= ConfidentScore;
}

TSharedPtr<FJsonValue> GameTestItemKey::FindPath(const FJsonObject& InObject, const FString& InPath)
{
	TArray<FString> Segments;
	InPath.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty=*/true);
	const FJsonObject* Current = &InObject;
	TSharedPtr<FJsonValue> Value;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		if (Current == nullptr)
		{
			return nullptr;
		}
		Value = GameTestItemMatch::FindProperty(*Current, Segments[Index]);
		if (!Value.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Child = nullptr;
		Current = Index + 1 < Segments.Num() && Value->TryGetObject(Child) && Child != nullptr && Child->IsValid() ? Child->Get() : nullptr;
	}
	return Value;
}

void GameTestItemKey::Flatten(const FJsonObject& InObject, int32 InDepth, int32 InMax, TArray<TPair<FString, TSharedPtr<FJsonValue>>>& OutLeaves)
{
	struct FFrame
	{
		const FJsonObject* Object;
		FString Prefix;
		int32 Depth;
	};
	TArray<FFrame> Stack;
	Stack.Add({ &InObject, FString(), 1 });
	// 深度优先、按出现顺序：结构体里的字段跟在结构体名后面，列表里看着有层次
	while (Stack.Num() > 0 && OutLeaves.Num() < InMax)
	{
		const FFrame Frame = Stack.Pop();
		TArray<FFrame> Children;
		for (const auto& Pair : Frame.Object->Values)
		{
			if (OutLeaves.Num() >= InMax)
			{
				break;
			}
			// 键在 5.8 是 FSharedString，先按 Compat 的写法转成 FString
			const FString Key = GAMETEST_JSON_KEY(Pair.Key);
			if (Key.StartsWith(TEXT("__")) || !Pair.Value.IsValid())
			{
				continue;
			}
			const FString Path = Frame.Prefix.IsEmpty() ? Key : Frame.Prefix + TEXT(".") + Key;
			const TSharedPtr<FJsonObject>* Child = nullptr;
			if (Pair.Value->TryGetObject(Child) && Child != nullptr && Child->IsValid())
			{
				if (Frame.Depth < InDepth)
				{
					Children.Add({ Child->Get(), Path, Frame.Depth + 1 });
				}
				continue;
			}
			if (GameTestItemKeyDetail::IsScalar(Pair.Value))
			{
				OutLeaves.Emplace(Path, Pair.Value);
			}
		}
		for (int32 Index = Children.Num() - 1; Index >= 0; --Index)
		{
			Stack.Add(Children[Index]);
		}
	}
}

TSharedPtr<FJsonObject> GameTestItemKey::CandidateToJson(const FGameTestKeyCandidate& InCandidate)
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("by"), InCandidate.By);
	Json->SetStringField(TEXT("cond"), InCandidate.Cond);
	Json->SetStringField(TEXT("src"), InCandidate.Source);
	if (InCandidate.Value.IsValid())
	{
		Json->SetField(TEXT("v"), InCandidate.Value);
	}
	Json->SetNumberField(TEXT("score"), FMath::RoundToDouble(InCandidate.Score * 10.0) / 10.0);
	Json->SetNumberField(TEXT("hits"), InCandidate.Hits);
	if (InCandidate.Hits == 1)
	{
		Json->SetBoolField(TEXT("unique"), true);
	}
	if (InCandidate.bChanged)
	{
		Json->SetBoolField(TEXT("changed"), true);
	}
	if (InCandidate.bCombo)
	{
		Json->SetBoolField(TEXT("combo"), true);
	}
	if (!InCandidate.Why.IsEmpty())
	{
		Json->SetStringField(TEXT("why"), InCandidate.Why);
	}
	return Json;
}

TSharedPtr<FJsonObject> GameTestItemKey::SiteToJson(const FGameTestKeySite& InSite, int32 InMaxCandidates)
{
	const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("from"), InSite.From);
	Json->SetStringField(TEXT("base"), InSite.Base);
	Json->SetStringField(TEXT("pos"), InSite.Positional);
	Json->SetStringField(TEXT("entry"), InSite.EntryClass);
	Json->SetNumberField(TEXT("count"), InSite.Count);
	Json->SetStringField(TEXT("by"), InSite.By);
	if (!InSite.Cond.IsEmpty())
	{
		Json->SetStringField(TEXT("cond"), InSite.Cond);
	}
	if (InSite.bList)
	{
		Json->SetBoolField(TEXT("list"), true);
	}
	TArray<TSharedPtr<FJsonValue>> Candidates;
	for (int32 Index = 0; Index < InSite.Candidates.Num() && Index < InMaxCandidates; ++Index)
	{
		Candidates.Add(MakeShared<FJsonValueObject>(CandidateToJson(InSite.Candidates[Index])));
	}
	Json->SetArrayField(TEXT("cands"), Candidates);
	return Json;
}

bool GameTestItemKey::IsListContainer(const UWidget* InContainer)
{
	return Cast<UListView>(InContainer) != nullptr;
}

bool GameTestItemKey::CollectEntries(UWidget* InContainer, TArray<FGameTestKeyEntry>& OutEntries, int32 InMax)
{
	OutEntries.Reset();
	if (InContainer == nullptr)
	{
		return false;
	}

	if (UListView* List = Cast<UListView>(InContainer))
	{
		TMap<const UObject*, UUserWidget*> Displayed;
		for (UUserWidget* Entry : List->GetDisplayedEntryWidgets())
		{
			if (Entry == nullptr)
			{
				continue;
			}
			TScriptInterface<IUserObjectListEntry> EntryInterface;
			EntryInterface.SetObject(Entry);
			if (const UObject* Item = UUserObjectListEntryLibrary::GetListItemObject(EntryInterface))
			{
				Displayed.Add(Item, Entry);
			}
		}
		const TArray<UObject*>& Items = List->GetListItems();
		for (int32 Index = 0; Index < Items.Num() && Index < InMax; ++Index)
		{
			FGameTestKeyEntry Entry;
			Entry.Item = Items[Index];
			Entry.Index = Index;
			if (UUserWidget* const* Widget = Displayed.Find(Items[Index]))
			{
				Entry.Widget = *Widget;
			}
			OutEntries.Add(MoveTemp(Entry));
		}
		return true;
	}

	TArray<UWidget*> Children;
	bool bContainer = false;
	if (UDynamicEntryBox* Box = Cast<UDynamicEntryBox>(InContainer))
	{
		bContainer = true;
		for (UUserWidget* Entry : Box->GetAllEntries())
		{
			Children.Add(Entry);
		}
	}
	else if (UPanelWidget* Panel = Cast<UPanelWidget>(InContainer))
	{
		bContainer = true;
		for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
		{
			Children.Add(Panel->GetChildAt(Index));
		}
	}
	// 没有 UMG 父面板、挂在这个控件 Slate 上的 WBP（自定义列表、Lua 挂上去的）也是它的条目
	TArray<UUserWidget*> Attached;
	GameTestSlate::CollectAttachedUserWidgets(*InContainer, Attached);
	for (UUserWidget* Widget : Attached)
	{
		Children.AddUnique(Widget);
	}
	bContainer = bContainer || Attached.Num() > 0;

	for (UWidget* Child : Children)
	{
		if (Child == nullptr || OutEntries.Num() >= InMax)
		{
			continue;
		}
		FGameTestKeyEntry Entry;
		Entry.Widget = Child;
		Entry.Index = OutEntries.Num();
		OutEntries.Add(MoveTemp(Entry));
	}
	return bContainer;
}

int32 GameTestItemKey::FindEntry(const TArray<FGameTestKeyEntry>& InEntries, const UWidget* InWidget, const UObject* InItem)
{
	for (const FGameTestKeyEntry& Entry : InEntries)
	{
		if ((InItem != nullptr && Entry.Item.Get() == InItem) || (InWidget != nullptr && Entry.Widget.Get() == InWidget))
		{
			return Entry.Index;
		}
	}
	return INDEX_NONE;
}

TSharedPtr<FJsonObject> GameTestItemKey::DumpFields(const UObject* InObject, FGameTestKeyCache& InOutCache)
{
	using namespace GameTestItemKeyDetail;
	if (!IsValid(InObject))
	{
		return nullptr;
	}
	const TWeakObjectPtr<UObject> Key(const_cast<UObject*>(InObject));
	if (const TSharedPtr<FJsonObject>* Cached = InOutCache.Fields.Find(Key))
	{
		return *Cached;
	}

	const TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	int32 Count = 0;
	for (TFieldIterator<FProperty> It(InObject->GetClass(), EFieldIteratorFlags::IncludeSuper); It && Count < MaxFieldsPerSource; ++It)
	{
		const FProperty* Property = *It;
		if (Property == nullptr || Property->HasAnyPropertyFlags(CPF_Deprecated) || IsEngineOwner(Property->GetOwnerStruct()))
		{
			continue;
		}
		if (const TSharedPtr<FJsonValue> Value = PropertyValue(Property, Property->ContainerPtrToValuePtr<void>(InObject), 0))
		{
			Out->SetField(FieldName(*Property), Value);
			++Count;
		}
	}
	InOutCache.Fields.Add(Key, Out);
	return Out;
}

TSharedPtr<FJsonObject> GameTestItemKey::DumpLuaFields(UObject* InObject, FGameTestKeyCache& InOutCache)
{
	if (!IsValid(InObject) || !FAgentPanelLua::IsCompiledIn())
	{
		return nullptr;
	}
	const TWeakObjectPtr<UObject> Key(InObject);
	if (const TSharedPtr<FJsonObject>* Cached = InOutCache.LuaFields.Find(Key))
	{
		return *Cached;
	}
	FString Ignore;
	// 没绑 Lua 是常态：结果为空也缓存，同一次解析里不反复问 Lua
	const TSharedPtr<FJsonObject> Out = FAgentPanelLua::ReadFields(InObject, /*MaxDepth=*/3, MaxFieldsPerSource, Ignore);
	InOutCache.LuaFields.Add(Key, Out);
	return Out;
}

TSharedPtr<FJsonValue> GameTestItemKey::ReadValue(const FGameTestKeyEntry& InEntry, bool bInList,
	const FString& InSource, const FString& InPath, FGameTestKeyCache& InOutCache)
{
	const FString Source = InSource.IsEmpty() ? (bInList ? TEXT("item") : TEXT("self")) : InSource;
	UObject* Item = InEntry.Item.Get();
	UWidget* Widget = InEntry.Widget.Get();

	TSharedPtr<FJsonObject> Object;
	if (Source == TEXT("item"))
	{
		Object = DumpFields(Item, InOutCache);
	}
	else if (Source == TEXT("itemLua"))
	{
		Object = DumpLuaFields(Item, InOutCache);
	}
	else if (Source == TEXT("self"))
	{
		Object = DumpFields(Widget, InOutCache);
	}
	else if (Source == TEXT("lua"))
	{
		Object = DumpLuaFields(Widget, InOutCache);
	}
	else if (Source == TEXT("text"))
	{
		const FString Text = Widget != nullptr ? GameTestSlate::CollectText(Widget->GetCachedWidget(), 40) : FString();
		return Text.IsEmpty() ? TSharedPtr<FJsonValue>() : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Text));
	}
	else if (Source == TEXT("row") || Source == TEXT("col"))
	{
		const bool bRow = Source == TEXT("row");
		UPanelSlot* Slot = Widget != nullptr ? Widget->Slot : nullptr;
		if (const UUniformGridSlot* Uniform = Cast<UUniformGridSlot>(Slot))
		{
			return MakeShared<FJsonValueNumber>(bRow ? Uniform->GetRow() : Uniform->GetColumn());
		}
		if (const UGridSlot* Grid = Cast<UGridSlot>(Slot))
		{
			return MakeShared<FJsonValueNumber>(bRow ? Grid->GetRow() : Grid->GetColumn());
		}
		return nullptr;
	}
	return Object.IsValid() && !InPath.IsEmpty() ? FindPath(*Object, InPath) : nullptr;
}

void GameTestItemKey::FindMatches(const TArray<FGameTestKeyEntry>& InEntries, bool bInList, const TArray<FGameTestKeyCond>& InConds,
	FGameTestKeyCache& InOutCache, int32 InMaxHits, TArray<int32>& OutHits)
{
	OutHits.Reset();
	for (const FGameTestKeyEntry& Entry : InEntries)
	{
		bool bAll = InConds.Num() > 0;
		for (const FGameTestKeyCond& Cond : InConds)
		{
			const TSharedPtr<FJsonValue> Value = ReadValue(Entry, bInList, Cond.Source, Cond.Path, InOutCache);
			if (!Value.IsValid() || !GameTestItemMatch::ValuesEqual(*Value, FJsonValueString(Cond.Value)))
			{
				bAll = false;
				break;
			}
		}
		if (bAll)
		{
			OutHits.Add(Entry.Index);
			if (OutHits.Num() >= InMaxHits)
			{
				return;
			}
		}
	}
}

FString GameTestItemKey::DescribeValues(const TArray<FGameTestKeyEntry>& InEntries, bool bInList, const FGameTestKeyCond& InCond,
	FGameTestKeyCache& InOutCache, int32 InLimit)
{
	TArray<FString> Values;
	int32 Missing = 0;
	for (const FGameTestKeyEntry& Entry : InEntries)
	{
		const TSharedPtr<FJsonValue> Value = ReadValue(Entry, bInList, InCond.Source, InCond.Path, InOutCache);
		if (!Value.IsValid())
		{
			++Missing;
			continue;
		}
		if (Values.Num() < InLimit)
		{
			Values.Add(Value->Type == EJson::String ? Value->AsString() : FormatValue(*Value));
		}
	}
	const FString By = FormatBy(InCond.Source, InCond.Path, InCond.Source.IsEmpty());
	if (Values.Num() == 0)
	{
		return FString::Printf(TEXT("%d 个条目上都读不到 %s（字段名写错了？Lua 字段要写 @lua. 或 @itemLua.）"), InEntries.Num(), *By);
	}
	return FString::Printf(TEXT("现有的 %s：%s%s"), *By, *FString::Join(Values, TEXT("、")),
		InEntries.Num() - Missing > InLimit ? *FString::Printf(TEXT(" …（共 %d 个）"), InEntries.Num()) : TEXT(""));
}

void GameTestItemKey::BuildCandidates(UWidget* InContainer, const TArray<FGameTestKeyEntry>& InEntries, int32 InMine,
	int32 InMax, TArray<FGameTestKeyCandidate>& OutCandidates)
{
	using namespace GameTestItemKeyDetail;
	OutCandidates.Reset();
	if (!InEntries.IsValidIndex(InMine))
	{
		return;
	}
	const bool bList = IsListContainer(InContainer);
	const FGameTestKeyEntry& Mine = InEntries[InMine];
	FGameTestKeyCache Cache;

	TArray<FGameTestKeyCandidate> All;
	for (const FString& Source : CandidateSources(bList))
	{
		TArray<TPair<FString, TSharedPtr<FJsonValue>>> Leaves;
		SourceLeaves(Mine, bList, Source, Cache, CandidateLeavesPerSource, Leaves);
		const bool bDefault = bList ? Source == TEXT("item") : Source == TEXT("self");
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Leaf : Leaves)
		{
			const FString Value = FormatValue(*Leaf.Value);
			if (Value.IsEmpty())
			{
				continue;
			}
			FGameTestKeyCandidate Candidate;
			Candidate.Source = Source;
			Candidate.By = FormatBy(Source, Leaf.Key, bDefault);
			Candidate.Value = Leaf.Value;
			Candidate.Cond = Candidate.By + TEXT("=") + Value;
			Candidate.Hits = CountHits(InEntries, bList, Source, Leaf.Key, *Leaf.Value, Cache);
			Candidate.bEqualsIndex = EqualsIndex(*Leaf.Value, Mine.Index);
			Candidate.Score = Score(Source, Leaf.Key, *Leaf.Value, Candidate.Hits == 1, Candidate.bEqualsIndex, false, false, Candidate.Why);
			All.Add(MoveTemp(Candidate));
		}
	}
	All.Sort([](const FGameTestKeyCandidate& A, const FGameTestKeyCandidate& B) { return A.Score > B.Score; });

	// 单个字段都不唯一：前几个字段两两组合试一下（设计稿 3.C 组合）
	const bool bAnyUnique = All.ContainsByPredicate([](const FGameTestKeyCandidate& C) { return C.Hits == 1; });
	if (!bAnyUnique)
	{
		TArray<const FGameTestKeyCandidate*> Pool;
		for (const FGameTestKeyCandidate& Candidate : All)
		{
			if (Pool.Num() >= ComboPool)
			{
				break;
			}
			if (Candidate.Score > -100.0 - 10.0 && Candidate.Hits > 1 && Candidate.Why != TEXT("小数") && Candidate.Why != TEXT("布尔值"))
			{
				Pool.Add(&Candidate);
			}
		}
		TArray<FGameTestKeyCandidate> Combos;
		for (int32 A = 0; A < Pool.Num(); ++A)
		{
			for (int32 B = A + 1; B < Pool.Num(); ++B)
			{
				TArray<FGameTestKeyCond> Conds;
				FString Ignore;
				if (!ParseConds(Pool[A]->Cond + TEXT(",") + Pool[B]->Cond, Conds, Ignore))
				{
					continue;
				}
				TArray<int32> Hits;
				FindMatches(InEntries, bList, Conds, Cache, 2, Hits);
				if (Hits.Num() != 1)
				{
					continue;
				}
				FGameTestKeyCandidate Combo;
				Combo.bCombo = true;
				Combo.Source = Pool[A]->Source;
				Combo.By = Pool[A]->By + TEXT(",") + Pool[B]->By;
				Combo.Cond = Pool[A]->Cond + TEXT(",") + Pool[B]->Cond;
				Combo.Hits = 1;
				Combo.Value = MakeShared<FJsonValueString>(FString::Printf(TEXT("%s, %s"),
					*FormatValue(*Pool[A]->Value), *FormatValue(*Pool[B]->Value)));
				// 组合的分：两个字段各自当唯一算分取平均，再扣一点（字段越多越容易被数据改动打破）
				FString WhyA;
				FString WhyB;
				const double ScoreA = Score(Pool[A]->Source, Pool[A]->By, *Pool[A]->Value, true, Pool[A]->bEqualsIndex, false, false, WhyA);
				const double ScoreB = Score(Pool[B]->Source, Pool[B]->By, *Pool[B]->Value, true, Pool[B]->bEqualsIndex, false, false, WhyB);
				Combo.Score = (ScoreA + ScoreB) / 2.0 - 1.0;
				Combo.Why = TEXT("单个字段都不唯一，两个一起才唯一");
				Combos.Add(MoveTemp(Combo));
			}
		}
		All.Append(Combos);
		All.Sort([](const FGameTestKeyCandidate& A, const FGameTestKeyCandidate& B) { return A.Score > B.Score; });
	}

	for (int32 Index = 0; Index < All.Num() && Index < InMax; ++Index)
	{
		OutCandidates.Add(All[Index]);
	}
}

TSharedPtr<FJsonObject> GameTestItemKey::ListFields(UWidget* InContainer, const TArray<FGameTestKeyEntry>& InEntries, int32 InMine,
	const FString& InFilter, int32 InOffset, int32 InLimit)
{
	using namespace GameTestItemKeyDetail;
	const TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	if (!InEntries.IsValidIndex(InMine))
	{
		return Out;
	}
	const bool bList = IsListContainer(InContainer);
	const FGameTestKeyEntry& Mine = InEntries[InMine];
	FGameTestKeyCache Cache;

	// 全部字段：ListView 也列条目控件（标「滚动时复用」，唯一性只按显示出来的算），让人看清数据在哪
	const TArray<FString> Sources = bList
		? TArray<FString>{ TEXT("item"), TEXT("itemLua"), TEXT("self"), TEXT("lua") }
		: TArray<FString>{ TEXT("self"), TEXT("lua"), TEXT("text"), TEXT("row"), TEXT("col") };

	int32 Total = 0;
	int32 Emitted = 0;
	int32 Chars = 0;
	bool bTruncated = false;
	TArray<TSharedPtr<FJsonValue>> Groups;
	TMap<FString, TSharedPtr<FJsonObject>> GroupByTitle;
	for (const FString& Source : Sources)
	{
		TArray<TPair<FString, TSharedPtr<FJsonValue>>> Leaves;
		SourceLeaves(Mine, bList, Source, Cache, MaxFieldsPerSource, Leaves);
		const bool bDefault = bList ? Source == TEXT("item") : Source == TEXT("self");
		const bool bReused = bList && (Source == TEXT("self") || Source == TEXT("lua"));
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Leaf : Leaves)
		{
			const FString By = FormatBy(Source, Leaf.Key, bDefault);
			const FString Shown = Leaf.Value->Type == EJson::String ? Leaf.Value->AsString() : FormatValue(*Leaf.Value);
			if (!InFilter.IsEmpty() && !By.Contains(InFilter, ESearchCase::IgnoreCase) && !Shown.Contains(InFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
			++Total;
			if (Total <= InOffset || Emitted >= InLimit || bTruncated)
			{
				continue;
			}
			// 读结构的返回最多 12000 字（ADR 0067），超了标 truncated，下一页接着翻
			if (Chars > 11000 && Emitted > 0)
			{
				bTruncated = true;
				continue;
			}

			const FString Title = SourceTitle(Source);
			TSharedPtr<FJsonObject>& Group = GroupByTitle.FindOrAdd(Title);
			if (!Group.IsValid())
			{
				Group = MakeShared<FJsonObject>();
				Group->SetStringField(TEXT("source"), Source);
				Group->SetStringField(TEXT("title"), Title);
				const UObject* Owner = Source.StartsWith(TEXT("item")) ? Mine.Item.Get() : static_cast<const UObject*>(Mine.Widget.Get());
				if (Owner != nullptr)
				{
					Group->SetStringField(TEXT("class"), GameTestSnapshot::FriendlyClassName(Owner->GetClass()));
				}
				if (bReused)
				{
					Group->SetStringField(TEXT("note"), TEXT("滚动时复用，只按显示出来的条目算"));
				}
				Group->SetArrayField(TEXT("fields"), TArray<TSharedPtr<FJsonValue>>());
				Groups.Add(MakeShared<FJsonValueObject>(Group));
			}

			const TSharedPtr<FJsonObject> Field = MakeShared<FJsonObject>();
			Field->SetStringField(TEXT("by"), By);
			Field->SetStringField(TEXT("src"), Source);
			Field->SetField(TEXT("v"), Leaf.Value);
			const int32 Hits = CountHits(InEntries, bList, Source, Leaf.Key, *Leaf.Value, Cache);
			Field->SetNumberField(TEXT("hits"), Hits);
			if (Hits == 1)
			{
				Field->SetBoolField(TEXT("unique"), true);
			}
			const FString Value = FormatValue(*Leaf.Value);
			if (!Value.IsEmpty())
			{
				Field->SetStringField(TEXT("cond"), By + TEXT("=") + Value);
			}
			FString Why;
			Score(Source, Leaf.Key, *Leaf.Value, Hits == 1, EqualsIndex(*Leaf.Value, Mine.Index), false, false, Why);
			if (!Why.IsEmpty())
			{
				Field->SetStringField(TEXT("why"), Why);
			}
			TArray<TSharedPtr<FJsonValue>> Fields = Group->GetArrayField(TEXT("fields"));
			Fields.Add(MakeShared<FJsonValueObject>(Field));
			Group->SetArrayField(TEXT("fields"), Fields);
			Chars += By.Len() + Shown.Len() + 40;
			++Emitted;
		}
	}
	Out->SetArrayField(TEXT("groups"), Groups);
	Out->SetNumberField(TEXT("total"), Total);
	Out->SetNumberField(TEXT("count"), Emitted);
	Out->SetNumberField(TEXT("offset"), InOffset);
	const bool bMore = InOffset + Emitted < Total;
	Out->SetBoolField(TEXT("hasMore"), bMore);
	if (bMore)
	{
		Out->SetNumberField(TEXT("nextOffset"), InOffset + Emitted);
	}
	if (bTruncated)
	{
		Out->SetBoolField(TEXT("truncated"), true);
	}
	if (FAgentPanelLua::IsCompiledIn() == false)
	{
		Out->SetStringField(TEXT("luaNote"), TEXT("这个项目没有 Lua（UELink 编译时没找到 UnLua）"));
	}
	return Out;
}

void GameTestItemKey::Recheck(UWidget* InContainer, const FGameTestKeyEntry& InEntry, TArray<FGameTestKeyCandidate>& InOutCandidates)
{
	const bool bList = IsListContainer(InContainer);
	FGameTestKeyCache Cache;
	for (FGameTestKeyCandidate& Candidate : InOutCandidates)
	{
		if (Candidate.bCombo || !Candidate.Value.IsValid())
		{
			continue;
		}
		TArray<FGameTestKeyCond> Conds;
		FString Ignore;
		if (!ParseConds(Candidate.Cond, Conds, Ignore) || Conds.Num() != 1)
		{
			continue;
		}
		const TSharedPtr<FJsonValue> Now = ReadValue(InEntry, bList, Conds[0].Source, Conds[0].Path, Cache);
		// 条目没了（点完被移走）读不到不算变：没法判断
		if (!Now.IsValid() && !InEntry.Widget.IsValid() && !InEntry.Item.IsValid())
		{
			continue;
		}
		if (Now.IsValid() && GameTestItemMatch::ValuesEqual(*Now, *Candidate.Value))
		{
			continue;
		}
		Candidate.bChanged = true;
		const FString Path = Conds[0].Path;
		Candidate.Score = Score(Candidate.Source, Path, *Candidate.Value, Candidate.Hits == 1, Candidate.bEqualsIndex, true, false, Candidate.Why);
	}
	InOutCandidates.Sort([](const FGameTestKeyCandidate& A, const FGameTestKeyCandidate& B) { return A.Score > B.Score; });
}
