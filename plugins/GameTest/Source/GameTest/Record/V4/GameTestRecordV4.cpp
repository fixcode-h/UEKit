// Copyright Epic Games, Inc. All Rights Reserved.

#include "Record/V4/GameTestRecordV4.h"

#include "GameTestCompat.h"
#include "Observe/GameTestItemKey.h"
#include "Observe/GameTestSelector.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace GameTestRecordV4Impl
{
	/** 写目标的字段。 */
	const TCHAR* const TargetFields[] = { TEXT("target"), TEXT("toTarget"), TEXT("widget") };

	TSharedPtr<FJsonValue> DeepCopy(const TSharedPtr<FJsonValue>& InValue)
	{
		if (!InValue.IsValid())
		{
			return InValue;
		}
		// 套一层数组序列化再读回：JSON 值没有现成的深拷贝
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		TArray<TSharedPtr<FJsonValue>> Wrap;
		Wrap.Add(InValue);
		FJsonSerializer::Serialize(Wrap, Writer);
		TArray<TSharedPtr<FJsonValue>> Back;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Back) && Back.Num() == 1 ? Back[0] : InValue;
	}

	/** 屏名 → 类名：资源路径取最后一段（`/Game/UI/WBP_Main.WBP_Main_C` → `WBP_Main`）。 */
	FString ClassOfScreen(const FString& InScreen)
	{
		FString Name = InScreen;
		int32 Slash = INDEX_NONE;
		if (Name.FindLastChar(TEXT('/'), Slash))
		{
			Name = Name.Mid(Slash + 1);
		}
		int32 Dot = INDEX_NONE;
		if (Name.FindLastChar(TEXT('.'), Dot))
		{
			Name = Name.Mid(Dot + 1);
		}
		Name.RemoveFromEnd(TEXT("_C"));
		return Name;
	}

	/** 类名 → objects 里的名字：去掉 WBP_ / BP_ / W_ / UI_ 前缀，首字母小写（`WBP_BagSlot` → `bagSlot`）。 */
	FString NameOfClass(const FString& InClass)
	{
		FString Name = InClass;
		for (const TCHAR* Prefix : { TEXT("WBP_"), TEXT("BP_"), TEXT("W_"), TEXT("UI_"), TEXT("WB_") })
		{
			if (Name.StartsWith(Prefix, ESearchCase::IgnoreCase) && Name.Len() > FCString::Strlen(Prefix))
			{
				Name = Name.Mid(FCString::Strlen(Prefix));
				break;
			}
		}
		FString Out;
		for (const TCHAR C : Name)
		{
			if (FChar::IsAlnum(C) || C == TEXT('_'))
			{
				Out.AppendChar(C);
			}
		}
		if (Out.IsEmpty() || FChar::IsDigit(Out[0]))
		{
			Out = TEXT("ui") + Out;
		}
		Out[0] = FChar::ToLower(Out[0]);
		return Out;
	}

	/** 只有 class 的屏引用去重进 objects，返回名字。同名不同类时加序号。 */
	FString AddObject(FJsonObject& InOutObjects, const FString& InClass)
	{
		const FString Base = NameOfClass(InClass);
		for (int32 Suffix = 1; Suffix < 100; ++Suffix)
		{
			const FString Name = Suffix == 1 ? Base : FString::Printf(TEXT("%s%d"), *Base, Suffix);
			const TSharedPtr<FJsonObject>* Existing = nullptr;
			if (!InOutObjects.TryGetObjectField(Name, Existing) || Existing == nullptr || !Existing->IsValid())
			{
				const TSharedRef<FJsonObject> Def = MakeShared<FJsonObject>();
				Def->SetStringField(TEXT("class"), InClass);
				InOutObjects.SetObjectField(Name, Def);
				return Name;
			}
			FString Class;
			const bool bOnlyClass = (*Existing)->Values.Num() == 1 && (*Existing)->TryGetStringField(TEXT("class"), Class);
			if (bOnlyClass && Class.Equals(InClass, ESearchCase::IgnoreCase))
			{
				return Name;
			}
		}
		return Base;
	}

	/** 路径段 → 控件名（`#类名`、`text=…`、`*`、`..` 没有名字）。 */
	FString NameOfSegment(const FString& InSegment)
	{
		FString Name = InSegment.TrimStartAndEnd();
		Name.Split(TEXT("["), &Name, nullptr);
		if (Name.StartsWith(TEXT("#")) || Name.StartsWith(TEXT("text=")) || Name == TEXT("*") || Name == TEXT("..") || Name == TEXT("."))
		{
			return FString();
		}
		Name.Split(TEXT("#"), &Name, nullptr);
		return Name;
	}

	TSharedPtr<FJsonValue> ValueOf(const FString& InText)
	{
		if (InText.Equals(TEXT("true"), ESearchCase::IgnoreCase) || InText.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			return MakeShared<FJsonValueBoolean>(InText.Equals(TEXT("true"), ESearchCase::IgnoreCase));
		}
		if (InText.IsNumeric())
		{
			return MakeShared<FJsonValueNumber>(FCString::Atod(*InText));
		}
		FString Text = InText;
		if (Text.Len() >= 2 && (Text.StartsWith(TEXT("\"")) && Text.EndsWith(TEXT("\""))))
		{
			Text = Text.Mid(1, Text.Len() - 2);
		}
		return MakeShared<FJsonValueString>(Text);
	}

	/** 方括号里的条件 → ident。按序号的、行列号的认不了，返回 false。 */
	bool IdentOf(const FString& InInner, bool bInList, TArray<TSharedPtr<FJsonValue>>& OutIdent)
	{
		TArray<FGameTestKeyCond> Conds;
		FString Error;
		if (!GameTestItemKey::ParseConds(InInner, Conds, Error) || Conds.Num() == 0)
		{
			return false;
		}
		for (const FGameTestKeyCond& Cond : Conds)
		{
			FString Prop;
			if (Cond.Source.IsEmpty())
			{
				Prop = (bInList ? TEXT("@item.") : TEXT("@self.")) + Cond.Path;
			}
			else if (Cond.Source == TEXT("self") || Cond.Source == TEXT("lua") || Cond.Source == TEXT("item") || Cond.Source == TEXT("itemLua"))
			{
				Prop = FString::Printf(TEXT("@%s.%s"), *Cond.Source, *Cond.Path);
			}
			else if (Cond.Source == TEXT("text"))
			{
				Prop = TEXT(":text");
			}
			else
			{
				return false;
			}
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("prop"), Prop);
			Item->SetStringField(TEXT("op"), TEXT("eq"));
			Item->SetField(TEXT("value"), ValueOf(Cond.Value));
			OutIdent.Add(MakeShared<FJsonValueObject>(Item));
		}
		return true;
	}

	struct FConvert
	{
		FJsonObject& Objects;
		TArray<FString> Todos;
		bool bInline = false;
	};

	/** 旧判断字段 → 判断项（第 9 节表；录制的「看得见」按第 8 节写成 :onScreen）。没认出返回 false。 */
	bool ItemOfLeaf(const FJsonObject& InLeaf, FJsonObject& OutItem, FString& OutTodo)
	{
		bool bFlag = false;
		FString Text;
		struct FBoolMap
		{
			const TCHAR* Old;
			const TCHAR* Prop;
		};
		static const FBoolMap Bools[] = {
			{ TEXT("visible"), TEXT(":onScreen") }, { TEXT("onScreen"), TEXT(":onScreen") }, { TEXT("exists"), TEXT(":exists") },
			{ TEXT("enabled"), TEXT(":enabled") }, { TEXT("checked"), TEXT(":checked") }, { TEXT("selected"), TEXT(":selected") },
		};
		// 文字优先：文字类控件「看得见 + 文字是 X」只留文字（文字对得上就一定在屏上）
		if (InLeaf.TryGetStringField(TEXT("text"), Text))
		{
			OutItem.SetStringField(TEXT("prop"), TEXT(":text"));
			OutItem.SetStringField(TEXT("op"), TEXT("eq"));
			OutItem.SetStringField(TEXT("value"), Text);
			return true;
		}
		if (InLeaf.TryGetStringField(TEXT("textContains"), Text))
		{
			OutItem.SetStringField(TEXT("prop"), TEXT(":text"));
			OutItem.SetStringField(TEXT("op"), TEXT("contains"));
			OutItem.SetStringField(TEXT("value"), Text);
			return true;
		}
		if (InLeaf.TryGetStringField(TEXT("textMatches"), Text))
		{
			OutItem.SetStringField(TEXT("prop"), TEXT(":text"));
			OutItem.SetStringField(TEXT("op"), TEXT("matches"));
			OutItem.SetStringField(TEXT("value"), Text);
			return true;
		}
		for (const FBoolMap& Map : Bools)
		{
			if (InLeaf.TryGetBoolField(Map.Old, bFlag))
			{
				OutItem.SetStringField(TEXT("prop"), Map.Prop);
				OutItem.SetStringField(TEXT("op"), TEXT("is"));
				OutItem.SetBoolField(TEXT("value"), bFlag);
				return true;
			}
		}
		if (InLeaf.TryGetStringField(TEXT("field"), Text) && Text.StartsWith(TEXT("@")))
		{
			OutItem.SetStringField(TEXT("prop"), Text);
			const TSharedPtr<FJsonValue> Equals = InLeaf.TryGetField(TEXT("equals"));
			OutItem.SetStringField(TEXT("op"), TEXT("eq"));
			if (Equals.IsValid())
			{
				OutItem.SetField(TEXT("value"), Equals);
			}
			return true;
		}
		OutTodo = TEXT("录制的判断改写不了，请在面板里重新挑属性");
		OutItem.SetStringField(TEXT("prop"), TEXT(":exists"));
		OutItem.SetStringField(TEXT("op"), TEXT("is"));
		OutItem.SetBoolField(TEXT("value"), true);
		return false;
	}

	/** 旧判断（有 target）→ `{obj, part?, prop, op, value}`。 */
	TSharedPtr<FJsonObject> LeafToV4(const FJsonObject& InLeaf, const TSharedPtr<FJsonObject>& InKeyHint, FConvert& InOut)
	{
		FString Selector;
		InLeaf.TryGetStringField(TEXT("target"), Selector);
		FString Todo;
		const TSharedPtr<FJsonObject> Target = GameTestRecordV4::TargetOf(Selector, InKeyHint, InOut.Objects, Todo, InOut.bInline);
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->Values = Target->Values;
		Out->RemoveField(TEXT("slate"));
		FString ItemTodo;
		ItemOfLeaf(InLeaf, *Out, ItemTodo);
		// 判断里没有 part 字段（第 4 节）：部件并进属性路径，`:text` → `@部件:text`，`@self.X` → `@部件.X`
		FString Part;
		FString Prop;
		if (Out->TryGetStringField(TEXT("part"), Part) && !Part.IsEmpty() && Out->TryGetStringField(TEXT("prop"), Prop))
		{
			if (Prop.StartsWith(TEXT(":")))
			{
				Prop = TEXT("@") + Part + Prop;
			}
			else if (Prop.StartsWith(TEXT("@self.")))
			{
				Prop = TEXT("@") + Part + Prop.Mid(5);
			}
			Out->SetStringField(TEXT("prop"), Prop);
		}
		Out->RemoveField(TEXT("part"));
		if (!Todo.IsEmpty())
		{
			InOut.Todos.Add(Todo);
		}
		if (!ItemTodo.IsEmpty())
		{
			InOut.Todos.Add(ItemTodo);
		}
		return Out;
	}

	bool IsOldLeaf(const FJsonObject& InJson)
	{
		FString Target;
		return !InJson.HasField(TEXT("action")) && InJson.TryGetStringField(TEXT("target"), Target);
	}

	TSharedPtr<FJsonObject> KeyHintOf(const FJsonObject& InStep)
	{
		const TSharedPtr<FJsonObject>* Hint = nullptr;
		const TSharedPtr<FJsonObject>* Key = nullptr;
		if (InStep.TryGetObjectField(TEXT("hint"), Hint) && Hint != nullptr && Hint->IsValid() && (*Hint)->TryGetObjectField(TEXT("key"), Key) && Key != nullptr)
		{
			return *Key;
		}
		return nullptr;
	}

	void ConvertValue(const TSharedPtr<FJsonValue>& InValue, const TSharedPtr<FJsonObject>& InKeyHint, FConvert& InOut);

	/** 一个对象里的子值（hint 不动：截图、OCR、字段缓存只用于显示）。 */
	void ConvertChildren(FJsonObject& InJson, const TSharedPtr<FJsonObject>& InKeyHint, FConvert& InOut)
	{
		TArray<FString> Keys;
		for (const auto& Pair : InJson.Values)
		{
			Keys.Add(GAMETEST_JSON_KEY(Pair.Key));
		}
		for (const FString& Key : Keys)
		{
			if (Key == TEXT("hint"))
			{
				continue;
			}
			const TSharedPtr<FJsonValue> Value = InJson.TryGetField(Key);
			const TSharedPtr<FJsonObject>* Child = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Child) && Child != nullptr && Child->IsValid() && IsOldLeaf(**Child))
			{
				// skipIf / 条件里的旧判断：skipIf 改成 {cond, also, …}，别处直接换成判断
				if (Key == TEXT("skipIf"))
				{
					const TSharedRef<FJsonObject> SkipIf = MakeShared<FJsonObject>();
					for (const TCHAR* Kept : { TEXT("also"), TEXT("settleMs"), TEXT("verify"), TEXT("verifyTimeoutMs") })
					{
						if (const TSharedPtr<FJsonValue> KeptValue = (*Child)->TryGetField(Kept))
						{
							SkipIf->SetField(Kept, KeptValue);
						}
					}
					SkipIf->SetObjectField(TEXT("cond"), LeafToV4(**Child, InKeyHint, InOut));
					InJson.SetObjectField(Key, SkipIf);
				}
				else
				{
					InJson.SetObjectField(Key, LeafToV4(**Child, InKeyHint, InOut));
				}
				continue;
			}
			ConvertValue(Value, InKeyHint, InOut);
		}
	}

	/** 一个步骤：目标字段、assert / wait 的旧判断字段。 */
	void ConvertStep(FJsonObject& InStep, FConvert& InOut)
	{
		const TSharedPtr<FJsonObject> KeyHint = KeyHintOf(InStep);
		FString Action;
		InStep.TryGetStringField(TEXT("action"), Action);
		FString Until;
		InStep.TryGetStringField(TEXT("until"), Until);
		const bool bCondStep = Action == TEXT("assert") || (Action == TEXT("wait") && (Until == TEXT("widget") || Until == TEXT("widgetGone")));
		if (bCondStep && InStep.HasField(TEXT("target")))
		{
			// assert / wait 的判断原来写在步骤上：target 和旧判断字段搬进 cond
			static const TCHAR* const LeafFields[] = { TEXT("target"), TEXT("visible"), TEXT("onScreen"), TEXT("exists"), TEXT("enabled"), TEXT("checked"),
				TEXT("selected"), TEXT("text"), TEXT("textContains"), TEXT("textMatches"), TEXT("field"), TEXT("equals") };
			const TSharedRef<FJsonObject> Leaf = MakeShared<FJsonObject>();
			for (const TCHAR* Field : LeafFields)
			{
				if (const TSharedPtr<FJsonValue> Value = InStep.TryGetField(Field))
				{
					Leaf->SetField(Field, Value);
				}
			}
			if (Until == TEXT("widget") || Until == TEXT("widgetGone"))
			{
				Leaf->SetBoolField(TEXT("exists"), Until == TEXT("widget"));
				InStep.SetStringField(TEXT("until"), TEXT("cond"));
			}
			for (const TCHAR* Field : LeafFields)
			{
				InStep.RemoveField(Field);
			}
			InStep.SetObjectField(TEXT("cond"), LeafToV4(*Leaf, KeyHint, InOut));
		}
		for (const TCHAR* Field : TargetFields)
		{
			FString Selector;
			if (InStep.TryGetStringField(Field, Selector) && !Selector.IsEmpty())
			{
				FString Todo;
				InStep.SetObjectField(Field, GameTestRecordV4::TargetOf(Selector, KeyHint, InOut.Objects, Todo));
				if (!Todo.IsEmpty())
				{
					InOut.Todos.Add(Todo);
				}
			}
		}
		ConvertChildren(InStep, KeyHint, InOut);
	}

	void ConvertValue(const TSharedPtr<FJsonValue>& InValue, const TSharedPtr<FJsonObject>& InKeyHint, FConvert& InOut)
	{
		if (!InValue.IsValid())
		{
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (InValue->TryGetArray(Array) && Array != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& Item : *Array)
			{
				ConvertValue(Item, InKeyHint, InOut);
			}
			return;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (InValue->TryGetObject(Object) && Object != nullptr && Object->IsValid())
		{
			if ((*Object)->HasField(TEXT("action")))
			{
				ConvertStep(**Object, InOut);
			}
			else
			{
				ConvertChildren(**Object, InKeyHint, InOut);
			}
		}
	}
}

TSharedPtr<FJsonObject> GameTestRecordV4::TargetOf(const FString& InSelector, const TSharedPtr<FJsonObject>& InKeyHint, FJsonObject& InOutObjects, FString& OutTodo, bool bInInline)
{
	using namespace GameTestRecordV4Impl;
	const TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
	GameTestSelector::FParsed Parsed;
	FString Error;
	if (!GameTestSelector::Parse(InSelector, Parsed, Error))
	{
		OutTodo = FString::Printf(TEXT("录到的目标改写不了（%s）"), *Error);
		const TSharedRef<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("class"), TEXT("UserWidget"));
		Ref->SetStringField(TEXT("many"), TEXT("first"));
		Target->SetObjectField(TEXT("obj"), Ref);
		return Target;
	}
	if (Parsed.bHandle)
	{
		Target->SetStringField(TEXT("obj"), TEXT("$") + Parsed.Handle);
		return Target;
	}
	if (Parsed.SlateSegments.Num() > 0)
	{
		Target->SetStringField(TEXT("slate"), FString::Join(Parsed.SlateSegments, TEXT("/")));
	}

	// 当前对象（屏 → 条目 → …）和在它里面的部件前缀（嵌套 WBP 用 `.` 连）
	TSharedPtr<FJsonValue> Obj;
	const FString ScreenClass = Parsed.bAnyScreen || Parsed.bSlateOnly ? FString() : ClassOfScreen(Parsed.Screen);
	if (ScreenClass.IsEmpty())
	{
		const TSharedRef<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("class"), TEXT("UserWidget"));
		Ref->SetStringField(TEXT("many"), TEXT("first"));
		Obj = MakeShared<FJsonValueObject>(Ref);
		OutTodo = Parsed.bSlateOnly ? TEXT("纯 Slate 控件（不在任何界面里）：确认 slate 段能找到") : TEXT("录到的目标没写界面名：把 obj 换成具体的界面类");
	}
	else if (bInInline)
	{
		const TSharedRef<FJsonObject> Ref = MakeShared<FJsonObject>();
		Ref->SetStringField(TEXT("class"), ScreenClass);
		Obj = MakeShared<FJsonValueObject>(Ref);
	}
	else
	{
		Obj = MakeShared<FJsonValueString>(AddObject(InOutObjects, ScreenClass));
	}

	bool bList = false;
	FString EntryClass;
	FString HintCond;
	if (InKeyHint.IsValid())
	{
		InKeyHint->TryGetBoolField(TEXT("list"), bList);
		InKeyHint->TryGetStringField(TEXT("entry"), EntryClass);
		InKeyHint->TryGetStringField(TEXT("cond"), HintCond);
	}

	TArray<TArray<FString>> Hops;
	Hops.Add(Parsed.Segments);
	Hops.Append(Parsed.NestedHops);
	FString Prefix;
	FString Part;
	for (int32 HopIndex = 0; HopIndex < Hops.Num(); ++HopIndex)
	{
		const TArray<FString>& Hop = Hops[HopIndex];
		FString Last;
		for (const FString& Segment : Hop)
		{
			int32 Open = INDEX_NONE;
			if (!Segment.FindChar(TEXT('['), Open) || !Segment.EndsWith(TEXT("]")))
			{
				const FString Name = NameOfSegment(Segment);
				if (!Name.IsEmpty())
				{
					Last = Name;
				}
				else if (Segment != TEXT(".") && OutTodo.IsEmpty())
				{
					OutTodo = FString::Printf(TEXT("录到的路径段 %s 没有控件名：确认 part"), *Segment);
				}
				continue;
			}
			// 容器[条件]：条目对象
			const FString Container = Prefix + NameOfSegment(Segment);
			const FString Inner = Segment.Mid(Open + 1, Segment.Len() - Open - 2);
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("class"), EntryClass.IsEmpty() ? FString(TEXT("UserWidget")) : ClassOfScreen(EntryClass));
			const TSharedRef<FJsonObject> In = MakeShared<FJsonObject>();
			In->SetField(TEXT("obj"), Obj);
			In->SetStringField(TEXT("part"), Container);
			Entry->SetObjectField(TEXT("in"), In);
			TArray<TSharedPtr<FJsonValue>> Ident;
			if (!IdentOf(Inner, bList, Ident) && !(HintCond.Len() > 0 && IdentOf(HintCond, bList, Ident)))
			{
				Entry->SetStringField(TEXT("many"), TEXT("first"));
				OutTodo = FString::Printf(TEXT("条目按第几个认（[%s]）：在属性树里挑一个不会变的标识"), *Inner);
			}
			if (Ident.Num() > 0)
			{
				Entry->SetArrayField(TEXT("ident"), Ident);
			}
			if (EntryClass.IsEmpty() && OutTodo.IsEmpty())
			{
				OutTodo = TEXT("不知道条目的类：把 class 换成条目控件的类");
			}
			Obj = MakeShared<FJsonValueObject>(Entry);
			Prefix.Reset();
			Last.Reset();
		}
		if (HopIndex + 1 < Hops.Num())
		{
			// A>B：A 是嵌套 WBP 的部件，B 在它自己的树里
			if (!Last.IsEmpty())
			{
				Prefix += Last + TEXT(".");
			}
		}
		else
		{
			Part = Prefix + Last;
		}
	}
	Part.RemoveFromEnd(TEXT("."));
	Target->SetField(TEXT("obj"), Obj);
	if (!Part.IsEmpty())
	{
		Target->SetStringField(TEXT("part"), Part);
	}
	return Target;
}

int32 GameTestRecordV4::Convert(TArray<TSharedPtr<FJsonValue>>& InOutSteps, FJsonObject& InOutObjects)
{
	using namespace GameTestRecordV4Impl;
	int32 TodoSteps = 0;
	for (TSharedPtr<FJsonValue>& Value : InOutSteps)
	{
		Value = DeepCopy(Value);
		const TSharedPtr<FJsonObject>* Step = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Step) || Step == nullptr || !Step->IsValid())
		{
			continue;
		}
		FConvert Context{ InOutObjects, {} };
		if ((*Step)->HasField(TEXT("action")))
		{
			ConvertStep(**Step, Context);
		}
		else
		{
			ConvertChildren(**Step, nullptr, Context);
		}
		if (Context.Todos.Num() > 0)
		{
			(*Step)->SetStringField(TEXT("todo"), FString::Join(Context.Todos, TEXT("；")));
			++TodoSteps;
		}
	}
	return TodoSteps;
}

TSharedPtr<FJsonObject> GameTestRecordV4::SkipIfForCheck(const FJsonObject& InOldSkipIf, const TSharedPtr<FJsonObject>& InKeyHint)
{
	using namespace GameTestRecordV4Impl;
	if (!IsOldLeaf(InOldSkipIf))
	{
		return nullptr;
	}
	FJsonObject Objects;
	FConvert Context{ Objects, {}, /*bInline=*/true };
	const TSharedPtr<FJsonObject> Cond = LeafToV4(InOldSkipIf, InKeyHint, Context);
	if (!Cond.IsValid() || Context.Todos.Num() > 0)
	{
		return nullptr;
	}
	const TSharedRef<FJsonObject> SkipIf = MakeShared<FJsonObject>();
	SkipIf->SetObjectField(TEXT("cond"), Cond);
	return SkipIf;
}
