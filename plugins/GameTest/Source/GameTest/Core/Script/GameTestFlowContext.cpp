// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Script/GameTestFlowContext.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"

void FGameTestScriptScope::AddObjects(const FJsonObject& InObjects)
{
	for (const auto& Pair : InObjects.Values)
	{
		const FString Name = GAMETEST_JSON_KEY(Pair.Key);
		const TSharedPtr<FJsonObject>* Def = nullptr;
		FString Source;
		if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Def) && Def != nullptr && Def->IsValid())
		{
			ObjectDefs.Add(Name, *Def);
		}
		else if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String && Pair.Value->TryGetString(Source) && Source.TrimStartAndEnd().StartsWith(TEXT("@")))
		{
			ObjectSources.Add(Name, Source.TrimStartAndEnd());
		}
	}
}

FGameTestFlowContext::FGameTestFlowContext()
{
	// 总有一层作用域：临时步骤、gt.resolve 不压脚本也能 set / 读变量，扩展点不用判「有没有作用域」。
	Scopes.AddDefaulted();
}

bool FGameTestFlowContext::PushScript(const FString& InScript)
{
	// 第 0 层是构造时放的底，主场景占第 1 层；call 的嵌套层数 = 总层数 - 2。
	if (Scopes.Num() - 1 >= FMath::Max(1, Options.MaxCallDepth) + 1)
	{
		return false;
	}
	FGameTestScriptScope& Scope = Scopes.AddDefaulted_GetRef();
	Scope.Script = InScript;
	return true;
}

void FGameTestFlowContext::PopScript()
{
	if (Scopes.Num() > 1)
	{
		Scopes.Pop();
	}
}

FGameTestScriptScope& FGameTestFlowContext::GetScope()
{
	return Scopes.Last();
}

const FGameTestScriptScope& FGameTestFlowContext::GetScope() const
{
	return Scopes.Last();
}

void FGameTestFlowContext::PushLocals()
{
	GetScope().Locals.AddDefaulted();
}

void FGameTestFlowContext::PopLocals()
{
	TArray<TMap<FString, FGameTestValue>>& Locals = GetScope().Locals;
	if (Locals.Num() > 0)
	{
		Locals.Pop();
	}
}

void FGameTestFlowContext::SetLocal(const FString& InName, const FGameTestValue& InValue)
{
	TArray<TMap<FString, FGameTestValue>>& Locals = GetScope().Locals;
	if (Locals.Num() == 0)
	{
		GetScope().Vars.Add(InName, InValue);
		return;
	}
	Locals.Last().Add(InName, InValue);
}

bool FGameTestFlowContext::FindVar(const FString& InName, FGameTestValue& OutValue) const
{
	const FGameTestScriptScope& Scope = GetScope();
	for (int32 Index = Scope.Locals.Num() - 1; Index >= 0; --Index)
	{
		if (const FGameTestValue* Found = Scope.Locals[Index].Find(InName))
		{
			OutValue = *Found;
			return true;
		}
	}
	if (const FGameTestValue* Found = Scope.Vars.Find(InName))
	{
		OutValue = *Found;
		return true;
	}
	if (const FGameTestValue* Found = Scope.Params.Find(InName))
	{
		OutValue = *Found;
		return true;
	}
	return false;
}

void FGameTestFlowContext::SetVar(const FString& InName, const FGameTestValue& InValue)
{
	FGameTestScriptScope& Scope = GetScope();
	for (int32 Index = Scope.Locals.Num() - 1; Index >= 0; --Index)
	{
		if (FGameTestValue* Found = Scope.Locals[Index].Find(InName))
		{
			*Found = InValue;
			return;
		}
	}
	Scope.Vars.Add(InName, InValue);
}

TSharedPtr<FJsonObject> FGameTestFlowContext::SnapshotVars(int32 InMaxChars) const
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	const FGameTestScriptScope& Scope = GetScope();
	int32 Used = 0;
	bool bTruncated = false;
	// 内层的局部最先给，和 FindVar 的优先级一致：调试时看到的就是脚本此刻读到的。
	auto AddAll = [&Out, &Used, &bTruncated, InMaxChars](const TMap<FString, FGameTestValue>& InMap)
	{
		for (const TPair<FString, FGameTestValue>& Pair : InMap)
		{
			if (Out->HasField(Pair.Key))
			{
				continue;
			}
			const FString Shown = Pair.Value.ToDisplay(500);
			if (InMaxChars > 0 && Used + Shown.Len() + Pair.Key.Len() > InMaxChars)
			{
				bTruncated = true;
				return;
			}
			Used += Shown.Len() + Pair.Key.Len();
			Out->SetField(Pair.Key, Pair.Value.ToJson());
		}
	};
	for (int32 Index = Scope.Locals.Num() - 1; Index >= 0 && !bTruncated; --Index)
	{
		AddAll(Scope.Locals[Index]);
	}
	if (!bTruncated)
	{
		AddAll(Scope.Vars);
	}
	if (!bTruncated)
	{
		AddAll(Scope.Params);
	}
	if (bTruncated)
	{
		Out->SetBoolField(TEXT("_truncated"), true);
	}
	return Out;
}

FGameTestFlowContext::FReadOnlyScope::FReadOnlyScope(FGameTestFlowContext& InContext)
	: Context(InContext)
{
	++Context.ReadOnlyDepth;
}

FGameTestFlowContext::FReadOnlyScope::~FReadOnlyScope()
{
	--Context.ReadOnlyDepth;
}
