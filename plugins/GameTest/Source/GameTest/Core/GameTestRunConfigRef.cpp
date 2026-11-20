// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestRunConfigRef.h"

#include "Core/GameTestRunConfig.h"
#include "Core/Script/GameTestScriptOptions.h"
#include "GameTestCompat.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace GameTestRunConfigRefImpl
{
	/** 规则数组：每条要有 class，`InArrayField` 是规则里必须有的数组字段（ident / how）。 */
	bool CheckRules(const FJsonObject& InJson, const TCHAR* InField, const TCHAR* InArrayField, FString& OutError)
	{
		if (!InJson.HasField(InField))
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Rules = nullptr;
		if (!InJson.TryGetArrayField(InField, Rules) || Rules == nullptr)
		{
			OutError = FString::Printf(TEXT("%s 要写成数组 [{\"class\": 类名, \"%s\": [...]}]"), InField, InArrayField);
			return false;
		}
		for (int32 Index = 0; Index < Rules->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Rule = nullptr;
			FString Class;
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (!(*Rules)[Index].IsValid() || !(*Rules)[Index]->TryGetObject(Rule) || Rule == nullptr || !Rule->IsValid()
				|| !(*Rule)->TryGetStringField(TEXT("class"), Class) || Class.IsEmpty() || !(*Rule)->TryGetArrayField(InArrayField, Items) || Items == nullptr)
			{
				OutError = FString::Printf(TEXT("%s 第 %d 条要写 class（类名）和 %s（数组）"), InField, Index + 1, InArrayField);
				return false;
			}
		}
		return true;
	}

	bool ReadTol(const FJsonObject& InTol, const TCHAR* InKey, double& OutTol, FString& OutError)
	{
		if (!InTol.HasField(InKey))
		{
			return true;
		}
		double Value = 0.0;
		if (!InTol.TryGetNumberField(InKey, Value) || Value < 0.0)
		{
			OutError = FString::Printf(TEXT("refDefaults.tol.%s 要写非负数字"), InKey);
			return false;
		}
		OutTol = Value;
		return true;
	}
}

bool GameTestRunConfigRef::Parse(const FJsonObject& InJson, FGameTestScriptOptions& OutOptions, FString& OutError)
{
	using namespace GameTestRunConfigRefImpl;
	for (const TCHAR* Old : { TEXT("itemKeys"), TEXT("objectKeys") })
	{
		if (InJson.HasField(Old))
		{
			OutError = FString::Printf(TEXT("settings_old：运行设置里还有 1.x 的 %s，2.0.0 起改成 identRules。请在面板里用「升级脚本」一起迁移（会先备份）"), Old);
			return false;
		}
	}
	if (!CheckRules(InJson, TEXT("identRules"), TEXT("ident"), OutError) || !CheckRules(InJson, TEXT("closeRules"), TEXT("how"), OutError))
	{
		return false;
	}
	OutOptions.IdentRules = InJson.TryGetField(TEXT("identRules"));
	OutOptions.CloseRules = InJson.TryGetField(TEXT("closeRules"));
	if (InJson.HasField(TEXT("allowDestroy")) && !InJson.TryGetBoolField(TEXT("allowDestroy"), OutOptions.bAllowDestroy))
	{
		OutError = TEXT("allowDestroy 要写 true / false");
		return false;
	}
	if (!InJson.HasField(TEXT("refDefaults")))
	{
		return true;
	}
	const TSharedPtr<FJsonObject>* Defaults = nullptr;
	if (!InJson.TryGetObjectField(TEXT("refDefaults"), Defaults) || Defaults == nullptr || !Defaults->IsValid())
	{
		OutError = TEXT("refDefaults 要写成对象 {many, ignoreCase, tol, closeVerifyMs}");
		return false;
	}
	const FJsonObject& Ref = **Defaults;
	FString Many;
	if (Ref.TryGetStringField(TEXT("many"), Many))
	{
		if (Many != TEXT("error") && Many != TEXT("first") && Many != TEXT("last") && Many != TEXT("nearest"))
		{
			OutError = TEXT("refDefaults.many 只能是 error、first、last、nearest");
			return false;
		}
		OutOptions.RefMany = Many;
	}
	if (Ref.HasField(TEXT("ignoreCase")) && !Ref.TryGetBoolField(TEXT("ignoreCase"), OutOptions.bRefIgnoreCase))
	{
		OutError = TEXT("refDefaults.ignoreCase 要写 true / false");
		return false;
	}
	double Verify = 0.0;
	if (Ref.HasField(TEXT("closeVerifyMs")))
	{
		if (!Ref.TryGetNumberField(TEXT("closeVerifyMs"), Verify) || Verify < 0.0 || Verify > 60000.0)
		{
			OutError = TEXT("refDefaults.closeVerifyMs 要写 0–60000 的毫秒数");
			return false;
		}
		OutOptions.CloseVerifyMs = FMath::RoundToInt(Verify);
	}
	const TSharedPtr<FJsonObject>* Tol = nullptr;
	if (Ref.TryGetObjectField(TEXT("tol"), Tol) && Tol != nullptr && Tol->IsValid())
	{
		return ReadTol(**Tol, TEXT("float"), OutOptions.TolFloat, OutError) && ReadTol(**Tol, TEXT("vector"), OutOptions.TolVector, OutError)
			&& ReadTol(**Tol, TEXT("rotator"), OutOptions.TolRotator, OutError) && ReadTol(**Tol, TEXT("color"), OutOptions.TolColor, OutError);
	}
	return true;
}

void GameTestRunConfigRef::Write(const FGameTestScriptOptions& InOptions, FJsonObject& OutJson)
{
	const FGameTestScriptOptions Defaults;
	if (InOptions.IdentRules.IsValid())
	{
		OutJson.SetField(TEXT("identRules"), InOptions.IdentRules);
	}
	if (InOptions.CloseRules.IsValid())
	{
		OutJson.SetField(TEXT("closeRules"), InOptions.CloseRules);
	}
	if (InOptions.bAllowDestroy != Defaults.bAllowDestroy)
	{
		OutJson.SetBoolField(TEXT("allowDestroy"), InOptions.bAllowDestroy);
	}
	const TSharedRef<FJsonObject> Ref = MakeShared<FJsonObject>();
	if (InOptions.RefMany != Defaults.RefMany)
	{
		Ref->SetStringField(TEXT("many"), InOptions.RefMany);
	}
	if (InOptions.bRefIgnoreCase != Defaults.bRefIgnoreCase)
	{
		Ref->SetBoolField(TEXT("ignoreCase"), InOptions.bRefIgnoreCase);
	}
	if (InOptions.CloseVerifyMs != Defaults.CloseVerifyMs)
	{
		Ref->SetNumberField(TEXT("closeVerifyMs"), InOptions.CloseVerifyMs);
	}
	const TSharedRef<FJsonObject> Tol = MakeShared<FJsonObject>();
	if (InOptions.TolFloat != Defaults.TolFloat)
	{
		Tol->SetNumberField(TEXT("float"), InOptions.TolFloat);
	}
	if (InOptions.TolVector != Defaults.TolVector)
	{
		Tol->SetNumberField(TEXT("vector"), InOptions.TolVector);
	}
	if (InOptions.TolRotator != Defaults.TolRotator)
	{
		Tol->SetNumberField(TEXT("rotator"), InOptions.TolRotator);
	}
	if (InOptions.TolColor != Defaults.TolColor)
	{
		Tol->SetNumberField(TEXT("color"), InOptions.TolColor);
	}
	if (Tol->Values.Num() > 0)
	{
		Ref->SetObjectField(TEXT("tol"), Tol);
	}
	if (Ref->Values.Num() > 0)
	{
		OutJson.SetObjectField(TEXT("refDefaults"), Ref);
	}
}

bool GameTestRunConfigRef::SaveIdentRule(const FString& InClass, const TArray<FString>& InProps, FString& OutError)
{
	FGameTestRunConfig Config;
	if (!GameTestRunConfig::Load(Config, OutError))
	{
		return false;
	}
	FString Wanted = InClass;
	Wanted.RemoveFromEnd(TEXT("_C"));
	TArray<TSharedPtr<FJsonValue>> Rules;
	const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
	if (Config.ScriptOptions.IdentRules.IsValid() && Config.ScriptOptions.IdentRules->TryGetArray(Existing) && Existing != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Existing)
		{
			const TSharedPtr<FJsonObject>* Rule = nullptr;
			FString Class;
			if (Value.IsValid() && Value->TryGetObject(Rule) && Rule != nullptr && Rule->IsValid() && (*Rule)->TryGetStringField(TEXT("class"), Class))
			{
				Class.RemoveFromEnd(TEXT("_C"));
				if (Class.Equals(Wanted, ESearchCase::IgnoreCase) && !(*Rule)->HasField(TEXT("in")))
				{
					continue;
				}
			}
			Rules.Add(Value);
		}
	}
	const TSharedRef<FJsonObject> Rule = MakeShared<FJsonObject>();
	Rule->SetStringField(TEXT("class"), Wanted);
	TArray<TSharedPtr<FJsonValue>> Props;
	for (const FString& Prop : InProps)
	{
		Props.Add(MakeShared<FJsonValueString>(Prop));
	}
	Rule->SetArrayField(TEXT("ident"), Props);
	Rules.Add(MakeShared<FJsonValueObject>(Rule));
	Config.ScriptOptions.IdentRules = MakeShared<FJsonValueArray>(Rules);
	return GameTestRunConfig::Save(Config, OutError);
}
