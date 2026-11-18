// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Script/GameTestDiag.h"

#include "Dom/JsonObject.h"

TSharedPtr<FJsonObject> FGameTestDiag::ToJson() const
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	if (!Id.IsEmpty())
	{
		Out->SetStringField(TEXT("id"), Id);
	}
	Out->SetStringField(TEXT("path"), Path);
	Out->SetStringField(TEXT("code"), Code.ToString());
	Out->SetStringField(TEXT("message"), Message);
	return Out;
}

FString FGameTestDiag::ToLine() const
{
	const FString Where = Id.IsEmpty() ? Path : FString::Printf(TEXT("%s（%s）"), *Path, *Id);
	return Where.IsEmpty() ? Message : FString::Printf(TEXT("%s：%s"), *Where, *Message);
}

FGameTestDiagSink::FScope::FScope(FGameTestDiagSink& InSink, const FString& InSegment, const FString& InId, EScopeMode InMode)
	: Sink(InSink)
	, PrevPath(InSink.Path)
	, PrevRootLen(InSink.RootLen)
	, PrevId(InSink.CurrentId)
{
	if (InMode == EScopeMode::FromRoot)
	{
		Sink.Path.LeftInline(Sink.RootLen);
	}
	if (!InSegment.IsEmpty())
	{
		Sink.Path += Sink.Path.IsEmpty() ? InSegment : TEXT(".") + InSegment;
	}
	if (InMode == EScopeMode::NewRoot)
	{
		Sink.RootLen = Sink.Path.Len();
	}
	if (!InId.IsEmpty())
	{
		Sink.CurrentId = InId;
	}
}

FGameTestDiagSink::FScope::~FScope()
{
	Sink.Path = PrevPath;
	Sink.RootLen = PrevRootLen;
	Sink.CurrentId = PrevId;
}

void FGameTestDiagSink::Error(FName InCode, const FString& InMessage)
{
	Add(InCode, InMessage, false);
}

void FGameTestDiagSink::Warning(FName InCode, const FString& InMessage)
{
	Add(InCode, InMessage, true);
}

void FGameTestDiagSink::Add(FName InCode, const FString& InMessage, bool bWarning)
{
	FGameTestDiag& Diag = Items.AddDefaulted_GetRef();
	Diag.Id = CurrentId;
	Diag.Path = Path;
	Diag.Code = InCode;
	Diag.Message = InMessage;
	Diag.bWarning = bWarning;
	if (!bWarning)
	{
		++ErrorCount;
	}
}

void FGameTestDiagSink::AppendErrorLines(TArray<FString>& OutLines) const
{
	for (const FGameTestDiag& Diag : Items)
	{
		if (!Diag.bWarning)
		{
			OutLines.Add(Diag.ToLine());
		}
	}
}
