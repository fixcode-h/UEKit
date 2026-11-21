// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Sources/GameTestObjectLocator.h"

#include "Core/Script/GameTestDiag.h"
#include "Core/Expr/GameTestExpr.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestValue.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRefResolve.h"

bool FGameTestObjectLocator::Parse(const FJsonObject& InJson, FGameTestObjectLocator& OutLocator, TArray<FString>& OutErrors)
{
	FGameTestDiagSink Sink;
	FGameTestExprScope Scope;
	GameTestRefCheck::RefObject(InJson, Scope, Sink);
	if (Sink.HasErrors())
	{
		Sink.AppendErrorLines(OutErrors);
		return false;
	}
	if (!FGameTestRef::FromJson(InJson, OutLocator.Ref))
	{
		OutErrors.Add(TEXT("对象引用要写 class"));
		return false;
	}
	OutLocator.Class = OutLocator.Ref.Class;
	return true;
}

TSharedRef<FJsonObject> FGameTestObjectLocator::ToJson() const
{
	return Ref.ToJson();
}

FString FGameTestObjectLocator::Describe() const
{
	return Ref.Describe();
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
		Out->SetStringField(TEXT("picked"), GameTestRefResolve::DisplayName(Picked));
		Out->SetStringField(TEXT("by"), By);
	}
	Out->SetNumberField(TEXT("candidates"), Candidates);
	return Out;
}

EGameTestLocate GameTestObjectLocate::Locate(const FGameTestObjectLocator& InLocator, FGameTestFlowContext& InContext, FGameTestLocateResult& OutResult)
{
	FGameTestRef Ref = InLocator.Ref;
	if (Ref.Class.IsEmpty())
	{
		Ref.Class = InLocator.Class;
	}
	const FGameTestRefResult Result = GameTestRefResolve::ResolveRef(Ref, InContext);
	OutResult.Picked = Result.Get();
	OutResult.By = Result.By;
	OutResult.Candidates = Result.ClassCount;
	OutResult.Message = Result.Error;
	if (Result.IsOk())
	{
		return EGameTestLocate::Found;
	}
	if (Result.Code == TEXT("ref_ambiguous"))
	{
		return EGameTestLocate::Ambiguous;
	}
	return Result.Code == TEXT("ref_not_found") ? EGameTestLocate::NotFound : EGameTestLocate::Error;
}

bool GameTestObjectLocate::FindAll(const FGameTestObjectLocator& InLocator, FGameTestFlowContext& InContext, TArray<UObject*>& OutObjects, FString& OutError)
{
	FGameTestRef Ref = InLocator.Ref;
	if (Ref.Class.IsEmpty())
	{
		Ref.Class = InLocator.Class;
	}
	TArray<FGameTestRefCandidate> Candidates;
	FName Code;
	if (!GameTestRefResolve::Collect(Ref, InContext, Candidates, OutError, Code))
	{
		return false;
	}
	for (const FGameTestRefCandidate& Candidate : Candidates)
	{
		if (Candidate.Object.IsValid() && (Ref.Ident.Num() == 0 || GameTestRefResolve::MatchesIdent(Candidate, Ref.Ident, InContext)))
		{
			OutObjects.Add(Candidate.Object.Get());
		}
	}
	return true;
}

FGameTestValue GameTestObjectLocate::ResolveNamed(const FString& InName, FGameTestFlowContext& InContext)
{
	const FGameTestRefResult Result = GameTestRefResolve::Resolve(MakeShared<FJsonValueString>(InName), InContext);
	GameTestRefResolve::WritePayload(Result, InContext);
	if (!Result.IsOk())
	{
		return FGameTestValue::Unresolved(Result.Error, Result.Code.IsNone() ? FName(TEXT("ref_not_found")) : Result.Code);
	}
	return FGameTestValue::FromObject(Result.Get());
}

FGameTestValue GameTestObjectLocate::ReadKey(UObject* InObject, const FString& InPath, FGameTestFlowContext& InContext)
{
	// 不带 @ 的是属性名（Health、Stats.Level）；`#hp`、`/Comp.X`、`.X` 补上 @；`:状态`、`@self.X` 这类旧写法交给解析报 path_old
	const FString Path = InPath.StartsWith(TEXT("@")) || InPath.StartsWith(TEXT(":")) ? InPath
		: InPath.StartsWith(TEXT("#")) || InPath.StartsWith(TEXT("/")) || InPath.StartsWith(TEXT(".")) ? TEXT("@") + InPath
		: TEXT("@.") + InPath;
	return GameTestPropRead::ReadText(InObject, Path, InContext, GameTestPropRead::ListItemOf(InObject)).Value;
}
