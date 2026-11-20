// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestRef.h"

#include "Core/Expr/GameTestExpr.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"
#include "Observe/Compare/GameTestCompareRegistry.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestState.h"

namespace GameTestRefImpl
{
	FString ShortJson(const TSharedPtr<FJsonValue>& InValue)
	{
		return FGameTestValue::FromJson(InValue).ToDisplay(40);
	}

	/** 字段白名单：不认的报 unknown_field；v3 的旧字段说清楚改成什么。 */
	void CheckFields(const FJsonObject& InJson, const TArray<FString>& InAllowed, const TCHAR* InWhat, FGameTestDiagSink& InSink)
	{
		for (const auto& Pair : InJson.Values)
		{
			const FString Key = GAMETEST_JSON_KEY(Pair.Key);
			if (InAllowed.Contains(Key))
			{
				continue;
			}
			if (Key == TEXT("key"))
			{
				InSink.Error(TEXT("unknown_field"), FString::Printf(TEXT("%s 的 key 在 v4 改成 ident：[{\"prop\": \"@self.字段\", \"op\": \"eq\", \"value\": 值}]"), InWhat));
			}
			else if (Key == TEXT("component"))
			{
				InSink.Error(TEXT("unknown_field"), FString::Printf(TEXT("%s 的 component 在 v4 改成另起一个组件引用：{\"class\": 组件类, \"in\": {\"obj\": Actor 的引用}}"), InWhat));
			}
			else if (Key == TEXT("target"))
			{
				InSink.Error(TEXT("unknown_field"), FString::Printf(TEXT("%s 不认 target：v4 写 obj（对象引用）和 part（部件名）"), InWhat));
			}
			else
			{
				InSink.Error(TEXT("unknown_field"), FString::Printf(TEXT("%s 不认识字段 %s（可用：%s）"), InWhat, *Key, *FString::Join(InAllowed, TEXT("、"))));
			}
		}
	}

	bool IsOneOf(const FString& InValue, std::initializer_list<const TCHAR*> InList)
	{
		for (const TCHAR* Item : InList)
		{
			if (InValue.Equals(Item, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	/** 部件名：`.` 分开的每段是标识符（设计器里起的控件名）。 */
	bool IsPartPath(const FString& InPart)
	{
		TArray<FString> Pieces;
		InPart.ParseIntoArray(Pieces, TEXT("."), /*CullEmpty=*/false);
		if (Pieces.Num() == 0)
		{
			return false;
		}
		for (const FString& Piece : Pieces)
		{
			if (Piece.IsEmpty() || Piece.Contains(TEXT("/")) || Piece.Contains(TEXT(":")) || Piece.Contains(TEXT("[")) || Piece.Contains(TEXT(">")))
			{
				return false;
			}
		}
		return true;
	}

	/** `objects` 里的名字引用的名字（in.obj 写名字时）：查环用。 */
	FString InObjName(const FJsonObject& InRef)
	{
		const TSharedPtr<FJsonObject>* In = nullptr;
		if (!InRef.TryGetObjectField(TEXT("in"), In) || In == nullptr || !In->IsValid())
		{
			return FString();
		}
		FString Name;
		TSharedPtr<FJsonObject> Inline;
		return GameTestRefForm::Classify((*In)->TryGetField(TEXT("obj")), Name, Inline) == EGameTestRefForm::Named ? Name : FString();
	}
}

//~ FGameTestIdentItem ---------------------------------------------------------

const TArray<FString>& FGameTestIdentItem::Fields()
{
	static const TArray<FString> List = { TEXT("prop"), TEXT("op"), TEXT("value"), TEXT("cmp"), TEXT("tol"), TEXT("ignoreCase") };
	return List;
}

FGameTestIdentItem FGameTestIdentItem::FromJson(const FJsonObject& InJson)
{
	FGameTestIdentItem Item;
	InJson.TryGetStringField(TEXT("prop"), Item.Prop);
	InJson.TryGetStringField(TEXT("op"), Item.Op);
	InJson.TryGetStringField(TEXT("cmp"), Item.Cmp);
	Item.Value = InJson.TryGetField(TEXT("value"));
	double Tol = 0.0;
	if (InJson.TryGetNumberField(TEXT("tol"), Tol))
	{
		Item.Tol = Tol;
	}
	bool bIgnoreCase = true;
	if (InJson.TryGetBoolField(TEXT("ignoreCase"), bIgnoreCase))
	{
		Item.IgnoreCase = bIgnoreCase;
	}
	return Item;
}

TSharedRef<FJsonObject> FGameTestIdentItem::ToJson() const
{
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("prop"), Prop);
	if (!Op.IsEmpty())
	{
		Out->SetStringField(TEXT("op"), Op);
	}
	if (Value.IsValid())
	{
		Out->SetField(TEXT("value"), Value);
	}
	if (!Cmp.IsEmpty())
	{
		Out->SetStringField(TEXT("cmp"), Cmp);
	}
	if (Tol.IsSet())
	{
		Out->SetNumberField(TEXT("tol"), Tol.GetValue());
	}
	if (IgnoreCase.IsSet())
	{
		Out->SetBoolField(TEXT("ignoreCase"), IgnoreCase.GetValue());
	}
	return Out;
}

FString FGameTestIdentItem::Describe() const
{
	const FString PropText = Prop.IsEmpty() ? FString(TEXT(":exists")) : Prop;
	const IGameTestCompare* Compare = Op.IsEmpty() ? nullptr : FGameTestCompareRegistry::Find(Op);
	const FString Verb = Compare != nullptr ? Compare->Verb() : Op.IsEmpty() ? FString(TEXT("等于")) : Op;
	if (!Value.IsValid())
	{
		return FString::Printf(TEXT("%s %s"), *PropText, Op.IsEmpty() ? TEXT("为真") : *Verb);
	}
	return FString::Printf(TEXT("%s %s %s"), *PropText, *Verb, *GameTestRefImpl::ShortJson(Value));
}

//~ FGameTestRef ---------------------------------------------------------------

const TArray<FString>& FGameTestRef::Fields()
{
	static const TArray<FString> List = { TEXT("class"), TEXT("in"), TEXT("ident"), TEXT("many"), TEXT("near"), TEXT("client") };
	return List;
}

bool FGameTestRef::FromJson(const FJsonObject& InJson, FGameTestRef& OutRef)
{
	OutRef = FGameTestRef();
	if (!InJson.TryGetStringField(TEXT("class"), OutRef.Class) || OutRef.Class.TrimStartAndEnd().IsEmpty())
	{
		return false;
	}
	OutRef.Class.TrimStartAndEndInline();
	const TSharedPtr<FJsonValue> In = InJson.TryGetField(TEXT("in"));
	FString InText;
	const TSharedPtr<FJsonObject>* InObject = nullptr;
	if (In.IsValid() && In->TryGetString(InText))
	{
		InText.TrimStartAndEndInline();
		OutRef.In = InText.Equals(TEXT("screen"), ESearchCase::IgnoreCase) ? EGameTestRefIn::Screen
			: InText.Equals(TEXT("world"), ESearchCase::IgnoreCase) ? EGameTestRefIn::World
			: InText.Equals(TEXT("all"), ESearchCase::IgnoreCase) ? EGameTestRefIn::All
			: InText.StartsWith(TEXT("@")) ? EGameTestRefIn::Source
			: EGameTestRefIn::Auto;
		OutRef.InSource = OutRef.In == EGameTestRefIn::Source ? InText : FString();
	}
	else if (In.IsValid() && In->TryGetObject(InObject) && InObject != nullptr && InObject->IsValid())
	{
		OutRef.InObj = (*InObject)->TryGetField(TEXT("obj"));
		(*InObject)->TryGetStringField(TEXT("part"), OutRef.InPart);
		OutRef.In = OutRef.InPart.IsEmpty() ? EGameTestRefIn::Owner : EGameTestRefIn::Container;
	}
	const TArray<TSharedPtr<FJsonValue>>* Ident = nullptr;
	if (InJson.TryGetArrayField(TEXT("ident"), Ident) && Ident != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Ident)
		{
			const TSharedPtr<FJsonObject>* Item = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Item) && Item != nullptr && Item->IsValid())
			{
				OutRef.Ident.Add(FGameTestIdentItem::FromJson(**Item));
			}
		}
	}
	InJson.TryGetStringField(TEXT("many"), OutRef.Many);
	const TSharedPtr<FJsonValue> Near = InJson.TryGetField(TEXT("near"));
	if (Near.IsValid() && !Near->IsNull())
	{
		OutRef.Near = Near;
	}
	const TSharedPtr<FJsonValue> Client = InJson.TryGetField(TEXT("client"));
	if (Client.IsValid() && !Client->IsNull())
	{
		double Number = 0.0;
		OutRef.Client = Client->TryGetNumber(Number) ? FString::FromInt(FMath::RoundToInt(Number)) : Client->AsString();
	}
	return true;
}

TSharedRef<FJsonObject> FGameTestRef::ToJson() const
{
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("class"), Class);
	switch (In)
	{
	case EGameTestRefIn::Screen:
		Out->SetStringField(TEXT("in"), TEXT("screen"));
		break;
	case EGameTestRefIn::World:
		Out->SetStringField(TEXT("in"), TEXT("world"));
		break;
	case EGameTestRefIn::All:
		Out->SetStringField(TEXT("in"), TEXT("all"));
		break;
	case EGameTestRefIn::Source:
		Out->SetStringField(TEXT("in"), InSource);
		break;
	case EGameTestRefIn::Container:
	case EGameTestRefIn::Owner:
	{
		const TSharedRef<FJsonObject> InJson = MakeShared<FJsonObject>();
		if (InObj.IsValid())
		{
			InJson->SetField(TEXT("obj"), InObj);
		}
		if (!InPart.IsEmpty())
		{
			InJson->SetStringField(TEXT("part"), InPart);
		}
		Out->SetObjectField(TEXT("in"), InJson);
		break;
	}
	default:
		break;
	}
	if (Ident.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FGameTestIdentItem& Item : Ident)
		{
			Items.Add(MakeShared<FJsonValueObject>(Item.ToJson()));
		}
		Out->SetArrayField(TEXT("ident"), Items);
	}
	if (!Many.IsEmpty())
	{
		Out->SetStringField(TEXT("many"), Many);
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

FString FGameTestRef::Describe() const
{
	TArray<FString> Parts;
	for (const FGameTestIdentItem& Item : Ident)
	{
		Parts.Add(Item.Describe());
	}
	const FString Head = Name.IsEmpty() ? Class : FString::Printf(TEXT("%s（%s）"), *Name, *Class);
	return Parts.Num() > 0 ? FString::Printf(TEXT("%s[%s]"), *Head, *FString::Join(Parts, TEXT("，"))) : Head;
}

//~ 写法 ------------------------------------------------------------------------

bool GameTestRefForm::IsName(const FString& InText)
{
	if (InText.IsEmpty() || InText.Len() > 64 || FChar::IsDigit(InText[0]))
	{
		return false;
	}
	for (const TCHAR C : InText)
	{
		if (!(C < 0x80 && (FChar::IsAlnum(C) || C == TEXT('_'))))
		{
			return false;
		}
	}
	return true;
}

EGameTestRefForm GameTestRefForm::Classify(const TSharedPtr<FJsonValue>& InValue, FString& OutText, TSharedPtr<FJsonObject>& OutInline)
{
	OutText.Reset();
	OutInline.Reset();
	if (!InValue.IsValid())
	{
		return EGameTestRefForm::Invalid;
	}
	FString Text;
	if (InValue->TryGetString(Text))
	{
		Text.TrimStartAndEndInline();
		if (Text.StartsWith(TEXT("$")) && IsName(Text.RightChop(1)))
		{
			OutText = Text.RightChop(1);
			return EGameTestRefForm::Var;
		}
		if (IsName(Text))
		{
			OutText = Text;
			return EGameTestRefForm::Named;
		}
		OutText = Text;
		return Text.IsEmpty() ? EGameTestRefForm::Invalid : EGameTestRefForm::Selector;
	}
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (InValue->TryGetObject(Object) && Object != nullptr && Object->IsValid() && (*Object)->HasField(TEXT("class")))
	{
		OutInline = *Object;
		return EGameTestRefForm::Inline;
	}
	return EGameTestRefForm::Invalid;
}

//~ FGameTestTarget ------------------------------------------------------------

const TArray<FString>& FGameTestTarget::Fields()
{
	static const TArray<FString> List = { TEXT("obj"), TEXT("part"), TEXT("slate") };
	return List;
}

bool FGameTestTarget::FromJson(const TSharedPtr<FJsonValue>& InValue, FGameTestTarget& OutTarget)
{
	OutTarget = FGameTestTarget();
	if (!InValue.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (InValue->TryGetObject(Object) && Object != nullptr && Object->IsValid() && (*Object)->HasField(TEXT("obj")))
	{
		OutTarget.Obj = (*Object)->TryGetField(TEXT("obj"));
		(*Object)->TryGetStringField(TEXT("part"), OutTarget.Part);
		(*Object)->TryGetStringField(TEXT("slate"), OutTarget.Slate);
		OutTarget.Part.TrimStartAndEndInline();
		return true;
	}
	FString Text;
	TSharedPtr<FJsonObject> Inline;
	const EGameTestRefForm Form = GameTestRefForm::Classify(InValue, Text, Inline);
	if (Form == EGameTestRefForm::Named || Form == EGameTestRefForm::Var || Form == EGameTestRefForm::Inline)
	{
		OutTarget.Obj = InValue;
		return true;
	}
	return false;
}

TSharedRef<FJsonObject> FGameTestTarget::ToJson() const
{
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	if (Obj.IsValid())
	{
		Out->SetField(TEXT("obj"), Obj);
	}
	if (!Part.IsEmpty())
	{
		Out->SetStringField(TEXT("part"), Part);
	}
	if (!Slate.IsEmpty())
	{
		Out->SetStringField(TEXT("slate"), Slate);
	}
	return Out;
}

FString FGameTestTarget::Describe() const
{
	FString Text;
	TSharedPtr<FJsonObject> Inline;
	FString Head;
	switch (GameTestRefForm::Classify(Obj, Text, Inline))
	{
	case EGameTestRefForm::Named:
		Head = Text;
		break;
	case EGameTestRefForm::Var:
		Head = TEXT("$") + Text;
		break;
	case EGameTestRefForm::Inline:
	{
		FGameTestRef Ref;
		Head = FGameTestRef::FromJson(*Inline, Ref) ? Ref.Describe() : FString(TEXT("（引用写错了）"));
		break;
	}
	default:
		Head = TEXT("（没写对象）");
		break;
	}
	if (!Part.IsEmpty())
	{
		Head += TEXT(".") + Part;
	}
	if (!Slate.IsEmpty())
	{
		Head += TEXT(" >> ") + Slate;
	}
	return Head;
}

//~ 写法检查 --------------------------------------------------------------------

namespace GameTestRefCheck
{
	void SelectorRemoved(const FString& InSelector, FGameTestDiagSink& InSink)
	{
		InSink.Error(TEXT("selector_removed"), FString::Printf(
			TEXT("「%s」是旧的选择器写法，v4 不认：写成 {\"obj\": 对象引用, \"part\": \"部件名\"}（对象引用是 objects 里的名字、$变量或 {\"class\": 类名, \"ident\": [...]}）；"
				 "旧脚本在面板里用「升级脚本」转换"), *InSelector.Left(80)));
	}

	void Ref(const TSharedPtr<FJsonValue>& InValue, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink)
	{
		FString Text;
		TSharedPtr<FJsonObject> Inline;
		switch (GameTestRefForm::Classify(InValue, Text, Inline))
		{
		case EGameTestRefForm::Named:
			if (InScope.KnownObjects != nullptr && !InScope.KnownObjects->Contains(Text))
			{
				InSink.Error(TEXT("ref_not_found"), FString::Printf(TEXT("objects 里没有「%s」：先在场景的 objects 里定义它，或直接写 {\"class\": \"%s\"}"), *Text, *Text));
			}
			return;
		case EGameTestRefForm::Var:
			if (InScope.KnownVars != nullptr && !InScope.KnownVars->Contains(Text))
			{
				InSink.Error(TEXT("var_undefined"), FString::Printf(TEXT("变量 $%s 还没有定义（obj.find saveAs、loop each 的 as 才会存对象）"), *Text));
			}
			return;
		case EGameTestRefForm::Inline:
			RefObject(*Inline, InScope, InSink);
			return;
		case EGameTestRefForm::Selector:
			SelectorRemoved(Text, InSink);
			return;
		default:
			InSink.Error(TEXT("bad_type"), TEXT("对象引用要写 objects 里的名字、\"$变量\" 或 {\"class\": 类名, \"ident\": [...]}"));
			return;
		}
	}

	void RefObject(const FJsonObject& InJson, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink)
	{
		using namespace GameTestRefImpl;
		CheckFields(InJson, FGameTestRef::Fields(), TEXT("对象引用"), InSink);
		FString Class;
		if (!InJson.TryGetStringField(TEXT("class"), Class) || Class.TrimStartAndEnd().IsEmpty())
		{
			InSink.Error(TEXT("ref_bad_class"), TEXT("对象引用要写 class（类名或资源路径，按继承认）"));
		}
		else if (Class.Contains(TEXT(":")) || Class.Contains(TEXT(">")))
		{
			InSink.Error(TEXT("ref_bad_class"), FString::Printf(TEXT("class 写成了「%s」：只写类名（WBP_Shop）或资源路径（/Game/UI/WBP_Shop），部件写在 part 里"), *Class));
		}
		if (const TSharedPtr<FJsonValue> In = InJson.TryGetField(TEXT("in")))
		{
			FGameTestDiagSink::FScope Scope(InSink, TEXT("in"));
			FString InText;
			const TSharedPtr<FJsonObject>* InObject = nullptr;
			if (In->TryGetString(InText))
			{
				if (InText.Equals(TEXT("widgets"), ESearchCase::IgnoreCase))
				{
					InSink.Error(TEXT("bad_type"), TEXT("in 的 widgets 在 v4 改名 screen"));
				}
				else if (!IsOneOf(InText, { TEXT("screen"), TEXT("world"), TEXT("all") }) && !InText.StartsWith(TEXT("@")))
				{
					InSink.Error(TEXT("bad_type"), FString::Printf(TEXT("in 写成了 %s：只能是 screen、world、all、@来源.路径 或 {\"obj\": 引用, \"part\": 容器}"), *InText));
				}
			}
			else if (In->TryGetObject(InObject) && InObject != nullptr && InObject->IsValid())
			{
				for (const auto& Pair : (*InObject)->Values)
				{
					const FString Key = GAMETEST_JSON_KEY(Pair.Key);
					if (Key != TEXT("obj") && Key != TEXT("part"))
					{
						InSink.Error(TEXT("unknown_field"), FString::Printf(TEXT("in 不认识字段 %s（只有 obj、part）"), *Key));
					}
				}
				if (!(*InObject)->HasField(TEXT("obj")))
				{
					InSink.Error(TEXT("bad_type"), TEXT("in 写成对象时要有 obj（外层对象的引用）"));
				}
				else
				{
					FGameTestDiagSink::FScope ObjScope(InSink, TEXT("obj"));
					Ref((*InObject)->TryGetField(TEXT("obj")), InScope, InSink);
				}
				FString Part;
				if ((*InObject)->TryGetStringField(TEXT("part"), Part) && !IsPartPath(Part))
				{
					InSink.Error(TEXT("bad_type"), FString::Printf(TEXT("part「%s」要写设计器里起的控件名，嵌套 WBP 用 . 进去"), *Part));
				}
			}
			else if (!In->IsNull())
			{
				InSink.Error(TEXT("bad_type"), TEXT("in 要写字符串（screen / world / all / @来源）或 {\"obj\", \"part\"}"));
			}
		}
		const TSharedPtr<FJsonValue> Ident = InJson.TryGetField(TEXT("ident"));
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Ident.IsValid() && !Ident->IsNull())
		{
			if (!Ident->TryGetArray(Items) || Items == nullptr)
			{
				InSink.Error(TEXT("bad_type"), TEXT("ident 要写成判断项数组 [{\"prop\", \"op\", \"value\"}]"));
			}
			else
			{
				for (int32 Index = 0; Index < Items->Num(); ++Index)
				{
					FGameTestDiagSink::FScope Scope(InSink, FString::Printf(TEXT("ident.%d"), Index));
					const TSharedPtr<FJsonObject>* Item = nullptr;
					if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Item) || Item == nullptr || !Item->IsValid())
					{
						InSink.Error(TEXT("bad_type"), TEXT("判断项要写成对象 {\"prop\", \"op\", \"value\"}"));
						continue;
					}
					GameTestRefCheck::Ident(**Item, InScope, InSink, /*bInPropRequired=*/true);
				}
			}
		}
		FString Many;
		if (InJson.TryGetStringField(TEXT("many"), Many) && !IsOneOf(Many, { TEXT("error"), TEXT("first"), TEXT("last"), TEXT("nearest") }))
		{
			InSink.Error(TEXT("bad_type"), FString::Printf(TEXT("many 写成了 %s：只能是 error、first、last、nearest"), *Many));
		}
		if (const TSharedPtr<FJsonValue> Near = InJson.TryGetField(TEXT("near")))
		{
			FGameTestDiagSink::FScope Scope(InSink, TEXT("near"));
			const TSharedPtr<FJsonObject>* NearObject = nullptr;
			if (Near->TryGetObject(NearObject) && NearObject != nullptr && NearObject->IsValid())
			{
				Ref(Near, InScope, InSink);
			}
			else if (Near->Type == EJson::String)
			{
				const FString NearText = Near->AsString();
				if (NearText.StartsWith(TEXT("$")) || NearText.StartsWith(TEXT("=")))
				{
					GameTestRefs::Validate(Near, InScope, InSink);
				}
				else if (!NearText.StartsWith(TEXT("@")) && !GameTestRefForm::IsName(NearText))
				{
					InSink.Error(TEXT("bad_type"), TEXT("near 要写 [x,y,z]、$变量、@pawn 或另一个引用"));
				}
			}
		}
	}

	void Ident(const FJsonObject& InJson, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink, bool bInPropRequired)
	{
		using namespace GameTestRefImpl;
		CheckFields(InJson, FGameTestIdentItem::Fields(), TEXT("判断项"), InSink);
		FString Prop;
		if (!InJson.TryGetStringField(TEXT("prop"), Prop) || Prop.IsEmpty())
		{
			if (bInPropRequired)
			{
				InSink.Error(TEXT("bad_type"), TEXT("判断项要写 prop（属性路径，@self.X、@部件.X、@部件:状态）"));
			}
		}
		else
		{
			GameTestRefCheck::Prop(Prop, InSink);
		}
		FString Op;
		const IGameTestCompare* Compare = nullptr;
		if (InJson.TryGetStringField(TEXT("op"), Op) && !Op.IsEmpty())
		{
			Compare = FGameTestCompareRegistry::Find(Op);
			if (Compare == nullptr)
			{
				TArray<FString> Suggestions;
				FGameTestCompareRegistry::Get().Suggest(Op, Suggestions);
				InSink.Error(TEXT("op_unknown"), FString::Printf(TEXT("没有比较 %s%s"), *Op,
					Suggestions.Num() > 0 ? *FString::Printf(TEXT("（是不是 %s）"), *FString::Join(Suggestions, TEXT("、"))) : TEXT("")));
			}
		}
		const TSharedPtr<FJsonValue> Value = InJson.TryGetField(TEXT("value"));
		if (Compare != nullptr)
		{
			if (Compare->NeedsValue() && !Value.IsValid())
			{
				InSink.Error(TEXT("bad_type"), FString::Printf(TEXT("比较 %s 要写 value"), *Op));
			}
			if (Value.IsValid())
			{
				FString ValueText;
				const bool bRef = Value->TryGetString(ValueText) && (ValueText.StartsWith(TEXT("$")) || ValueText.StartsWith(TEXT("=")));
				if (!bRef)
				{
					FGameTestDiagSink::FScope Scope(InSink, TEXT("value"));
					Compare->ValidateValue(Value, InSink);
				}
			}
			FString Cmp;
			if (InJson.TryGetStringField(TEXT("cmp"), Cmp) && !IsOneOf(Cmp, { TEXT("eq"), TEXT("ne"), TEXT("gt"), TEXT("gte"), TEXT("lt"), TEXT("lte") }))
			{
				InSink.Error(TEXT("bad_type"), FString::Printf(TEXT("cmp 写成了 %s：只能是 eq、ne、gt、gte、lt、lte"), *Cmp));
			}
		}
		if (Value.IsValid())
		{
			FGameTestDiagSink::FScope Scope(InSink, TEXT("value"));
			GameTestRefs::Validate(Value, InScope, InSink);
		}
	}

	void Target(const TSharedPtr<FJsonValue>& InValue, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink)
	{
		using namespace GameTestRefImpl;
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (InValue.IsValid() && InValue->TryGetObject(Object) && Object != nullptr && Object->IsValid() && (*Object)->HasField(TEXT("obj")))
		{
			CheckFields(**Object, FGameTestTarget::Fields(), TEXT("部件写法"), InSink);
			{
				FGameTestDiagSink::FScope Scope(InSink, TEXT("obj"));
				Ref((*Object)->TryGetField(TEXT("obj")), InScope, InSink);
			}
			FString Part;
			if ((*Object)->TryGetStringField(TEXT("part"), Part) && !Part.IsEmpty() && !IsPartPath(Part))
			{
				InSink.Error(TEXT("bad_type"), FString::Printf(TEXT("part「%s」要写设计器里起的控件名（嵌套 WBP 用 . 进去），运行时生成的条目写成引用 {\"class\", \"in\": {\"obj\", \"part\": 容器}}"), *Part));
			}
			FString Slate;
			if ((*Object)->HasField(TEXT("slate")) && (!(*Object)->TryGetStringField(TEXT("slate"), Slate) || Slate.IsEmpty()))
			{
				InSink.Error(TEXT("bad_type"), TEXT("slate 要写 类型[text=子串][序号]，多段用 / 分开"));
			}
			return;
		}
		Ref(InValue, InScope, InSink);
	}

	void Prop(const FString& InProp, FGameTestDiagSink& InSink)
	{
		if (InProp.Contains(TEXT("$")))
		{
			// 嵌了 `$变量` 的路径运行时才替换出来，写的时候查不了
			return;
		}
		FGameTestPropPath Path;
		FString Error;
		if (!FGameTestPropPath::Parse(InProp, Path, Error))
		{
			InSink.Error(TEXT("prop_missing"), FString::Printf(TEXT("属性路径「%s」写得不对：%s"), *InProp, *Error));
			return;
		}
		if (Path.HasState() && FGameTestStateRegistry::Find(Path.State) == nullptr)
		{
			TArray<FString> Suggestions;
			FGameTestStateRegistry::Get().Suggest(Path.State, Suggestions);
			InSink.Error(TEXT("prop_missing"), FString::Printf(TEXT("没有算出状态 :%s%s"), *Path.State,
				Suggestions.Num() > 0 ? *FString::Printf(TEXT("（是不是 :%s）"), *FString::Join(Suggestions, TEXT("、:"))) : TEXT("")));
		}
	}

	void Objects(const FJsonObject& InObjects, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink)
	{
		for (const auto& Pair : InObjects.Values)
		{
			const FString Name = GAMETEST_JSON_KEY(Pair.Key);
			FGameTestDiagSink::FScope Scope(InSink, FString::Printf(TEXT("objects.%s"), *Name));
			if (!GameTestRefForm::IsName(Name))
			{
				InSink.Error(TEXT("bad_type"), FString::Printf(TEXT("objects 的名字「%s」只能用字母、数字、下划线"), *Name));
			}
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
			{
				InSink.Error(TEXT("bad_type"), TEXT("objects 的值要写成引用对象 {\"class\", \"ident\"…}（不能写另一个名字）"));
				continue;
			}
			RefObject(**Object, InScope, InSink);
		}
		// in.obj 引用别的名字可以，但不能成环
		for (const auto& Pair : InObjects.Values)
		{
			const FString Start = GAMETEST_JSON_KEY(Pair.Key);
			TArray<FString> Chain = { Start };
			FString Current = Start;
			for (int32 Hop = 0; Hop < 32; ++Hop)
			{
				const TSharedPtr<FJsonObject> Def = InObjects.HasTypedField<EJson::Object>(Current) ? InObjects.GetObjectField(Current) : nullptr;
				const FString Next = Def.IsValid() ? GameTestRefImpl::InObjName(*Def) : FString();
				if (Next.IsEmpty())
				{
					break;
				}
				if (Chain.Contains(Next))
				{
					if (Next == Start)
					{
						FGameTestDiagSink::FScope Scope(InSink, FString::Printf(TEXT("objects.%s"), *Start));
						InSink.Error(TEXT("ref_cycle"), FString::Printf(TEXT("objects 的 in.obj 成环：%s → %s"), *FString::Join(Chain, TEXT(" → ")), *Next));
					}
					break;
				}
				Chain.Add(Next);
				Current = Next;
			}
		}
	}
}
