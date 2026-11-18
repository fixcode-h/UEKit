// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Lua/GameTestLuaBridge.h"

#include "AgentPanelBridge.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"

namespace
{
	/** 一次读表最多列多少项：条件、取值用不到更多，免得大表拖慢每帧判断。 */
	constexpr int32 MaxFields = 200;
	/** Lua 函数调用的时限（毫秒）：脚本里调的应该是马上返回的函数，死循环由 UELink 的钩子中止。 */
	constexpr int32 CallTimeoutMs = 10000;

	TSharedRef<FJsonObject> BaseArgs(UWorld* InWorld, UObject* InSelf)
	{
		const TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
		if (InWorld != nullptr)
		{
			// 多客户端时每个世界一个 Lua 环境：按世界选
			Args->SetStringField(TEXT("world"), FAgentPanelBridge::WorldId(InWorld));
		}
		if (InSelf != nullptr)
		{
			Args->SetStringField(TEXT("path"), InSelf->GetPathName());
		}
		return Args;
	}

	FGameTestLuaValue FromGetResult(const TSharedPtr<FJsonObject>& InOut)
	{
		FGameTestLuaValue Value;
		const TSharedPtr<FJsonObject>* Desc = nullptr;
		if (InOut.IsValid() && InOut->TryGetObjectField(TEXT("value"), Desc) && Desc != nullptr)
		{
			Value.Desc = *Desc;
		}
		const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
		if (InOut.IsValid() && InOut->TryGetArrayField(TEXT("fields"), Fields) && Fields != nullptr)
		{
			Value.Fields = *Fields;
		}
		bool bMore = false;
		if (InOut.IsValid() && InOut->TryGetBoolField(TEXT("more"), bMore))
		{
			Value.bMore = bMore;
		}
		return Value;
	}

	/** 一项描述（{t, v, n, obj…}）→ 普通 JSON 值；表只给项数，不再往下展开。 */
	TSharedPtr<FJsonValue> DescToPlain(const FJsonObject& InDesc)
	{
		FString Type;
		InDesc.TryGetStringField(TEXT("t"), Type);
		FString Obj;
		if (InDesc.TryGetStringField(TEXT("obj"), Obj) && Type == TEXT("userdata"))
		{
			return MakeShared<FJsonValueString>(Obj);
		}
		if (Type == TEXT("table"))
		{
			double Count = 0;
			InDesc.TryGetNumberField(TEXT("n"), Count);
			const TSharedPtr<FJsonObject> Marker = MakeShared<FJsonObject>();
			Marker->SetNumberField(TEXT("$table"), Count);
			if (!Obj.IsEmpty())
			{
				Marker->SetStringField(TEXT("$obj"), Obj);
			}
			return MakeShared<FJsonValueObject>(Marker);
		}
		const TSharedPtr<FJsonValue> V = InDesc.TryGetField(TEXT("v"));
		if (Type == TEXT("nil") || !V.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}
		return V;
	}
}

FString FGameTestLuaValue::Type() const
{
	FString Type;
	if (Desc.IsValid())
	{
		Desc->TryGetStringField(TEXT("t"), Type);
	}
	return Type;
}

TSharedPtr<FJsonValue> FGameTestLuaValue::ToPlain() const
{
	if (!Desc.IsValid())
	{
		return MakeShared<FJsonValueNull>();
	}
	if (Type() != TEXT("table"))
	{
		return DescToPlain(*Desc);
	}
	// 键全是 1..n 的连续整数时当数组（Lua 的数组习惯），否则当对象
	bool bArray = Fields.Num() > 0;
	for (int32 Index = 0; Index < Fields.Num() && bArray; ++Index)
	{
		const TSharedPtr<FJsonObject> Field = Fields[Index].IsValid() ? Fields[Index]->AsObject() : nullptr;
		double KeyNumber = 0;
		bArray = Field.IsValid() && Field->TryGetNumberField(TEXT("kn"), KeyNumber) && KeyNumber == Index + 1;
	}
	if (bArray)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const TSharedPtr<FJsonValue>& Entry : Fields)
		{
			Items.Add(DescToPlain(*Entry->AsObject()));
		}
		return MakeShared<FJsonValueArray>(Items);
	}
	const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	for (const TSharedPtr<FJsonValue>& Entry : Fields)
	{
		const TSharedPtr<FJsonObject> Field = Entry.IsValid() ? Entry->AsObject() : nullptr;
		FString Key;
		if (Field.IsValid() && Field->TryGetStringField(TEXT("k"), Key))
		{
			Object->SetField(Key, DescToPlain(*Field));
		}
	}
	return MakeShared<FJsonValueObject>(Object);
}

FGameTestValue FGameTestLuaValue::ToValue() const
{
	FString ObjectPath;
	if (Desc.IsValid() && Desc->TryGetStringField(TEXT("obj"), ObjectPath) && !ObjectPath.IsEmpty())
	{
		// Lua 里拿到的是 UObject（代理或绑定的实例表）：还原成对象，能继续按反射取成员、当 invoke 的目标
		if (UObject* Object = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return FGameTestValue::FromObject(Object);
		}
	}
	return FGameTestValue::FromJson(ToPlain());
}

bool GameTestLuaBridge::IsAvailable()
{
	return FAgentPanelBridge::Get().HasOp(TEXT("lua.get"));
}

bool GameTestLuaBridge::Read(UWorld* InWorld, UObject* InSelf, const TArray<TSharedPtr<FJsonValue>>& InKeys, FGameTestLuaValue& OutValue, FString& OutError,
	bool bInFunctions)
{
	if (!IsAvailable())
	{
		OutError = TEXT("项目没有 UnLua（UELink 没编进 Lua 支持），读不了 Lua");
		return false;
	}
	const TSharedRef<FJsonObject> Args = BaseArgs(InWorld, InSelf);
	Args->SetArrayField(TEXT("keys"), InKeys);
	Args->SetNumberField(TEXT("limit"), MaxFields);
	Args->SetBoolField(TEXT("functions"), bInFunctions);
	TSharedPtr<FJsonObject> Out;
	if (!FAgentPanelBridge::Get().CallOp(TEXT("lua.get"), Args, Out, OutError))
	{
		return false;
	}
	OutValue = FromGetResult(Out);
	return true;
}

bool GameTestLuaBridge::Write(UWorld* InWorld, UObject* InSelf, const TArray<TSharedPtr<FJsonValue>>& InKeys, const TSharedPtr<FJsonValue>& InValue,
	FString& OutError)
{
	if (!IsAvailable())
	{
		OutError = TEXT("项目没有 UnLua（UELink 没编进 Lua 支持），写不了 Lua");
		return false;
	}
	const TSharedRef<FJsonObject> Args = BaseArgs(InWorld, InSelf);
	Args->SetArrayField(TEXT("keys"), InKeys);
	Args->SetField(TEXT("value"), InValue.IsValid() ? InValue : MakeShared<FJsonValueNull>());
	TSharedPtr<FJsonObject> Out;
	return FAgentPanelBridge::Get().CallOp(TEXT("lua.set"), Args, Out, OutError);
}

bool GameTestLuaBridge::ReadModule(UWorld* InWorld, const FString& InModule, const TArray<TSharedPtr<FJsonValue>>& InKeys, FGameTestLuaValue& OutValue, FString& OutError)
{
	bool bLoaded = false;
	if (!IsModuleLoaded(InWorld, InModule, bLoaded, OutError))
	{
		return false;
	}
	if (!bLoaded)
	{
		OutError = FString::Printf(TEXT("Lua 模块 %s 还没 require（GameTest 读值不主动加载模块）"), *InModule);
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Keys = {
		MakeShared<FJsonValueString>(TEXT("package")), MakeShared<FJsonValueString>(TEXT("loaded")), MakeShared<FJsonValueString>(InModule) };
	Keys.Append(InKeys);
	return Read(InWorld, nullptr, Keys, OutValue, OutError);
}

bool GameTestLuaBridge::IsModuleLoaded(UWorld* InWorld, const FString& InModule, bool& bOutLoaded, FString& OutError)
{
	FGameTestLuaValue Value;
	const TArray<TSharedPtr<FJsonValue>> Keys = {
		MakeShared<FJsonValueString>(TEXT("package")), MakeShared<FJsonValueString>(TEXT("loaded")), MakeShared<FJsonValueString>(InModule) };
	if (!Read(InWorld, nullptr, Keys, Value, OutError))
	{
		return false;
	}
	bOutLoaded = !Value.IsNil();
	return true;
}

bool GameTestLuaBridge::Call(UWorld* InWorld, UObject* InSelf, const FString& InModule, const TArray<TSharedPtr<FJsonValue>>& InFunctionKeys,
	const TArray<TSharedPtr<FJsonValue>>& InArgs, TArray<FGameTestLuaValue>& OutResults, FString& OutError)
{
	if (!IsAvailable())
	{
		OutError = TEXT("项目没有 UnLua（UELink 没编进 Lua 支持），调不了 Lua 函数");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Keys;
	if (!InModule.IsEmpty())
	{
		bool bLoaded = false;
		if (!IsModuleLoaded(InWorld, InModule, bLoaded, OutError))
		{
			return false;
		}
		if (!bLoaded)
		{
			OutError = FString::Printf(TEXT("Lua 模块 %s 还没 require，先让游戏加载它（或在脚本里调用会 require 它的函数）"), *InModule);
			return false;
		}
		Keys = { MakeShared<FJsonValueString>(TEXT("package")), MakeShared<FJsonValueString>(TEXT("loaded")), MakeShared<FJsonValueString>(InModule) };
	}
	Keys.Append(InFunctionKeys);

	const TSharedRef<FJsonObject> Args = BaseArgs(InWorld, InModule.IsEmpty() ? InSelf : nullptr);
	Args->SetArrayField(TEXT("keys"), Keys);
	Args->SetArrayField(TEXT("args"), InArgs);
	// 实例表上的函数按方法调：Lua 里写的是 self:Fn()
	Args->SetBoolField(TEXT("self"), InSelf != nullptr && InModule.IsEmpty());
	Args->SetNumberField(TEXT("timeoutMs"), CallTimeoutMs);
	TSharedPtr<FJsonObject> Out;
	if (!FAgentPanelBridge::Get().CallOp(TEXT("lua.call"), Args, Out, OutError))
	{
		return false;
	}
	OutResults.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
	if (Out.IsValid() && Out->TryGetArrayField(TEXT("results"), Results) && Results != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Result : *Results)
		{
			const TSharedPtr<FJsonObject> Desc = Result.IsValid() ? Result->AsObject() : nullptr;
			FGameTestLuaValue Value;
			Value.Desc = Desc;
			const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
			if (Desc.IsValid() && Desc->TryGetArrayField(TEXT("fields"), Fields) && Fields != nullptr)
			{
				Value.Fields = *Fields;
			}
			OutResults.Add(MoveTemp(Value));
		}
	}
	return true;
}

uint64 GameTestLuaBridge::ErrorSeq()
{
	if (!FAgentPanelBridge::Get().HasOp(TEXT("lua.errors")))
	{
		return 0;
	}
	const TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetNumberField(TEXT("limit"), 1);
	TSharedPtr<FJsonObject> Out;
	FString Ignore;
	double Seq = 0;
	if (FAgentPanelBridge::Get().CallOp(TEXT("lua.errors"), Args, Out, Ignore) && Out.IsValid())
	{
		Out->TryGetNumberField(TEXT("seq"), Seq);
	}
	return (uint64)Seq;
}

bool GameTestLuaBridge::Errors(uint64 InSince, int32 InLimit, TArray<TSharedPtr<FJsonValue>>& OutItems, FString& OutError)
{
	OutItems.Reset();
	if (!FAgentPanelBridge::Get().HasOp(TEXT("lua.errors")))
	{
		// 没有 Lua：没有 Lua 报错，不算错
		return true;
	}
	const TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetNumberField(TEXT("since"), (double)InSince);
	Args->SetNumberField(TEXT("limit"), FMath::Clamp(InLimit, 1, 200));
	TSharedPtr<FJsonObject> Out;
	if (!FAgentPanelBridge::Get().CallOp(TEXT("lua.errors"), Args, Out, OutError))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (Out.IsValid() && Out->TryGetArrayField(TEXT("items"), Items) && Items != nullptr)
	{
		OutItems = *Items;
	}
	return true;
}

TArray<TSharedPtr<FJsonValue>> GameTestLuaBridge::SplitPath(const FString& InPath)
{
	TArray<TSharedPtr<FJsonValue>> Keys;
	FString Current;
	auto Flush = [&Keys, &Current]()
	{
		if (!Current.IsEmpty())
		{
			Keys.Add(MakeShared<FJsonValueString>(Current));
			Current.Reset();
		}
	};
	for (int32 Index = 0; Index < InPath.Len(); ++Index)
	{
		const TCHAR Ch = InPath[Index];
		if (Ch == TEXT('.'))
		{
			Flush();
		}
		else if (Ch == TEXT('['))
		{
			Flush();
			int32 Close = InPath.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index);
			if (Close == INDEX_NONE)
			{
				Close = InPath.Len();
			}
			FString Inner = InPath.Mid(Index + 1, Close - Index - 1).TrimStartAndEnd();
			if (Inner.StartsWith(TEXT("\"")) || Inner.StartsWith(TEXT("'")))
			{
				Keys.Add(MakeShared<FJsonValueString>(Inner.Mid(1, Inner.Len() - 2)));
			}
			else if (Inner.IsNumeric())
			{
				// Lua 路径里的下标按 Lua 的键原样传（数组从 1 起），和脚本变量 $var[0] 从 0 数不同：读的是 Lua 表本身
				Keys.Add(MakeShared<FJsonValueNumber>(FCString::Atod(*Inner)));
			}
			else
			{
				Keys.Add(MakeShared<FJsonValueString>(Inner));
			}
			Index = Close;
		}
		else
		{
			Current.AppendChar(Ch);
		}
	}
	Flush();
	return Keys;
}
