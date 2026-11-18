// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/GameTestCondition.h"

#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestJson.h"
#include "GameTestCompat.h"
#include "Observe/GameTestItemKey.h"
#include "Observe/GameTestItemMatch.h"
#include "Observe/GameTestSlate.h"
#include "Observe/GameTestTextMatch.h"
#include "Observe/GameTestWidgetNode.h"

#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/UserWidget.h"
#include "Components/CheckBox.h"
#include "Components/Image.h"
#include "Components/ListView.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// unity build 下各文件的匿名 namespace 会合并：辅助函数放进带文件名的 namespace，不和别的文件撞名。
namespace GameTestConditionDetail
{
	const TCHAR* BoolText(bool bInValue)
	{
		return bInValue ? TEXT("true") : TEXT("false");
	}

	/** `@来源.路径` → 来源、路径；来源不认识返回 false。 */
	bool SplitField(const FString& InField, FString& OutSource, FString& OutPath)
	{
		if (!InField.StartsWith(TEXT("@")))
		{
			return false;
		}
		FString Rest = InField.RightChop(1);
		if (!Rest.Split(TEXT("."), &OutSource, &OutPath) || OutPath.IsEmpty())
		{
			return false;
		}
		return OutSource == TEXT("self") || OutSource == TEXT("lua") || OutSource == TEXT("item") || OutSource == TEXT("itemLua");
	}

	TSharedPtr<SWidget> SlateOf(const FGameTestWidgetNode& InNode)
	{
		if (const TSharedPtr<SWidget> Slate = InNode.Slate.Pin())
		{
			return Slate;
		}
		const UWidget* Widget = InNode.Widget.Get();
		return Widget != nullptr ? Widget->GetCachedWidget() : nullptr;
	}

	/** 列表条目：从控件所在的 WBP 往外找第一个实现了 IUserObjectListEntry 的，取它显示的数据对象。 */
	UObject* FindListItem(UUserWidget* InOwner)
	{
		for (UUserWidget* Current = InOwner; Current != nullptr; Current = Current->GetTypedOuter<UUserWidget>())
		{
			if (Current->GetClass()->ImplementsInterface(UUserObjectListEntry::StaticClass()))
			{
				TScriptInterface<IUserObjectListEntry> Entry;
				Entry.SetObject(Current);
				return UUserObjectListEntryLibrary::GetListItemObject(Entry);
			}
		}
		return nullptr;
	}

	/** 条件里 `equals` 的值配不上时的统一提示。 */
	void CheckEquals(const FJsonObject& InCond, const TCHAR* InWho, FGameTestConditionCheck& OutCheck, TSharedPtr<FJsonValue>& OutExpected)
	{
		OutExpected = InCond.TryGetField(TEXT("equals"));
		if (!OutExpected.IsValid())
		{
			OutCheck.Mismatches.Add(FString::Printf(TEXT("%s 需要配套的 equals（要比较的值）"), InWho));
		}
	}

	/** 没解析到目标时，除 exists 以外的条件都不成立：只记一条原因，不逐项刷屏。 */
	void CheckUnresolved(const FJsonObject& InCond, const FString& InResolveError, FGameTestConditionCheck& OutCheck)
	{
		int32 Others = 0;
		for (const auto& Pair : InCond.Values)
		{
			const FString Key = GAMETEST_JSON_KEY(Pair.Key);
			if (Key != TEXT("exists") && Key != TEXT("any") && Key != TEXT("equals") && GameTestCondition::IsKey(Key))
			{
				++Others;
			}
		}
		if (Others > 0)
		{
			OutCheck.Count += Others;
			OutCheck.Mismatches.Add(FString::Printf(TEXT("没找到目标：%s"), *InResolveError));
		}
	}

	void CheckAny(const FJsonObject& InCond, const FGameTestWidgetNode* InNode, const FString& InResolveError, FGameTestConditionCheck& OutCheck)
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = GameTestJson::TryGetArray(InCond, TEXT("any"));
		if (Items == nullptr)
		{
			return;
		}
		++OutCheck.Count;
		TArray<FString> Failed;
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			const TSharedPtr<FJsonObject>* Sub = nullptr;
			if (!Item.IsValid() || !Item->TryGetObject(Sub) || Sub == nullptr || !Sub->IsValid())
			{
				continue;
			}
			FGameTestConditionCheck SubCheck;
			GameTestCondition::Check(**Sub, InNode, InResolveError, SubCheck);
			OutCheck.Actuals.Append(SubCheck.Actuals);
			if (SubCheck.Holds())
			{
				return;
			}
			Failed.Add(FString::Join(SubCheck.Mismatches, TEXT("、")));
		}
		OutCheck.Mismatches.Add(FString::Printf(TEXT("any 里一个都不成立（%s）"), *FString::Join(Failed, TEXT("；"))));
	}

	/** 1.0 起的断言条件（visible、enabled、text、textContains、count、selected、selectedProperty、stateKey）。 */
	void CheckClassic(const FJsonObject& InCond, const FGameTestWidgetNode& InNode, FGameTestConditionCheck& OutCheck)
	{
		bool bExpectVisible = true;
		if (GameTestJson::TryGetBool(InCond, TEXT("visible"), bExpectVisible))
		{
			++OutCheck.Count;
			const bool bActual = GameTestHandlers::IsActuallyVisible(InNode);
			OutCheck.Actuals.Add(FString::Printf(TEXT("visible=%s（%s）"), BoolText(bActual), *InNode.Visibility));
			if (bActual != bExpectVisible)
			{
				OutCheck.Mismatches.Add(FString::Printf(TEXT("期望 visible=%s，实际 %s（visibility=%s）"),
					BoolText(bExpectVisible), BoolText(bActual), *InNode.Visibility));
			}
		}

		bool bExpectEnabled = true;
		if (GameTestJson::TryGetBool(InCond, TEXT("enabled"), bExpectEnabled))
		{
			++OutCheck.Count;
			OutCheck.Actuals.Add(FString::Printf(TEXT("enabled=%s"), BoolText(InNode.bEnabled)));
			if (InNode.bEnabled != bExpectEnabled)
			{
				OutCheck.Mismatches.Add(FString::Printf(TEXT("期望 enabled=%s，实际 %s"), BoolText(bExpectEnabled), BoolText(InNode.bEnabled)));
			}
		}

		FString ExpectedText;
		if (GameTestJson::TryGetString(InCond, TEXT("text"), ExpectedText))
		{
			++OutCheck.Count;
			OutCheck.Actuals.Add(FString::Printf(TEXT("text='%s'"), *InNode.Text));
			if (!InNode.Text.Equals(ExpectedText, ESearchCase::CaseSensitive))
			{
				OutCheck.Mismatches.Add(FString::Printf(TEXT("期望 text='%s'，实际 '%s'"), *ExpectedText, *InNode.Text));
			}
		}

		FString TextContains;
		if (GameTestJson::TryGetString(InCond, TEXT("textContains"), TextContains))
		{
			++OutCheck.Count;
			OutCheck.Actuals.Add(FString::Printf(TEXT("text='%s'"), *InNode.Text));
			if (!InNode.Text.Contains(TextContains, ESearchCase::IgnoreCase))
			{
				OutCheck.Mismatches.Add(FString::Printf(TEXT("期望 text 含 '%s'，实际 '%s'"), *TextContains, *InNode.Text));
			}
		}

		int32 ExpectedCount = 0;
		if (GameTestJson::TryGetInt(InCond, TEXT("count"), ExpectedCount))
		{
			++OutCheck.Count;
			const UWidget* Widget = InNode.Widget.Get();
			if (Widget == nullptr || !Widget->IsA<UListView>())
			{
				OutCheck.Mismatches.Add(TEXT("count 只能用在列表上，目标不是 ListView"));
			}
			else
			{
				OutCheck.Actuals.Add(FString::Printf(TEXT("count=%d"), InNode.Items.Num()));
				if (InNode.Items.Num() != ExpectedCount)
				{
					OutCheck.Mismatches.Add(FString::Printf(TEXT("期望 count=%d，实际 %d"), ExpectedCount, InNode.Items.Num()));
				}
			}
		}

		bool bExpectSelected = true;
		if (GameTestJson::TryGetBool(InCond, TEXT("selected"), bExpectSelected))
		{
			++OutCheck.Count;
			bool bAnySelected = false;
			for (const TSharedPtr<FGameTestListItem>& Item : InNode.Items)
			{
				if (Item.IsValid() && Item->bSelected)
				{
					bAnySelected = true;
					break;
				}
			}
			OutCheck.Actuals.Add(FString::Printf(TEXT("selected=%s"), BoolText(bAnySelected)));
			if (bAnySelected != bExpectSelected)
			{
				OutCheck.Mismatches.Add(FString::Printf(TEXT("期望 selected=%s，实际 %s"), BoolText(bExpectSelected), BoolText(bAnySelected)));
			}
		}

		FString SelectedProperty;
		if (GameTestJson::TryGetString(InCond, TEXT("selectedProperty"), SelectedProperty))
		{
			++OutCheck.Count;
			TSharedPtr<FJsonValue> ExpectedValue;
			CheckEquals(InCond, TEXT("selectedProperty"), OutCheck, ExpectedValue);
			if (ExpectedValue.IsValid())
			{
				FString ActualText = TEXT("（没有选中项）");
				bool bMatched = false;
				for (const TSharedPtr<FGameTestListItem>& Item : InNode.Items)
				{
					if (!Item.IsValid() || !Item->bSelected || !Item->Properties.IsValid())
					{
						continue;
					}
					if (const TSharedPtr<FJsonValue> Value = GameTestItemMatch::FindProperty(*Item->Properties, SelectedProperty))
					{
						ActualText = Value->AsString();
						bMatched = GameTestItemMatch::ValuesEqual(*Value, *ExpectedValue);
					}
					break;
				}
				OutCheck.Actuals.Add(FString::Printf(TEXT("selected.%s=%s"), *SelectedProperty, *ActualText));
				if (!bMatched)
				{
					OutCheck.Mismatches.Add(FString::Printf(TEXT("期望选中项的 %s=%s，实际 %s"),
						*SelectedProperty, *ExpectedValue->AsString(), *ActualText));
				}
			}
		}

		// 通用状态断言：读快照 `state` 的字段（容器类的当前值都走这条）。
		FString StateKey;
		if (GameTestJson::TryGetString(InCond, TEXT("stateKey"), StateKey))
		{
			++OutCheck.Count;
			TSharedPtr<FJsonValue> ExpectedValue;
			CheckEquals(InCond, TEXT("stateKey"), OutCheck, ExpectedValue);
			if (!ExpectedValue.IsValid())
			{
				return;
			}
			if (!InNode.State.IsValid())
			{
				OutCheck.Mismatches.Add(FString::Printf(TEXT("%s（%s）在快照里没有 state（它不是有状态控件）"), *InNode.Path, *InNode.ClassName));
			}
			else if (const TSharedPtr<FJsonValue> ActualValue = InNode.State->TryGetField(StateKey))
			{
				OutCheck.Actuals.Add(FString::Printf(TEXT("%s=%s"), *StateKey, *ActualValue->AsString()));
				if (!GameTestItemMatch::ValuesEqual(*ActualValue, *ExpectedValue))
				{
					OutCheck.Mismatches.Add(FString::Printf(TEXT("期望 state.%s=%s，实际 %s"),
						*StateKey, *ExpectedValue->AsString(), *ActualValue->AsString()));
				}
			}
			else
			{
				// 字段名写错是硬错误——把该节点实际有的字段列出来，省一次瞎猜。
				TArray<FString> Keys;
				for (const auto& Pair : InNode.State->Values)
				{
					const FString PairKey = GAMETEST_JSON_KEY(Pair.Key);
					Keys.Add(PairKey);
				}
				OutCheck.Mismatches.Add(FString::Printf(TEXT("state 里没有字段 '%s'（实际有：%s）"),
					*StateKey, Keys.Num() > 0 ? *FString::Join(Keys, TEXT("、")) : TEXT("（空）")));
			}
		}
	}

	/** 1.6.0 新加的条件（textLike、checked、field、image）。 */
	void CheckState(const FJsonObject& InCond, const FGameTestWidgetNode& InNode, FGameTestConditionCheck& OutCheck)
	{
		FString Like;
		if (GameTestJson::TryGetString(InCond, TEXT("textLike"), Like))
		{
			++OutCheck.Count;
			const FString Now = GameTestCondition::ReadText(InNode);
			OutCheck.Actuals.Add(FString::Printf(TEXT("text='%s'"), *Now));
			const GameTestTextMatch::ETier Tier = GameTestTextMatch::Compare(Like, Now);
			// 只认前两档：「已领取」包含「领取」，按包含比会把没领的当成领过了
			if (Tier != GameTestTextMatch::ETier::Exact && Tier != GameTestTextMatch::ETier::Normalized)
			{
				OutCheck.Mismatches.Add(FString::Printf(TEXT("期望文字像「%s」（忽略数字和标点），实际「%s」"), *Like, *Now));
			}
		}

		bool bExpectChecked = true;
		if (GameTestJson::TryGetBool(InCond, TEXT("checked"), bExpectChecked))
		{
			++OutCheck.Count;
			bool bChecked = false;
			if (!GameTestCondition::ReadChecked(InNode, bChecked))
			{
				OutCheck.Mismatches.Add(FString::Printf(TEXT("%s（%s）不是能勾选的控件：自己拼的勾用 visible（勾的图片）或 field（存状态的字段）"),
					*InNode.Path, *InNode.ClassName));
			}
			else
			{
				OutCheck.Actuals.Add(FString::Printf(TEXT("checked=%s"), BoolText(bChecked)));
				if (bChecked != bExpectChecked)
				{
					OutCheck.Mismatches.Add(FString::Printf(TEXT("期望 checked=%s，实际 %s"), BoolText(bExpectChecked), BoolText(bChecked)));
				}
			}
		}

		FString Field;
		if (GameTestJson::TryGetString(InCond, TEXT("field"), Field))
		{
			++OutCheck.Count;
			TSharedPtr<FJsonValue> ExpectedValue;
			CheckEquals(InCond, TEXT("field"), OutCheck, ExpectedValue);
			if (ExpectedValue.IsValid())
			{
				FString Why;
				const TSharedPtr<FJsonValue> Actual = GameTestCondition::ReadField(InNode.Widget.Get(), Field, Why);
				if (!Actual.IsValid())
				{
					OutCheck.Mismatches.Add(FString::Printf(TEXT("读不到 %s：%s"), *Field, *Why));
				}
				else
				{
					OutCheck.Actuals.Add(FString::Printf(TEXT("%s=%s"), *Field, *Actual->AsString()));
					if (!GameTestItemMatch::ValuesEqual(*Actual, *ExpectedValue))
					{
						OutCheck.Mismatches.Add(FString::Printf(TEXT("期望 %s=%s，实际 %s"), *Field, *ExpectedValue->AsString(), *Actual->AsString()));
					}
				}
			}
		}

		FString Image;
		if (GameTestJson::TryGetString(InCond, TEXT("image"), Image))
		{
			++OutCheck.Count;
			const FString Now = GameTestCondition::ReadImage(InNode.Widget.Get());
			OutCheck.Actuals.Add(FString::Printf(TEXT("image=%s"), Now.IsEmpty() ? TEXT("（无）") : *Now));
			if (Now.IsEmpty() || !Now.Contains(Image, ESearchCase::IgnoreCase))
			{
				OutCheck.Mismatches.Add(FString::Printf(TEXT("期望贴图名含 %s，实际 %s"), *Image, Now.IsEmpty() ? TEXT("（不是图片或没有贴图）") : *Now));
			}
		}
	}

	bool ValidateType(const FJsonObject& InCond, const FString& InKey, FString& OutError)
	{
		const TSharedPtr<FJsonValue> Value = InCond.TryGetField(InKey);
		if (!Value.IsValid())
		{
			return true;
		}
		static const TArray<FString> BoolKeys = { TEXT("exists"), TEXT("visible"), TEXT("enabled"), TEXT("selected"), TEXT("checked") };
		static const TArray<FString> StringKeys = { TEXT("text"), TEXT("textContains"), TEXT("textLike"), TEXT("selectedProperty"),
			TEXT("stateKey"), TEXT("field"), TEXT("image") };
		if (BoolKeys.Contains(InKey) && Value->Type != EJson::Boolean)
		{
			OutError = FString::Printf(TEXT("%s 要写 true 或 false"), *InKey);
			return false;
		}
		if (StringKeys.Contains(InKey) && (Value->Type != EJson::String || Value->AsString().IsEmpty()))
		{
			OutError = FString::Printf(TEXT("%s 要写非空字符串"), *InKey);
			return false;
		}
		if (InKey == TEXT("count") && Value->Type != EJson::Number)
		{
			OutError = TEXT("count 要写整数");
			return false;
		}
		return true;
	}
}

const TArray<FString>& GameTestCondition::Keys()
{
	static const TArray<FString> All = {
		TEXT("exists"), TEXT("visible"), TEXT("enabled"), TEXT("text"), TEXT("textContains"), TEXT("textLike"),
		TEXT("count"), TEXT("selected"), TEXT("selectedProperty"), TEXT("stateKey"), TEXT("equals"),
		TEXT("checked"), TEXT("field"), TEXT("image"), TEXT("any"),
	};
	return All;
}

bool GameTestCondition::IsKey(const FString& InKey)
{
	return Keys().Contains(InKey);
}

bool GameTestCondition::Validate(const FJsonObject& InCond, const TArray<FString>& InExtraAllowed, FString& OutError)
{
	int32 Conditions = 0;
	for (const auto& Pair : InCond.Values)
	{
		const FString Key = GAMETEST_JSON_KEY(Pair.Key);
		if (InExtraAllowed.Contains(Key))
		{
			continue;
		}
		if (!IsKey(Key))
		{
			OutError = FString::Printf(TEXT("不认识字段 '%s'（条件可用：%s）"), *Key, *FString::Join(Keys(), TEXT(", ")));
			return false;
		}
		if (!GameTestConditionDetail::ValidateType(InCond, Key, OutError))
		{
			return false;
		}
		if (Key != TEXT("equals"))
		{
			++Conditions;
		}
	}
	if (Conditions == 0)
	{
		OutError = TEXT("至少写一个条件（checked / textLike / visible / enabled / exists / field+equals / stateKey+equals / any …）");
		return false;
	}
	for (const TCHAR* NeedsEquals : { TEXT("stateKey"), TEXT("selectedProperty"), TEXT("field") })
	{
		if (InCond.HasField(NeedsEquals) && !InCond.HasField(TEXT("equals")))
		{
			OutError = FString::Printf(TEXT("%s 要配 equals（要比较的值）"), NeedsEquals);
			return false;
		}
	}
	FString Field;
	if (GameTestJson::TryGetString(InCond, TEXT("field"), Field))
	{
		FString Source;
		FString Path;
		if (!GameTestConditionDetail::SplitField(Field, Source, Path))
		{
			OutError = FString::Printf(TEXT("field '%s' 要写成 @self.路径 / @lua.路径 / @item.路径 / @itemLua.路径"), *Field);
			return false;
		}
	}
	if (InCond.HasField(TEXT("any")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = GameTestJson::TryGetArray(InCond, TEXT("any"));
		if (Items == nullptr || Items->Num() == 0)
		{
			OutError = TEXT("any 要写非空数组，每一项是一个条件对象");
			return false;
		}
		for (int32 Index = 0; Index < Items->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Sub = nullptr;
			if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Sub) || Sub == nullptr || !Sub->IsValid())
			{
				OutError = FString::Printf(TEXT("any 的第 %d 项不是对象"), Index + 1);
				return false;
			}
			FString SubError;
			if (!Validate(**Sub, TArray<FString>(), SubError))
			{
				OutError = FString::Printf(TEXT("any 的第 %d 项：%s"), Index + 1, *SubError);
				return false;
			}
		}
	}
	return true;
}

void GameTestCondition::Check(const FJsonObject& InCond, const FGameTestWidgetNode* InNode, const FString& InResolveError, FGameTestConditionCheck& OutCheck)
{
	bool bExpectExists = true;
	if (GameTestJson::TryGetBool(InCond, TEXT("exists"), bExpectExists))
	{
		++OutCheck.Count;
		const bool bExists = InNode != nullptr;
		OutCheck.Actuals.Add(FString::Printf(TEXT("exists=%s"), GameTestConditionDetail::BoolText(bExists)));
		if (bExists != bExpectExists)
		{
			OutCheck.Mismatches.Add(bExpectExists
				? FString::Printf(TEXT("期望控件存在，但没有解析到：%s"), *InResolveError)
				: FString(TEXT("期望控件不存在，但它还在")));
		}
	}

	if (InNode == nullptr)
	{
		GameTestConditionDetail::CheckUnresolved(InCond, InResolveError, OutCheck);
	}
	else
	{
		GameTestConditionDetail::CheckClassic(InCond, *InNode, OutCheck);
		GameTestConditionDetail::CheckState(InCond, *InNode, OutCheck);
	}
	GameTestConditionDetail::CheckAny(InCond, InNode, InResolveError, OutCheck);
}

bool GameTestCondition::HasNegative(const FJsonObject& InCond)
{
	bool bValue = true;
	if ((GameTestJson::TryGetBool(InCond, TEXT("exists"), bValue) && !bValue)
		|| (GameTestJson::TryGetBool(InCond, TEXT("visible"), bValue) && !bValue))
	{
		return true;
	}
	if (const TArray<TSharedPtr<FJsonValue>>* Items = GameTestJson::TryGetArray(InCond, TEXT("any")))
	{
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			const TSharedPtr<FJsonObject>* Sub = nullptr;
			if (Item.IsValid() && Item->TryGetObject(Sub) && Sub != nullptr && Sub->IsValid() && HasNegative(**Sub))
			{
				return true;
			}
		}
	}
	return false;
}

FString GameTestCondition::Describe(const FJsonObject& InCond)
{
	TArray<FString> Parts;
	for (const auto& Pair : InCond.Values)
	{
		const FString Key = GAMETEST_JSON_KEY(Pair.Key);
		if (!IsKey(Key) || Key == TEXT("equals") || !Pair.Value.IsValid())
		{
			continue;
		}
		if (Key == TEXT("textLike"))
		{
			Parts.Add(FString::Printf(TEXT("文字像「%s」"), *Pair.Value->AsString()));
		}
		else if (Key == TEXT("any"))
		{
			TArray<FString> Subs;
			if (const TArray<TSharedPtr<FJsonValue>>* Items = GameTestJson::TryGetArray(InCond, TEXT("any")))
			{
				for (const TSharedPtr<FJsonValue>& Item : *Items)
				{
					const TSharedPtr<FJsonObject>* Sub = nullptr;
					if (Item.IsValid() && Item->TryGetObject(Sub) && Sub != nullptr && Sub->IsValid())
					{
						Subs.Add(Describe(**Sub));
					}
				}
			}
			Parts.Add(FString::Printf(TEXT("（%s）"), *FString::Join(Subs, TEXT(" 或 "))));
		}
		else if (Key == TEXT("field") || Key == TEXT("stateKey") || Key == TEXT("selectedProperty"))
		{
			const TSharedPtr<FJsonValue> Equals = InCond.TryGetField(TEXT("equals"));
			Parts.Add(FString::Printf(TEXT("%s=%s"), *Pair.Value->AsString(), Equals.IsValid() ? *Equals->AsString() : TEXT("?")));
		}
		else
		{
			Parts.Add(FString::Printf(TEXT("%s=%s"), *Key, *Pair.Value->AsString()));
		}
	}
	return FString::Join(Parts, TEXT("、"));
}

bool GameTestCondition::ReadChecked(const FGameTestWidgetNode& InNode, bool& OutChecked)
{
	if (const UCheckBox* CheckBox = Cast<UCheckBox>(InNode.Widget.Get()))
	{
		OutChecked = CheckBox->IsChecked();
		return true;
	}
	if (InNode.State.IsValid())
	{
		for (const TCHAR* Key : { TEXT("checked"), TEXT("selected") })
		{
			if (InNode.State->TryGetBoolField(Key, OutChecked))
			{
				return true;
			}
		}
	}
	return false;
}

FString GameTestCondition::ReadText(const FGameTestWidgetNode& InNode)
{
	if (!InNode.Text.IsEmpty())
	{
		return InNode.Text;
	}
	return GameTestSlate::CollectTextNear(GameTestConditionDetail::SlateOf(InNode), 40);
}

FString GameTestCondition::ReadImage(const UWidget* InWidget)
{
	const UImage* Image = Cast<UImage>(InWidget);
	if (Image == nullptr)
	{
		return FString();
	}
	const UObject* Resource = Image->GetBrush().GetResourceObject();
	return Resource != nullptr ? Resource->GetName() : FString();
}

TSharedPtr<FJsonValue> GameTestCondition::ReadField(UWidget* InWidget, const FString& InField, FString& OutWhy)
{
	FString Source;
	FString Path;
	if (!GameTestConditionDetail::SplitField(InField, Source, Path))
	{
		OutWhy = TEXT("写法要是 @self.路径 / @lua.路径 / @item.路径 / @itemLua.路径");
		return nullptr;
	}
	if (InWidget == nullptr)
	{
		OutWhy = TEXT("目标是纯 Slate 控件，没有 UMG 对象可读");
		return nullptr;
	}
	UUserWidget* Owner = Cast<UUserWidget>(InWidget);
	if (Owner == nullptr)
	{
		Owner = InWidget->GetTypedOuter<UUserWidget>();
	}
	if (Owner == nullptr)
	{
		OutWhy = TEXT("目标不在任何 WBP 里");
		return nullptr;
	}

	FGameTestKeyEntry Entry;
	Entry.Widget = Owner;
	if (Source == TEXT("item") || Source == TEXT("itemLua"))
	{
		UObject* Item = GameTestConditionDetail::FindListItem(Owner);
		if (Item == nullptr)
		{
			OutWhy = TEXT("目标所在的 WBP 不是列表条目（没有数据对象）");
			return nullptr;
		}
		Entry.Item = Item;
	}
	FGameTestKeyCache Cache;
	const TSharedPtr<FJsonValue> Value = GameTestItemKey::ReadValue(Entry, /*bInList=*/false, Source, Path, Cache);
	if (!Value.IsValid())
	{
		OutWhy = FString::Printf(TEXT("%s 上没有 %s（%s）"), *Owner->GetClass()->GetName(), *Path,
			Source.Contains(TEXT("lua")) ? TEXT("没绑 Lua、或 Lua 表里没这个字段") : TEXT("不是它自己声明的字段"));
	}
	return Value;
}
