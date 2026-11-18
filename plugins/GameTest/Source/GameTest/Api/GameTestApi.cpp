// Copyright Epic Games, Inc. All Rights Reserved.

#include "Api/GameTestApi.h"

#include "Core/Script/GameTestBuiltins.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

TSharedPtr<FJsonObject> FGameTestApiDesc::ToJson() const
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("name"), Name);
	Out->SetStringField(TEXT("category"), Category);
	Out->SetStringField(TEXT("src"), Src);
	if (!Display.IsEmpty())
	{
		Out->SetStringField(TEXT("display"), Display);
	}
	Out->SetStringField(TEXT("sig"), Signature());
	TArray<TSharedPtr<FJsonValue>> ArgsJson;
	for (const FGameTestApiArg& Arg : Args)
	{
		TSharedPtr<FJsonObject> One = MakeShared<FJsonObject>();
		One->SetStringField(TEXT("name"), Arg.Name);
		One->SetStringField(TEXT("type"), Arg.Type);
		if (Arg.bOptional)
		{
			One->SetBoolField(TEXT("optional"), true);
		}
		if (!Arg.Note.IsEmpty())
		{
			One->SetStringField(TEXT("note"), Arg.Note);
		}
		ArgsJson.Add(MakeShared<FJsonValueObject>(One));
	}
	Out->SetArrayField(TEXT("args"), ArgsJson);
	if (!Returns.IsEmpty())
	{
		Out->SetStringField(TEXT("returns"), Returns);
	}
	Out->SetBoolField(TEXT("readOnly"), bReadOnly);
	if (bMayWait)
	{
		Out->SetBoolField(TEXT("waits"), true);
	}
	if (!Summary.IsEmpty())
	{
		Out->SetStringField(TEXT("note"), Summary);
	}
	if (!File.IsEmpty())
	{
		Out->SetStringField(TEXT("file"), File);
		Out->SetNumberField(TEXT("line"), Line);
	}
	return Out;
}

FString FGameTestApiDesc::Signature() const
{
	TArray<FString> Parts;
	for (const FGameTestApiArg& Arg : Args)
	{
		Parts.Add(Arg.bOptional ? Arg.Name + TEXT("?") : Arg.Name);
	}
	FString Out = FString::Printf(TEXT("%s(%s)"), *Name, *FString::Join(Parts, TEXT(", ")));
	if (!Returns.IsEmpty())
	{
		Out += TEXT(" → ") + Returns;
	}
	return Out;
}

const FGameTestValue& FGameTestApiCall::Arg(int32 InIndex) const
{
	static const FGameTestValue NullValue;
	return Args.IsValidIndex(InIndex) ? Args[InIndex] : NullValue;
}

EGameTestCallState FGameTestApiCall::Fail(FName InCode, const FString& InMessage)
{
	ErrorCode = InCode;
	Error = InMessage;
	return EGameTestCallState::Failed;
}

EGameTestCallState FGameTestApiCall::Done(const FGameTestValue& InResult)
{
	Result = InResult;
	return EGameTestCallState::Done;
}

FName IGameTestApi::GetKey() const
{
	return FName(*GetDesc().Name);
}

FGameTestLambdaApi::FGameTestLambdaApi(FGameTestApiDesc&& InDesc, FCallFn&& InFn)
	: Desc(MoveTemp(InDesc))
	, Fn(MoveTemp(InFn))
{
}

EGameTestCallState FGameTestLambdaApi::Call(FGameTestApiCall& InOutCall, FGameTestFlowContext& InContext) const
{
	if (!Fn)
	{
		return InOutCall.Fail(TEXT("fn_missing"), FString::Printf(TEXT("接口 %s 没有实现"), *Desc.Name));
	}
	return Fn(InOutCall, InContext);
}

TGameTestRegistry<IGameTestApi>& GameTestApis::Get()
{
	static TGameTestRegistry<IGameTestApi> Registry;
	GameTestBuiltins::EnsureRegistered();
	return Registry;
}

void GameTestApis::Add(const TCHAR* InName, const TCHAR* InCategory, const TCHAR* InSummary, TArray<FGameTestApiArg>&& InArgs,
	const TCHAR* InReturns, bool bReadOnly, FGameTestLambdaApi::FCallFn&& InFn, bool bMayWait)
{
	FGameTestApiDesc Desc;
	Desc.Name = InName;
	Desc.Category = InCategory;
	Desc.Summary = InSummary;
	Desc.Args = MoveTemp(InArgs);
	Desc.Returns = InReturns;
	Desc.bReadOnly = bReadOnly;
	Desc.bMayWait = bMayWait;
	Get().Register(MakeShared<FGameTestLambdaApi>(MoveTemp(Desc), MoveTemp(InFn)));
}

TMap<FString, FString>& GameTestApis::GetRenamed()
{
	static TMap<FString, FString> Renamed;
	return Renamed;
}

void GameTestApis::RemoveWhere(TFunctionRef<bool(const FGameTestApiDesc&)> InPredicate)
{
	TArray<FName> Doomed;
	for (const TSharedRef<IGameTestApi>& Api : Get().GetAll())
	{
		if (InPredicate(Api->GetDesc()))
		{
			Doomed.Add(Api->GetKey());
		}
	}
	for (const FName Key : Doomed)
	{
		Get().Unregister(Key);
	}
}

bool GameTestApis::BindArgs(const FGameTestApiDesc& InDesc, const TMap<FString, FGameTestValue>* InNamed,
	const TArray<FGameTestValue>* InPositional, TArray<FGameTestValue>& OutArgs, FName& OutCode, FString& OutError)
{
	OutArgs.Reset();
	OutArgs.SetNum(InDesc.Args.Num());
	if (InPositional)
	{
		if (InPositional->Num() > InDesc.Args.Num())
		{
			OutCode = TEXT("arg_type");
			OutError = FString::Printf(TEXT("%s 最多 %d 个参数，给了 %d 个：%s"), *InDesc.Name, InDesc.Args.Num(), InPositional->Num(), *InDesc.Signature());
			return false;
		}
		for (int32 Index = 0; Index < InPositional->Num(); ++Index)
		{
			OutArgs[Index] = (*InPositional)[Index];
		}
	}
	if (InNamed)
	{
		for (const TPair<FString, FGameTestValue>& Pair : *InNamed)
		{
			const int32 At = InDesc.Args.IndexOfByPredicate([&Pair](const FGameTestApiArg& Arg) { return Arg.Name.Equals(Pair.Key, ESearchCase::IgnoreCase); });
			if (At == INDEX_NONE)
			{
				OutCode = TEXT("unknown_field");
				OutError = FString::Printf(TEXT("%s 没有参数 %s：%s"), *InDesc.Name, *Pair.Key, *InDesc.Signature());
				return false;
			}
			OutArgs[At] = Pair.Value;
		}
	}
	const int32 Given = InPositional ? InPositional->Num() : 0;
	for (int32 Index = 0; Index < InDesc.Args.Num(); ++Index)
	{
		const bool bMissing = InNamed ? OutArgs[Index].IsNull() && !InNamed->Contains(InDesc.Args[Index].Name) : Index >= Given;
		if (bMissing && !InDesc.Args[Index].bOptional)
		{
			OutCode = TEXT("arg_missing");
			OutError = FString::Printf(TEXT("%s 缺参数 %s：%s"), *InDesc.Name, *InDesc.Args[Index].Name, *InDesc.Signature());
			return false;
		}
	}
	return true;
}

FGameTestValue GameTestApis::CallNow(const FString& InName, const TArray<FGameTestValue>& InPositional, FGameTestFlowContext& InContext)
{
	const IGameTestApi* Api = Get().Find(FName(*InName));
	if (!Api)
	{
		if (const FString* NewName = GetRenamed().Find(InName))
		{
			return FGameTestValue::Unresolved(FString::Printf(TEXT("接口 %s 已改名，改用 %s"), *InName, **NewName), TEXT("api_renamed"));
		}
		return FGameTestValue::Unresolved(FString::Printf(TEXT("没有接口 %s"), *InName), TEXT("api_missing"));
	}
	const FGameTestApiDesc& Desc = Api->GetDesc();
	if (InContext.IsReadOnly() && !Desc.bReadOnly)
	{
		return FGameTestValue::Unresolved(FString::Printf(TEXT("%s 会改游戏状态，不能写在条件或表达式里，改成 api 步骤"), *InName), TEXT("expr_not_readonly"));
	}
	if (Desc.bMayWait)
	{
		return FGameTestValue::Unresolved(FString::Printf(TEXT("%s 要等待，不能写在条件或表达式里，改成 api 步骤"), *InName), TEXT("expr_not_readonly"));
	}
	for (const FGameTestValue& Arg : InPositional)
	{
		if (Arg.IsUnresolved())
		{
			return Arg;
		}
	}
	FGameTestApiCall Call;
	FName Code;
	FString Error;
	if (!BindArgs(Desc, nullptr, &InPositional, Call.Args, Code, Error))
	{
		return FGameTestValue::Unresolved(Error, Code);
	}
	const EGameTestCallState State = Api->Call(Call, InContext);
	if (State == EGameTestCallState::Done)
	{
		return Call.Result;
	}
	if (State == EGameTestCallState::Pending)
	{
		// 声明了不等待却返回了「还在等」：实现有问题，收拾掉半路状态，按取不到算，不卡住条件判断。
		if (Call.Cleanup)
		{
			Call.Cleanup();
		}
		return FGameTestValue::Unresolved(FString::Printf(TEXT("%s 这次要等待，条件里不能用"), *InName), TEXT("expr_not_readonly"));
	}
	return FGameTestValue::Unresolved(Call.Error.IsEmpty() ? FString::Printf(TEXT("%s 调用失败"), *InName) : Call.Error,
		Call.ErrorCode.IsNone() ? FName(TEXT("unresolved")) : Call.ErrorCode);
}
