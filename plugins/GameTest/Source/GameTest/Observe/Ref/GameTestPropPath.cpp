// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestPropPath.h"

#include "Core/Script/GameTestFlowContext.h"
#include "GameTestCompat.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Observe/Lua/GameTestLuaBridge.h"
#include "Observe/Ref/GameTestState.h"
#include "Observe/Sources/GameTestReflectValue.h"

#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/UserWidget.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

// unity build 下各文件的匿名 namespace 会合并：辅助函数放进带文件名的 namespace。
namespace GameTestPropPathImpl
{
	/** 一段 `Items[3]` / `Map["key"]`：名字 + 下标。 */
	struct FSegment
	{
		FString Name;
		/** 下标原文：数字或带引号的键（已去引号）。 */
		TArray<FString> Keys;
	};

	bool SplitSegment(const FString& InText, FSegment& OutSegment, FString& OutError)
	{
		OutSegment = FSegment();
		int32 Bracket = INDEX_NONE;
		InText.FindChar(TEXT('['), Bracket);
		OutSegment.Name = Bracket == INDEX_NONE ? InText : InText.Left(Bracket);
		FString Rest = Bracket == INDEX_NONE ? FString() : InText.Mid(Bracket);
		while (!Rest.IsEmpty())
		{
			int32 Close = INDEX_NONE;
			if (!Rest.StartsWith(TEXT("[")) || !Rest.FindChar(TEXT(']'), Close))
			{
				OutError = FString::Printf(TEXT("「%s」的方括号不配对"), *InText);
				return false;
			}
			FString Key = Rest.Mid(1, Close - 1).TrimStartAndEnd();
			if (Key.Len() >= 2 && (Key.StartsWith(TEXT("\"")) || Key.StartsWith(TEXT("'"))))
			{
				Key = Key.Mid(1, Key.Len() - 2);
			}
			OutSegment.Keys.Add(Key);
			Rest = Rest.Mid(Close + 1);
		}
		return true;
	}

	bool IsIdentifier(const FString& InText)
	{
		if (InText.IsEmpty() || FChar::IsDigit(InText[0]))
		{
			return false;
		}
		for (const TCHAR C : InText)
		{
			if (!(FChar::IsAlnum(C) || C == TEXT('_')))
			{
				return false;
			}
		}
		return true;
	}

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

	/** 反射走路：当前所在的容器（对象或结构体）和值指针。 */
	struct FCursor
	{
		const UStruct* Struct = nullptr;
		void* Ptr = nullptr;
		UObject* Object = nullptr;
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
			InOutCursor.Struct = Next->GetClass();
			InOutCursor.Ptr = Next;
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

	/** 反射读剩下的段（`ShopId`、`Data.Items[0].Name`）。 */
	FGameTestPropRead ReadReflect(UObject* InObject, const TArray<FString>& InSegments, int32 InFrom)
	{
		FCursor Cursor;
		Cursor.Object = InObject;
		Cursor.Struct = InObject->GetClass();
		Cursor.Ptr = InObject;
		for (int32 Index = InFrom; Index < InSegments.Num(); ++Index)
		{
			FString Error;
			if (Cursor.Prop != nullptr && !Descend(Cursor, Error))
			{
				return Fail(Error);
			}
			FSegment Segment;
			if (!SplitSegment(InSegments[Index], Segment, Error))
			{
				return Fail(Error);
			}
			const FProperty* Prop = GameTestReflectValue::FindProperty(Cursor.Struct, Segment.Name);
			if (Prop == nullptr)
			{
				return Fail(FString::Printf(TEXT("%s 上没有属性 %s"), Cursor.Struct != nullptr ? *Cursor.Struct->GetName() : TEXT("?"), *Segment.Name));
			}
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Cursor.Ptr);
			for (const FString& Key : Segment.Keys)
			{
				if (!StepIndex(Prop, ValuePtr, Key, Error))
				{
					return Fail(Error);
				}
			}
			Cursor.Prop = Prop;
			Cursor.ValuePtr = ValuePtr;
		}
		FGameTestPropRead Read;
		if (Cursor.Prop == nullptr)
		{
			// `@部件` 后面什么都没写：值是部件对象本身
			Read.Value = FGameTestValue::FromObject(InObject);
			Read.Kind = EGameTestKind::Object;
			Read.Type = InObject->GetClass()->GetName();
			Read.Src = TEXT("cpp");
			Read.bReadOnly = true;
			Read.Holder = InObject;
			return Read;
		}
		FString Why;
		Read.Value = GameTestReflectValue::FromProperty(Cursor.Prop, Cursor.ValuePtr);
		Read.Kind = FGameTestPropKind::Of(Cursor.Prop, &Why);
		Read.Type = FGameTestPropKind::TypeName(Cursor.Prop);
		const UStruct* OwnerStruct = Cursor.Prop->GetOwnerStruct();
		const UPackage* Package = OwnerStruct != nullptr ? OwnerStruct->GetPackage() : nullptr;
		Read.Src = Package != nullptr && Package->GetName().StartsWith(TEXT("/Script/")) ? TEXT("cpp") : TEXT("bp");
		Read.bReadOnly = Read.Kind == EGameTestKind::None || Cursor.Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly | CPF_EditConst);
		Read.Holder = Cursor.Object;
		Read.PropName = Cursor.Prop->GetName();
		Read.RawProp = Cursor.Prop;
		Read.RawValue = Cursor.ValuePtr;
		if (const FTextProperty* TextProp = CastField<FTextProperty>(Cursor.Prop))
		{
			FString Namespace;
			FString Key;
			if (GameTestCompat::TextKeyOf(TextProp->GetPropertyValue(Cursor.ValuePtr), Namespace, Key))
			{
				Read.TextKey = Namespace + TEXT(",") + Key;
			}
		}
		return Read;
	}

	FGameTestPropRead ReadLua(UObject* InObject, const TArray<FString>& InSegments, int32 InFrom, FGameTestFlowContext& InContext)
	{
		TArray<FString> Rest;
		for (int32 Index = InFrom; Index < InSegments.Num(); ++Index)
		{
			Rest.Add(InSegments[Index]);
		}
		if (Rest.Num() == 0)
		{
			return Fail(TEXT("@lua 后面要写字段名（@lua.Count）"));
		}
		const FString Path = FString::Join(Rest, TEXT("."));
		FGameTestLuaValue LuaValue;
		FString Error;
		UWorld* World = GameTestWidgetProbe::WorldOf(InContext);
		if (!GameTestLuaBridge::Read(World, InObject, GameTestLuaBridge::SplitPath(Path), LuaValue, Error))
		{
			return Fail(FString::Printf(TEXT("%s 的 Lua 表读不到 %s：%s"), *InObject->GetName(), *Path, *Error));
		}
		FGameTestPropRead Read;
		Read.Value = LuaValue.ToValue();
		Read.Kind = FGameTestPropKind::OfValue(Read.Value);
		Read.Type = TEXT("lua:") + LuaValue.Type();
		Read.Src = TEXT("lua");
		Read.Holder = InObject;
		Read.PropName = TEXT("lua.") + Path;
		if (LuaValue.IsNil())
		{
			Read.Value = FGameTestValue::Unresolved(FString::Printf(TEXT("%s 的 Lua 表里没有 %s"), *InObject->GetName(), *Path), TEXT("prop_missing"));
		}
		return Read;
	}
}

//~ 解析 -------------------------------------------------------------------------

bool FGameTestPropPath::IsSourceWord(const FString& InSegment)
{
	return InSegment.Equals(TEXT("self"), ESearchCase::IgnoreCase) || InSegment.Equals(TEXT("lua"), ESearchCase::IgnoreCase)
		|| InSegment.Equals(TEXT("item"), ESearchCase::IgnoreCase) || InSegment.Equals(TEXT("itemLua"), ESearchCase::IgnoreCase);
}

bool FGameTestPropPath::Parse(const FString& InText, FGameTestPropPath& OutPath, FString& OutError)
{
	using namespace GameTestPropPathImpl;
	OutPath = FGameTestPropPath();
	OutPath.Text = InText.TrimStartAndEnd();
	const FString& Text = OutPath.Text;
	if (Text.StartsWith(TEXT(":")))
	{
		OutPath.State = Text.RightChop(1);
		if (!IsIdentifier(OutPath.State))
		{
			OutError = TEXT("冒号后面要写状态名（:text、:onScreen）");
			return false;
		}
		return true;
	}
	if (!Text.StartsWith(TEXT("@")))
	{
		OutError = TEXT("属性路径以 @ 开头（@self.X、@部件.X、@部件:状态），算出状态以 : 开头");
		return false;
	}
	// 按顶层的 `.` 和最后的 `:` 拆（方括号和引号里的不算）
	FString Current;
	int32 Depth = 0;
	TCHAR Quote = 0;
	FString Body = Text.RightChop(1);
	for (int32 Index = 0; Index < Body.Len(); ++Index)
	{
		const TCHAR C = Body[Index];
		if (Quote != 0)
		{
			Current.AppendChar(C);
			Quote = C == Quote ? 0 : Quote;
			continue;
		}
		if (C == TEXT('"') || C == TEXT('\''))
		{
			Quote = C;
			Current.AppendChar(C);
			continue;
		}
		if (C == TEXT('['))
		{
			++Depth;
		}
		else if (C == TEXT(']'))
		{
			--Depth;
		}
		if (Depth == 0 && C == TEXT('.'))
		{
			OutPath.Segments.Add(Current);
			Current.Reset();
			continue;
		}
		if (Depth == 0 && C == TEXT(':'))
		{
			OutPath.Segments.Add(Current);
			OutPath.State = Body.Mid(Index + 1);
			Current.Reset();
			if (!IsIdentifier(OutPath.State))
			{
				OutError = FString::Printf(TEXT("「%s」：冒号后面要写状态名"), *Text);
				return false;
			}
			break;
		}
		Current.AppendChar(C);
	}
	if (OutPath.State.IsEmpty())
	{
		OutPath.Segments.Add(Current);
	}
	if (Depth != 0 || Quote != 0)
	{
		OutError = FString::Printf(TEXT("「%s」的方括号或引号不配对"), *Text);
		return false;
	}
	for (const FString& Segment : OutPath.Segments)
	{
		FSegment Parsed;
		if (Segment.TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(TEXT("「%s」里有空段（两个点连在一起，或 @ 后面直接是点）"), *Text);
			return false;
		}
		if (!SplitSegment(Segment, Parsed, OutError))
		{
			return false;
		}
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
	TArray<FString> Pieces;
	InPart.ParseIntoArray(Pieces, TEXT("."));
	UObject* Current = InObject;
	for (const FString& Piece : Pieces)
	{
		UObject* Next = GameTestPropPathImpl::FindOnePart(Current, Piece);
		if (Next == nullptr)
		{
			OutError = Cast<UUserWidget>(Current) != nullptr || Cast<AActor>(Current) != nullptr
				? FString::Printf(TEXT("%s 里没有部件 %s"), *Current->GetClass()->GetName(), *Piece)
				: FString::Printf(TEXT("%s 不是 WBP，不能再往里找 %s（嵌套的 WBP 才能用 . 进去）"), *Current->GetClass()->GetName(), *Piece);
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
	// 还没显示出来的 ListView 条目只有数据对象没有控件：`@item.X` 照样能读（引用按它过滤，过滤完再滚出来）
	const bool bItemOnly = InObject == nullptr && InItem != nullptr && InPath.Segments.Num() > 0
		&& (InPath.Segments[0].Equals(TEXT("item"), ESearchCase::IgnoreCase) || InPath.Segments[0].Equals(TEXT("itemLua"), ESearchCase::IgnoreCase));
	if (bItemOnly)
	{
		return InPath.Segments[0].Equals(TEXT("itemLua"), ESearchCase::IgnoreCase) ? ReadLua(InItem, InPath.Segments, 1, InContext) : ReadReflect(InItem, InPath.Segments, 1);
	}
	if (InObject == nullptr)
	{
		if (bExistsState && InPath.Segments.Num() == 0)
		{
			FGameTestPropRead Read;
			Read.Value = FGameTestValue::FromBool(false);
			Read.Kind = EGameTestKind::Bool;
			Read.Src = TEXT("state");
			Read.Type = TEXT("bool");
			Read.bReadOnly = true;
			return Read;
		}
		return Fail(TEXT("对象没找到"), TEXT("ref_not_found"));
	}
	const TArray<FString>& Segments = InPath.Segments;
	UObject* Current = InObject;
	int32 Index = 0;
	bool bLua = false;
	if (Segments.Num() > 0)
	{
		const FString& First = Segments[0];
		if (First.Equals(TEXT("self"), ESearchCase::IgnoreCase))
		{
			Index = 1;
		}
		else if (First.Equals(TEXT("lua"), ESearchCase::IgnoreCase))
		{
			Index = 1;
			bLua = true;
		}
		else if (First.Equals(TEXT("item"), ESearchCase::IgnoreCase) || First.Equals(TEXT("itemLua"), ESearchCase::IgnoreCase))
		{
			UObject* Item = InItem != nullptr ? InItem : ListItemOf(InObject);
			if (Item == nullptr)
			{
				return Fail(FString::Printf(TEXT("%s 不是列表条目，没有数据对象（@item 只在 in 是 ListView 族容器时有）"), *InObject->GetClass()->GetName()));
			}
			Current = Item;
			Index = 1;
			bLua = First.Equals(TEXT("itemLua"), ESearchCase::IgnoreCase);
		}
		else
		{
			// 部件：一段一段往里找，找不到就当属性（`@ShopId` = `@self.ShopId`）
			while (Index < Segments.Num())
			{
				if (Segments[Index].Equals(TEXT("lua"), ESearchCase::IgnoreCase))
				{
					bLua = true;
					++Index;
					break;
				}
				UObject* Part = FindOnePart(Current, Segments[Index]);
				if (Part == nullptr)
				{
					break;
				}
				Current = Part;
				++Index;
			}
		}
	}

	if (InPath.HasState())
	{
		if (bLua || Index < Segments.Num())
		{
			if (bExistsState)
			{
				FGameTestPropRead Read;
				Read.Value = FGameTestValue::FromBool(false);
				Read.Kind = EGameTestKind::Bool;
				Read.Src = TEXT("state");
				Read.Type = TEXT("bool");
				Read.bReadOnly = true;
				return Read;
			}
			return Fail(FString::Printf(TEXT("%s 里没有部件 %s"), *InObject->GetClass()->GetName(), Index < Segments.Num() ? *Segments[Index] : TEXT("lua")), TEXT("part_missing"));
		}
		const IGameTestState* State = FGameTestStateRegistry::Find(InPath.State);
		if (State == nullptr)
		{
			return Fail(FString::Printf(TEXT("没有算出状态 :%s"), *InPath.State));
		}
		FGameTestStateContext StateContext(InContext);
		StateContext.Item = InItem != nullptr && Current == InObject ? InItem : nullptr;
		const FGameTestStateValue StateValue = State->Read(Current, StateContext);
		FGameTestPropRead Read;
		Read.Value = StateValue.Value;
		Read.TextKey = StateValue.TextKey;
		Read.Kind = State->GetKind();
		Read.Type = TEXT("state");
		Read.Src = TEXT("state");
		Read.bReadOnly = true;
		Read.Holder = Current;
		Read.PropName = TEXT(":") + InPath.State;
		return Read;
	}
	if (bLua)
	{
		return ReadLua(Current, Segments, Index, InContext);
	}
	return ReadReflect(Current, Segments, Index);
}

FGameTestPropRead GameTestPropRead::ReadText(UObject* InObject, const FString& InPath, FGameTestFlowContext& InContext, UObject* InItem)
{
	FGameTestPropPath Path;
	FString Error;
	if (!FGameTestPropPath::Parse(InPath, Path, Error))
	{
		return GameTestPropPathImpl::Fail(Error);
	}
	return Read(InObject, Path, InContext, InItem);
}
