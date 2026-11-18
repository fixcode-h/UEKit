// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Observe/Checks/GameTestCheck.h"
#include "Observe/Checks/GameTestCond.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * 判断：常用名 `{"alias": "金币", "gte": 500}`，展开成场景根 `_vars.json` 里的定义（协议第 9 节）。
 * 定义可以是一条判断或一棵条件树；别名上写的比较盖在定义上（定义写 `{"value": "@ps.Gold"}`，用的地方写 `gte`）。
 * `_vars.json` 按修改时间缓存，改了下次判断就生效。
 */
namespace
{
	struct FVarsCache
	{
		FString Path;
		FDateTime Stamp;
		TSharedPtr<FJsonObject> Root;
	};

	TSharedPtr<FJsonObject> LoadVars(const FString& InScenarioRoot, FString& OutError)
	{
		static FVarsCache Cache;
		const FString Path = FPaths::Combine(InScenarioRoot, TEXT("_vars.json"));
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Path);
		if (Stamp == FDateTime::MinValue())
		{
			OutError = FString::Printf(TEXT("没有 %s（常用名写在场景根的 _vars.json 里）"), *Path);
			return nullptr;
		}
		if (Cache.Path == Path && Cache.Stamp == Stamp && Cache.Root.IsValid())
		{
			return Cache.Root;
		}
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			OutError = FString::Printf(TEXT("%s 不是合法的 JSON"), *Path);
			return nullptr;
		}
		Cache = { Path, Stamp, Root };
		return Root;
	}

	/** 定义 + 别名上的比较 → 要判断的对象。 */
	TSharedPtr<FJsonObject> ExpandAlias(const FJsonObject& InJson, FGameTestFlowContext& InContext, FString& OutError)
	{
		FString Name;
		InJson.TryGetStringField(TEXT("alias"), Name);
		const TSharedPtr<FJsonObject> Vars = LoadVars(InContext.ScenarioRoot, OutError);
		if (!Vars.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Def = nullptr;
		if (!Vars->TryGetObjectField(Name, Def) || !Def || !Def->IsValid())
		{
			OutError = FString::Printf(TEXT("_vars.json 里没有常用名「%s」"), *Name);
			return nullptr;
		}
		if ((*Def)->HasField(TEXT("alias")))
		{
			OutError = FString::Printf(TEXT("常用名「%s」的定义里又引用了常用名：定义里直接写判断"), *Name);
			return nullptr;
		}
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const auto& Pair : (*Def)->Values)
		{
			Out->SetField(GAMETEST_JSON_KEY(Pair.Key), Pair.Value);
		}
		for (const FString& Key : GameTestCompare::Keys())
		{
			if (const TSharedPtr<FJsonValue> Value = InJson.TryGetField(Key))
			{
				Out->SetField(Key, Value);
			}
		}
		return Out;
	}

	class FCheckAlias : public IGameTestCheck
	{
	public:
		virtual FName GetKey() const override { return TEXT("alias"); }
		virtual FString GetSummary() const override { return TEXT("常用名（_vars.json 里定义的判断）"); }
		virtual bool Matches(const FJsonObject& InJson) const override { return InJson.HasField(TEXT("alias")); }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("alias") };
			return Fields;
		}

		virtual void Validate(const FJsonObject& InJson, FGameTestDiagSink& InSink) const override
		{
			FString Name;
			if (!InJson.TryGetStringField(TEXT("alias"), Name) || Name.IsEmpty())
			{
				InSink.Error(TEXT("bad_type"), TEXT("alias 要写常用名"));
			}
		}

		virtual void Evaluate(const FJsonObject& InJson, FGameTestFlowContext& InContext, FGameTestCheckResult& OutResult) const override
		{
			FString Error;
			const TSharedPtr<FJsonObject> Expanded = ExpandAlias(InJson, InContext, Error);
			if (!Expanded.IsValid())
			{
				OutResult.Unresolved = Error;
				return;
			}
			const FGameTestCondResult Result = GameTestCond::Evaluate(*Expanded, InContext);
			OutResult.bHolds = Result.Holds();
			OutResult.bScreenAbsent = Result.State == EGameTestCondState::Unknown;
			OutResult.Actual = Result.Parts.Num() > 0 ? Result.Parts[0].Actual : FGameTestValue::Null();
			OutResult.Say = Result.Summary();
		}

		virtual bool IsNegative(const FJsonObject& InJson) const override
		{
			// 展开后的反面判断已经在 Evaluate 里算成 bScreenAbsent，这里只要让条件树按「还不能判断」处理。
			return true;
		}

		virtual FString Describe(const FJsonObject& InJson) const override
		{
			FString Name;
			InJson.TryGetStringField(TEXT("alias"), Name);
			return GameTestCompare::HasAny(InJson) ? FString::Printf(TEXT("「%s」%s"), *Name, *GameTestCompare::Describe(InJson))
												   : FString::Printf(TEXT("「%s」"), *Name);
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCheckAlias()
	{
		GameTestChecks::Get().Register(MakeShared<FCheckAlias>());
	}
}
