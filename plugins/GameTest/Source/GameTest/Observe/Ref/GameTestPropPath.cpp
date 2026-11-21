// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestPropPath.h"

#include "Core/Script/GameTestFlowContext.h"
#include "GameTestCompat.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Observe/Lua/GameTestLuaBridge.h"
#include "Observe/Ref/GameTestState.h"
#include "Observe/Sources/GameTestReflectCall.h"
#include "Observe/Sources/GameTestReflectValue.h"

#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/UserWidget.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

// unity build 下各文件的匿名 namespace 会合并：辅助函数放进带文件名的 namespace。
namespace GameTestPropPathImpl
{
	bool IsPathIdentStart(TCHAR C)
	{
		return (C >= TEXT('A') && C <= TEXT('Z')) || (C >= TEXT('a') && C <= TEXT('z')) || C == TEXT('_');
	}

	bool IsPathIdentChar(TCHAR C)
	{
		return IsPathIdentStart(C) || (C >= TEXT('0') && C <= TEXT('9'));
	}

	/**
	 * 和面板 path.ts 的 IDENT 一样只认 ASCII：设计器名、属性名、Lua 字段名都是 ASCII。
	 * 另外认 `$`：`@/Claim_$what:text` 这类嵌 $变量 的段在展开前也要能解析（校验、显示用），运行时先替换再读。
	 */
	bool IsIdentifier(const FString& InText)
	{
		if (InText.IsEmpty() || !(IsPathIdentStart(InText[0]) || InText[0] == TEXT('$')))
		{
			return false;
		}
		for (const TCHAR C : InText)
		{
			if (!IsPathIdentChar(C) && C != TEXT('$'))
			{
				return false;
			}
		}
		return true;
	}

	bool IsSegStart(TCHAR C)
	{
		return C == TEXT('/') || C == TEXT('.') || C == TEXT('#') || C == TEXT(':');
	}

	const TCHAR* const NewRules = TEXT("对象自己的属性 @.X，子控件 / 组件 @/子控件.X（多层 @/A/B.X），Lua 字段 @#X，条目数据 @item.X / @item#X，运行状态 @:状态 / @/子控件:状态");

	/** Actor 的组件：按名字（忽略大小写）。 */
	UActorComponent* FindComponent(AActor* InActor, const FString& InName)
	{
		if (InActor == nullptr)
		{
			return nullptr;
		}
		TInlineComponentArray<UActorComponent*> Components(InActor);
		for (UActorComponent* Component : Components)
		{
			if (Component != nullptr && Component->GetName().Equals(InName, ESearchCase::IgnoreCase))
			{
				return Component;
			}
		}
		return nullptr;
	}

	/** 一层部件：UUserWidget 按它类自己树里的名字，Actor 按组件名。 */
	UObject* FindOnePart(UObject* InObject, const FString& InName)
	{
		if (UUserWidget* User = Cast<UUserWidget>(InObject))
		{
			return User->GetWidgetFromName(FName(*InName));
		}
		if (AActor* Actor = Cast<AActor>(InObject))
		{
			return FindComponent(Actor, InName);
		}
		return nullptr;
	}

	FString MissingPartWhy(const UObject* InIn, const FString& InName)
	{
		return Cast<UUserWidget>(InIn) != nullptr || Cast<AActor>(InIn) != nullptr
			? FString::Printf(TEXT("%s 里没有子控件或组件 %s"), *InIn->GetClass()->GetName(), *InName)
			: FString::Printf(TEXT("%s 不是 WBP 也不是 Actor，不能再往里找 %s（嵌套的 WBP 才能用 / 进去）"), *InIn->GetClass()->GetName(), *InName);
	}

	/** 反射走路：当前所在的容器（对象或结构体）和值指针。 */
	struct FCursor
	{
		const UStruct* Struct = nullptr;
		void* Ptr = nullptr;
		/** 当前容器是对象时是它；进了结构体为空（函数、Lua 字段要对象）。 */
		UObject* Object = nullptr;
		/** 最近经过的对象（进了结构体也不变）：读到的值挂在它上面，obj.set 往它上面写。 */
		UObject* Owner = nullptr;
		const FProperty* Prop = nullptr;
		void* ValuePtr = nullptr;
	};

	/** 按一个下标进容器属性（数组、Map），返回元素属性和指针；进不去写原因。 */
	bool StepIndex(const FProperty*& InOutProp, void*& InOutPtr, const FString& InKey, FString& OutError)
	{
		if (const FArrayProperty* Array = CastField<FArrayProperty>(InOutProp))
		{
			FScriptArrayHelper Helper(Array, InOutPtr);
			const int32 Index = FCString::Atoi(*InKey);
			if (!InKey.IsNumeric() || !Helper.IsValidIndex(Index))
			{
				OutError = FString::Printf(TEXT("%s 没有第 %s 项（共 %d 项，下标从 0 开始）"), *Array->GetName(), *InKey, Helper.Num());
				return false;
			}
			InOutProp = Array->Inner;
			InOutPtr = Helper.GetRawPtr(Index);
			return true;
		}
		if (const FMapProperty* Map = CastField<FMapProperty>(InOutProp))
		{
			FScriptMapHelper Helper(Map, InOutPtr);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				const FGameTestValue Key = GameTestReflectValue::FromProperty(Map->KeyProp, Helper.GetKeyPtr(Index));
				const FString KeyText = Key.IsString() ? Key.GetString() : Key.ToDisplay(200);
				if (KeyText.Equals(InKey, ESearchCase::IgnoreCase))
				{
					InOutProp = Map->ValueProp;
					InOutPtr = Helper.GetValuePtr(Index);
					return true;
				}
			}
			OutError = FString::Printf(TEXT("%s 里没有键 %s"), *Map->GetName(), *InKey);
			return false;
		}
		OutError = FString::Printf(TEXT("%s 不是数组或 Map，不能写 [%s]"), InOutProp != nullptr ? *InOutProp->GetName() : TEXT("?"), *InKey);
		return false;
	}

	/** 当前值是对象或结构体时，把它当成下一段的容器。 */
	bool Descend(FCursor& InOutCursor, FString& OutError)
	{
		if (const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(InOutCursor.Prop))
		{
			UObject* Next = ObjectProp->GetObjectPropertyValue(InOutCursor.ValuePtr);
			if (Next == nullptr)
			{
				OutError = FString::Printf(TEXT("%s 是空的"), *InOutCursor.Prop->GetName());
				return false;
			}
			InOutCursor.Object = Next;
			InOutCursor.Owner = Next;
			InOutCursor.Struct = Next->GetClass();
			InOutCursor.Ptr = Next;
			InOutCursor.Prop = nullptr;
			InOutCursor.ValuePtr = nullptr;
			return true;
		}
		if (const FStructProperty* StructProp = CastField<FStructProperty>(InOutCursor.Prop))
		{
			if (StructProp->Struct == nullptr)
			{
				OutError = FString::Printf(TEXT("%s 的结构体类型不认识"), *StructProp->GetName());
				return false;
			}
			InOutCursor.Struct = StructProp->Struct;
			InOutCursor.Ptr = InOutCursor.ValuePtr;
			// 进了结构体：下一段不再直接落在对象上（函数、Lua 字段要对象）
			InOutCursor.Object = nullptr;
			InOutCursor.Prop = nullptr;
			InOutCursor.ValuePtr = nullptr;
			return true;
		}
		OutError = FString::Printf(TEXT("%s 不是对象或结构体，不能再往下取"), *InOutCursor.Prop->GetName());
		return false;
	}

	FGameTestPropRead Fail(const FString& InWhy, FName InCode = TEXT("prop_missing"))
	{
		FGameTestPropRead Read;
		Read.Value = FGameTestValue::Unresolved(InWhy, InCode);
		return Read;
	}

	FGameTestPropRead ExistsFalse()
	{
		FGameTestPropRead Read;
		Read.Value = FGameTestValue::FromBool(false);
		Read.Kind = EGameTestKind::Bool;
		Read.Src = TEXT("state");
		Read.Type = TEXT("bool");
		Read.bReadOnly = true;
		return Read;
	}

	FGameTestPropRead ObjectItself(UObject* InObject)
	{
		FGameTestPropRead Read;
		Read.Value = FGameTestValue::FromObject(InObject);
		Read.Kind = EGameTestKind::Object;
		Read.Type = InObject != nullptr ? InObject->GetClass()->GetName() : FString(TEXT("Object"));
		Read.Src = TEXT("cpp");
		Read.bReadOnly = true;
		Read.Holder = InObject;
		return Read;
	}

	/** 光标停在反射属性上：读值、类型、来源、能不能写。 */
	FGameTestPropRead FromCursor(const FCursor& InCursor, UObject* InHolder)
	{
		FGameTestPropRead Read;
		FString Why;
		Read.Value = GameTestReflectValue::FromProperty(InCursor.Prop, InCursor.ValuePtr);
		Read.Kind = FGameTestPropKind::Of(InCursor.Prop, &Why);
		Read.Type = FGameTestPropKind::TypeName(InCursor.Prop);
		const UStruct* OwnerStruct = InCursor.Prop->GetOwnerStruct();
		const UPackage* Package = OwnerStruct != nullptr ? OwnerStruct->GetPackage() : nullptr;
		Read.Src = Package != nullptr && Package->GetName().StartsWith(TEXT("/Script/")) ? TEXT("cpp") : TEXT("bp");
		Read.bReadOnly = Read.Kind == EGameTestKind::None || InCursor.Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly | CPF_EditConst);
		Read.Holder = InHolder;
		Read.PropName = InCursor.Prop->GetName();
		Read.RawProp = InCursor.Prop;
		Read.RawValue = InCursor.ValuePtr;
		if (const FTextProperty* TextProp = CastField<FTextProperty>(InCursor.Prop))
		{
			FString Namespace;
			FString Key;
			if (GameTestCompat::TextKeyOf(TextProp->GetPropertyValue(InCursor.ValuePtr), Namespace, Key))
			{
				Read.TextKey = Namespace + TEXT(",") + Key;
			}
		}
		return Read;
	}

	/** `.名字()`：只许无参、有返回值、BlueprintPure 或 const 的函数（读值不能改游戏）。 */
	FGameTestValue CallGetter(UObject* InObject, const FString& InName, FGameTestFlowContext& InContext)
	{
		UFunction* Function = GameTestReflectValue::FindFunction(InObject->GetClass(), InName);
		if (Function == nullptr)
		{
			return FGameTestValue::Unresolved(FString::Printf(TEXT("%s 上没有函数 %s"), *InObject->GetClass()->GetName(), *InName), TEXT("prop_missing"));
		}
		if (!GameTestReflectValue::IsReadOnlyFunction(*Function))
		{
			return FGameTestValue::Unresolved(FString::Printf(TEXT("%s 不是 BlueprintPure 或 const 函数：路径里只能调取值函数"), *InName), TEXT("fn_not_callable"));
		}
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				return FGameTestValue::Unresolved(FString::Printf(TEXT("%s 有参数 %s：路径里只能调无参的取值函数（有参数的用 obj.call）"), *InName, *It->GetName()), TEXT("fn_not_callable"));
			}
		}
		if (Function->GetReturnProperty() == nullptr)
		{
			return FGameTestValue::Unresolved(FString::Printf(TEXT("%s 没有返回值"), *InName), TEXT("fn_not_callable"));
		}
		// 读路径不是一次调用步骤：不往报告的 payload.invoke 里写
		const TSharedPtr<FJsonObject> PrevPayload = InContext.StepPayload;
		InContext.StepPayload.Reset();
		const TMap<FString, FGameTestValue> None;
		GameTestReflectCall::FArgs Args;
		Args.Named = &None;
		const FGameTestValue Value = GameTestReflectCall::Call(InObject, Function, Args, InContext);
		InContext.StepPayload = PrevPayload;
		return Value;
	}

	FGameTestPropRead ReadLua(UObject* InObject, const TArray<TSharedPtr<FJsonValue>>& InKeys, const FString& InPath, FGameTestFlowContext& InContext)
	{
		FGameTestLuaValue LuaValue;
		FString Error;
		UWorld* World = GameTestWidgetProbe::WorldOf(InContext);
		if (!GameTestLuaBridge::Read(World, InObject, InKeys, LuaValue, Error))
		{
			return Fail(FString::Printf(TEXT("%s 的 Lua 表读不到 %s：%s"), *InObject->GetName(), *InPath, *Error));
		}
		FGameTestPropRead Read;
		Read.Value = LuaValue.ToValue();
		Read.Kind = FGameTestPropKind::OfValue(Read.Value);
		Read.Type = TEXT("lua:") + LuaValue.Type();
		Read.Src = TEXT("lua");
		Read.Holder = InObject;
		Read.PropName = TEXT("lua.") + InPath;
		if (LuaValue.IsNil())
		{
			Read.Value = FGameTestValue::Unresolved(FString::Printf(TEXT("%s 的 Lua 表里没有 %s"), *InObject->GetName(), *InPath), TEXT("prop_missing"));
		}
		return Read;
	}

	/** 子控件之后的各段（属性、函数、Lua、下标）和最后的状态。 */
	struct FWalk
	{
		FGameTestFlowContext& Context;
		/** 从对象自己出发、没走子控件（`:selected` 这类条目状态要条目的数据对象）。 */
		bool bAtRoot = false;
		FCursor Cursor;
		/** 调过取值函数之后：值不再有属性地址，按值继续走。 */
		bool bValueMode = false;
		FGameTestValue Value;

		explicit FWalk(FGameTestFlowContext& InContext)
			: Context(InContext)
		{
		}

		void StartAt(UObject* InObject)
		{
			Cursor = FCursor();
			Cursor.Object = InObject;
			Cursor.Owner = InObject;
			Cursor.Struct = InObject->GetClass();
			Cursor.Ptr = InObject;
			bValueMode = false;
		}

		/** 现在落在哪个对象上（函数、Lua、状态要对象）；落在结构体或标量上返回空并写原因。 */
		UObject* LandingObject(FString& OutError)
		{
			if (bValueMode)
			{
				UObject* Object = Value.GetObject();
				if (Object == nullptr)
				{
					OutError = FString::Printf(TEXT("函数返回的 %s 不是对象"), *Value.ToDisplay(60));
				}
				return Object;
			}
			if (Cursor.Prop != nullptr)
			{
				FCursor Probe = Cursor;
				if (CastField<FObjectPropertyBase>(Probe.Prop) == nullptr)
				{
					OutError = FString::Printf(TEXT("%s 不是对象"), *Probe.Prop->GetName());
					return nullptr;
				}
				if (!Descend(Probe, OutError))
				{
					return nullptr;
				}
				return Probe.Object;
			}
			if (Cursor.Object == nullptr)
			{
				OutError = TEXT("这里是结构体，不是对象");
			}
			return Cursor.Object;
		}

		bool StepProp(const FString& InName, FString& OutError)
		{
			if (bValueMode)
			{
				if (UObject* Object = Value.GetObject())
				{
					StartAt(Object);
				}
				else
				{
					for (const TPair<FString, FGameTestValue>& Pair : Value.GetMap())
					{
						if (Pair.Key.Equals(InName, ESearchCase::IgnoreCase))
						{
							Value = Pair.Value;
							return true;
						}
					}
					OutError = FString::Printf(TEXT("函数返回值里没有 %s"), *InName);
					return false;
				}
			}
			if (Cursor.Prop != nullptr && !Descend(Cursor, OutError))
			{
				return false;
			}
			const FProperty* Prop = GameTestReflectValue::FindProperty(Cursor.Struct, InName);
			if (Prop == nullptr)
			{
				OutError = FString::Printf(TEXT("%s 上没有属性 %s"), Cursor.Struct != nullptr ? *Cursor.Struct->GetName() : TEXT("?"), *InName);
				return false;
			}
			Cursor.Prop = Prop;
			Cursor.ValuePtr = Prop->ContainerPtrToValuePtr<void>(Cursor.Ptr);
			return true;
		}

		bool StepIndexSeg(const FString& InKey, FString& OutError)
		{
			if (bValueMode)
			{
				const TArray<FGameTestValue>& Items = Value.GetArray();
				if (InKey.IsNumeric())
				{
					const int32 Index = FCString::Atoi(*InKey);
					if (!Items.IsValidIndex(Index))
					{
						OutError = FString::Printf(TEXT("返回值没有第 %s 项（共 %d 项）"), *InKey, Items.Num());
						return false;
					}
					Value = Items[Index];
					return true;
				}
				for (const TPair<FString, FGameTestValue>& Pair : Value.GetMap())
				{
					if (Pair.Key.Equals(InKey, ESearchCase::IgnoreCase))
					{
						Value = Pair.Value;
						return true;
					}
				}
				OutError = FString::Printf(TEXT("返回值里没有键 %s"), *InKey);
				return false;
			}
			if (Cursor.Prop == nullptr)
			{
				OutError = FString::Printf(TEXT("[%s] 要跟在属性后面"), *InKey);
				return false;
			}
			const FProperty* Prop = Cursor.Prop;
			void* Ptr = Cursor.ValuePtr;
			if (!StepIndex(Prop, Ptr, InKey, OutError))
			{
				return false;
			}
			Cursor.Prop = Prop;
			Cursor.ValuePtr = Ptr;
			return true;
		}

		bool StepFn(const FString& InName, FString& OutError, FName& OutCode)
		{
			UObject* Object = LandingObject(OutError);
			if (Object == nullptr)
			{
				OutError = FString::Printf(TEXT("%s() 要落在对象上：%s"), *InName, *OutError);
				return false;
			}
			const FGameTestValue Result = CallGetter(Object, InName, Context);
			if (Result.IsUnresolved())
			{
				OutError = Result.GetString();
				OutCode = Result.GetCode();
				return false;
			}
			Value = Result;
			bValueMode = true;
			return true;
		}
	};
}

//~ 解析 -------------------------------------------------------------------------

int32 FGameTestPropPath::NumChildren() const
{
	int32 Count = 0;
	while (Count < Segs.Num() && Segs[Count].Kind == EGameTestPathSeg::Child)
	{
		++Count;
	}
	return Count;
}

FString FGameTestPropPath::ChildPart() const
{
	TArray<FString> Names;
	for (int32 Index = 0; Index < NumChildren(); ++Index)
	{
		Names.Add(Segs[Index].Name);
	}
	return FString::Join(Names, TEXT("."));
}

FString FGameTestPropPath::Format() const
{
	FString Out = bItem ? TEXT("@item") : TEXT("@");
	for (const FGameTestPathSeg& Seg : Segs)
	{
		switch (Seg.Kind)
		{
		case EGameTestPathSeg::Child:
			Out += TEXT("/") + Seg.Name;
			break;
		case EGameTestPathSeg::Lua:
			Out += TEXT("#") + Seg.Name;
			break;
		case EGameTestPathSeg::Fn:
			Out += TEXT(".") + Seg.Name + TEXT("()");
			break;
		case EGameTestPathSeg::Index:
			Out += Seg.Name.IsNumeric() ? FString::Printf(TEXT("[%s]"), *Seg.Name) : FString::Printf(TEXT("[\"%s\"]"), *Seg.Name);
			break;
		default:
			Out += TEXT(".") + Seg.Name;
			break;
		}
	}
	if (HasState())
	{
		Out += TEXT(":") + State;
	}
	return Out;
}

FString FGameTestPropPath::PartToPath(const FString& InPart)
{
	TArray<FString> Pieces;
	InPart.Replace(TEXT("/"), TEXT(".")).ParseIntoArray(Pieces, TEXT("."), /*CullEmpty=*/true);
	FString Out;
	for (const FString& Piece : Pieces)
	{
		Out += TEXT("/") + Piece;
	}
	return Out;
}

FString FGameTestPropPath::OfProp(const FString& InName)
{
	return TEXT("@.") + InName;
}

FString FGameTestPropPath::OfState(const FString& InState)
{
	return TEXT("@:") + InState;
}

FString FGameTestPropPath::SuggestNew(const FString& InOld)
{
	const FString Old = InOld.TrimStartAndEnd();
	if (Old.StartsWith(TEXT(":")))
	{
		return TEXT("@") + Old;
	}
	struct FMap
	{
		const TCHAR* From;
		const TCHAR* To;
	};
	// 长的在前：@itemLua. 要先于 @item 认
	static const FMap Maps[] = {
		{ TEXT("@itemLua."), TEXT("@item#") }, { TEXT("@self."), TEXT("@.") }, { TEXT("@self:"), TEXT("@:") }, { TEXT("@lua."), TEXT("@#") },
	};
	for (const FMap& Map : Maps)
	{
		if (Old.StartsWith(Map.From, ESearchCase::IgnoreCase))
		{
			return Map.To + Old.Mid(FCString::Strlen(Map.From));
		}
	}
	return FString();
}

bool FGameTestPropPath::Parse(const FString& InText, FGameTestPropPath& OutPath, FString& OutError)
{
	using namespace GameTestPropPathImpl;
	OutPath = FGameTestPropPath();
	OutPath.Text = InText.TrimStartAndEnd();
	const FString& S = OutPath.Text;
	auto Syntax = [&OutPath, &OutError, &S](const FString& InWhy, int32 InAt)
	{
		OutPath.FailCode = TEXT("path_syntax");
		OutError = FString::Printf(TEXT("属性路径「%s」第 %d 个字符：%s"), *S, InAt + 1, *InWhy);
		return false;
	};
	auto Old = [&OutPath, &OutError, &S]()
	{
		OutPath.FailCode = TEXT("path_old");
		const FString Suggest = SuggestNew(S);
		OutError = Suggest.IsEmpty()
			? FString::Printf(TEXT("「%s」是旧的属性路径写法（2.1.0 起改了）：%s"), *S, NewRules)
			: FString::Printf(TEXT("「%s」是旧的属性路径写法（2.1.0 起改了），改成 %s（%s）"), *S, *Suggest, NewRules);
		return false;
	};
	if (S.IsEmpty())
	{
		return Syntax(TEXT("是空的，属性路径以 @ 开头"), 0);
	}
	if (S[0] == TEXT(':'))
	{
		return Old();
	}
	if (S[0] != TEXT('@'))
	{
		return Syntax(TEXT("属性路径以 @ 开头（@.属性、@/子控件.属性、@:状态）"), 0);
	}
	int32 I = 1;
	if (S.StartsWith(TEXT("@item"), ESearchCase::CaseSensitive) && (S.Len() == 5 || !IsPathIdentChar(S[5])))
	{
		OutPath.bItem = true;
		I = 5;
	}
	else if (S.Len() > 1 && IsPathIdentStart(S[1]))
	{
		// `@self.X`、`@lua.X`、`@itemLua.X`、`@部件.X`：v4 的写法，@ 后面直接跟名字
		return Old();
	}
	while (I < S.Len())
	{
		const TCHAR C = S[I];
		if (OutPath.HasState())
		{
			return Syntax(TEXT("运行状态只能在最后"), I);
		}
		if (C == TEXT('['))
		{
			const int32 Close = S.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, I);
			if (Close == INDEX_NONE)
			{
				return Syntax(TEXT("方括号不配对"), I);
			}
			if (OutPath.Segs.Num() == 0 || OutPath.Segs.Last().Kind == EGameTestPathSeg::Child)
			{
				return Syntax(TEXT("下标要跟在属性后面"), I);
			}
			FString Key = S.Mid(I + 1, Close - I - 1).TrimStartAndEnd();
			if (Key.Len() >= 2 && (Key[0] == TEXT('"') || Key[0] == TEXT('\'')) && (Key[Key.Len() - 1] == TEXT('"') || Key[Key.Len() - 1] == TEXT('\'')))
			{
				Key = Key.Mid(1, Key.Len() - 2);
			}
			if (Key.IsEmpty())
			{
				return Syntax(TEXT("方括号里是空的"), I);
			}
			OutPath.Segs.Add({ EGameTestPathSeg::Index, Key });
			I = Close + 1;
			continue;
		}
		if (!IsSegStart(C))
		{
			return Syntax(FString::Printf(TEXT("「%c」不认识：每段以 / . # : 开头"), C), I);
		}
		int32 J = I + 1;
		while (J < S.Len() && !IsSegStart(S[J]) && S[J] != TEXT('['))
		{
			++J;
		}
		FString Name = S.Mid(I + 1, J - I - 1);
		EGameTestPathSeg Kind = C == TEXT('/') ? EGameTestPathSeg::Child : C == TEXT('#') ? EGameTestPathSeg::Lua : EGameTestPathSeg::Prop;
		const bool bState = C == TEXT(':');
		if (Kind == EGameTestPathSeg::Prop && !bState && Name.EndsWith(TEXT("()")))
		{
			Kind = EGameTestPathSeg::Fn;
			Name = Name.LeftChop(2);
		}
		if (!IsIdentifier(Name))
		{
			return Syntax(FString::Printf(TEXT("「%s」不是合法的名字"), *Name), I + 1);
		}
		if (Kind == EGameTestPathSeg::Child && !bState && OutPath.Segs.ContainsByPredicate([](const FGameTestPathSeg& Seg) { return Seg.Kind != EGameTestPathSeg::Child; }))
		{
			return Syntax(TEXT("子控件要写在属性前面"), I);
		}
		if (bState)
		{
			OutPath.State = Name;
		}
		else
		{
			OutPath.Segs.Add({ Kind, Name });
		}
		I = J;
	}
	return true;
}

//~ 读 -------------------------------------------------------------------------

UObject* GameTestPropRead::ListItemOf(UObject* InObject)
{
	UUserWidget* Current = Cast<UUserWidget>(InObject);
	if (Current == nullptr && InObject != nullptr)
	{
		Current = InObject->GetTypedOuter<UUserWidget>();
	}
	for (; Current != nullptr; Current = Current->GetTypedOuter<UUserWidget>())
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

UObject* GameTestPropRead::FindPart(UObject* InObject, const FString& InPart, FString& OutError)
{
	if (InObject == nullptr)
	{
		OutError = TEXT("对象没找到");
		return nullptr;
	}
	if (InPart.IsEmpty())
	{
		return InObject;
	}
	// target 的 part 用 `.` 分层；属性路径里的子控件段是 `/`，两种都认，从路径里截出来的部件也能直接用
	TArray<FString> Pieces;
	InPart.Replace(TEXT("/"), TEXT(".")).ParseIntoArray(Pieces, TEXT("."), /*CullEmpty=*/true);
	UObject* Current = InObject;
	for (const FString& Piece : Pieces)
	{
		UObject* Next = GameTestPropPathImpl::FindOnePart(Current, Piece);
		if (Next == nullptr)
		{
			OutError = GameTestPropPathImpl::MissingPartWhy(Current, Piece);
			return nullptr;
		}
		Current = Next;
	}
	return Current;
}

bool GameTestPropRead::PartPathOf(const UWidget* InWidget, const UObject* InOwner, FString& OutPart)
{
	OutPart.Reset();
	TArray<FString> Pieces;
	const UWidget* Current = InWidget;
	for (int32 Guard = 0; Current != nullptr && Current != InOwner && Guard < 32; ++Guard)
	{
		UUserWidget* Outer = Current->GetTypedOuter<UUserWidget>();
		if (Outer == nullptr || Outer->GetWidgetFromName(Current->GetFName()) != Current)
		{
			// 运行时生成的（列表条目、CreateWidget 加的）：不是设计器名字，不能当部件
			return false;
		}
		Pieces.Insert(Current->GetName(), 0);
		Current = Outer;
	}
	if (Current != InOwner)
	{
		return false;
	}
	OutPart = FString::Join(Pieces, TEXT("."));
	return true;
}

FGameTestPropRead GameTestPropRead::Read(UObject* InObject, const FGameTestPropPath& InPath, FGameTestFlowContext& InContext, UObject* InItem)
{
	using namespace GameTestPropPathImpl;
	const bool bExistsState = InPath.State.Equals(TEXT("exists"), ESearchCase::IgnoreCase);
	// 起点：@item 是条目的数据对象。还没显示出来的 ListView 条目只有数据对象没有控件：`@item.X` 照样能读（引用按它过滤，过滤完再滚出来）
	UObject* Start = InObject;
	if (InPath.bItem)
	{
		Start = InItem != nullptr ? InItem : ListItemOf(InObject);
		if (Start == nullptr)
		{
			if (InObject == nullptr)
			{
				return bExistsState && InPath.Segs.Num() == 0 ? ExistsFalse() : Fail(TEXT("对象没找到"), TEXT("ref_not_found"));
			}
			return Fail(FString::Printf(TEXT("%s 不是列表条目，没有数据对象（@item 只在 in 是 ListView 族容器时有）"), *InObject->GetClass()->GetName()));
		}
	}
	else if (InObject == nullptr)
	{
		return bExistsState && InPath.Segs.Num() == 0 ? ExistsFalse() : Fail(TEXT("对象没找到"), TEXT("ref_not_found"));
	}

	// 子控件 / 组件段
	UObject* Current = Start;
	const int32 Children = InPath.NumChildren();
	for (int32 Index = 0; Index < Children; ++Index)
	{
		UObject* Part = FindOnePart(Current, InPath.Segs[Index].Name);
		if (Part == nullptr)
		{
			return bExistsState ? ExistsFalse() : Fail(MissingPartWhy(Current, InPath.Segs[Index].Name), TEXT("part_missing"));
		}
		Current = Part;
	}

	FWalk Walk(InContext);
	Walk.bAtRoot = Children == 0 && !InPath.bItem;
	Walk.StartAt(Current);
	for (int32 Index = Children; Index < InPath.Segs.Num(); ++Index)
	{
		const FGameTestPathSeg& Seg = InPath.Segs[Index];
		FString Error;
		FName Code = TEXT("prop_missing");
		bool bOk = true;
		switch (Seg.Kind)
		{
		case EGameTestPathSeg::Prop:
			bOk = Walk.StepProp(Seg.Name, Error);
			break;
		case EGameTestPathSeg::Index:
			bOk = Walk.StepIndexSeg(Seg.Name, Error);
			break;
		case EGameTestPathSeg::Fn:
			bOk = Walk.StepFn(Seg.Name, Error, Code);
			break;
		case EGameTestPathSeg::Lua:
		{
			UObject* Holder = Walk.LandingObject(Error);
			if (Holder == nullptr)
			{
				return Fail(FString::Printf(TEXT("#%s 要落在绑了 Lua 的对象上：%s"), *Seg.Name, *Error));
			}
			// `#a.b[2]`：# 之后的 . 和 [] 都是 Lua 表的键
			TArray<TSharedPtr<FJsonValue>> Keys;
			FString LuaPath = Seg.Name;
			Keys.Add(MakeShared<FJsonValueString>(Seg.Name));
			for (int32 Rest = Index + 1; Rest < InPath.Segs.Num(); ++Rest)
			{
				const FGameTestPathSeg& Next = InPath.Segs[Rest];
				if (Next.Kind == EGameTestPathSeg::Index)
				{
					if (Next.Name.IsNumeric())
					{
						Keys.Add(MakeShared<FJsonValueNumber>(FCString::Atod(*Next.Name)));
						LuaPath += FString::Printf(TEXT("[%s]"), *Next.Name);
					}
					else
					{
						Keys.Add(MakeShared<FJsonValueString>(Next.Name));
						LuaPath += TEXT(".") + Next.Name;
					}
				}
				else if (Next.Kind == EGameTestPathSeg::Prop || Next.Kind == EGameTestPathSeg::Lua)
				{
					Keys.Add(MakeShared<FJsonValueString>(Next.Name));
					LuaPath += TEXT(".") + Next.Name;
				}
				else
				{
					return Fail(FString::Printf(TEXT("Lua 字段 #%s 后面不能调 %s()：调 Lua 函数用 obj.call"), *Seg.Name, *Next.Name));
				}
			}
			if (InPath.HasState())
			{
				return Fail(FString::Printf(TEXT("Lua 字段 #%s 后面不能写运行状态 :%s"), *Seg.Name, *InPath.State));
			}
			return ReadLua(Holder, Keys, LuaPath, InContext);
		}
		default:
			bOk = false;
			Error = TEXT("子控件要写在属性前面");
			break;
		}
		if (!bOk)
		{
			return Fail(Error, Code);
		}
	}

	if (InPath.HasState())
	{
		FString Error;
		const bool bMoved = InPath.Segs.Num() > Children;
		UObject* Holder = bMoved ? Walk.LandingObject(Error) : Current;
		if (Holder == nullptr)
		{
			return bExistsState ? ExistsFalse() : Fail(FString::Printf(TEXT("运行状态 :%s 要落在对象上：%s"), *InPath.State, *Error));
		}
		const IGameTestState* State = FGameTestStateRegistry::Find(InPath.State);
		if (State == nullptr)
		{
			return Fail(FString::Printf(TEXT("没有运行状态 :%s"), *InPath.State));
		}
		FGameTestStateContext StateContext(InContext);
		StateContext.Item = Walk.bAtRoot && !bMoved ? InItem : nullptr;
		const FGameTestStateValue StateValue = State->Read(Holder, StateContext);
		FGameTestPropRead Read;
		Read.Value = StateValue.Value;
		Read.TextKey = StateValue.TextKey;
		Read.Kind = State->GetKind();
		Read.Type = TEXT("state");
		Read.Src = TEXT("state");
		Read.bReadOnly = true;
		Read.Holder = Holder;
		Read.PropName = TEXT(":") + InPath.State;
		return Read;
	}
	if (Walk.bValueMode)
	{
		FGameTestPropRead Read;
		Read.Value = Walk.Value;
		Read.Kind = FGameTestPropKind::OfValue(Walk.Value);
		Read.Type = TEXT("fn");
		Read.Src = TEXT("fn");
		Read.bReadOnly = true;
		return Read;
	}
	if (Walk.Cursor.Prop == nullptr)
	{
		// `@` / `@/子控件` 后面什么都没写：值是对象本身
		return ObjectItself(Walk.Cursor.Object != nullptr ? Walk.Cursor.Object : Current);
	}
	// 落在结构体成员上时 Holder 是最近经过的对象（obj.set 原地写进结构体后要标脏的那个）
	return FromCursor(Walk.Cursor, Walk.Cursor.Owner != nullptr ? Walk.Cursor.Owner : Current);
}

FGameTestPropRead GameTestPropRead::ReadText(UObject* InObject, const FString& InPath, FGameTestFlowContext& InContext, UObject* InItem)
{
	FGameTestPropPath Path;
	FString Error;
	if (!FGameTestPropPath::Parse(InPath, Path, Error))
	{
		return GameTestPropPathImpl::Fail(Error, Path.FailCode.IsNone() ? FName(TEXT("path_syntax")) : Path.FailCode);
	}
	return Read(InObject, Path, InContext, InItem);
}
