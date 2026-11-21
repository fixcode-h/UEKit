// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Expr/GameTestExpr.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Checks/GameTestCheck.h"
#include "Observe/Compare/GameTestCompareRegistry.h"
#include "Observe/Compare/GameTestJudge.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRef.h"
#include "Observe/Ref/GameTestRefResolve.h"

/**
 * 判断：对象的属性或状态（docs/game-test-ref.md 第 4 节）
 * `{"obj": "shop", "prop": "@ChkAutoBuy.IsChecked", "op": "is", "value": "Checked"}`。
 *
 * 一个对象引用 + 一个判断项，`prop` 省略 = `:exists is true`。
 * 反面判断（:exists / :onScreen / :shown 为 false）：部件层的在对象本身找不到时记「还不能判断」（和 game-test-script 4.2 同一条规则）；
 * 对象层的 `:exists false` 在对象找不到时就是成立（引用的类有实例但 ident 都对不上也算不存在）。
 */
namespace GameTestCheckObjImpl
{
	const TArray<FString>& ObjFields()
	{
		static const TArray<FString> Fields = { TEXT("obj"), TEXT("prop"), TEXT("op"), TEXT("value"), TEXT("tol"), TEXT("ignoreCase"), TEXT("cmp") };
		return Fields;
	}

	/** 期望值是 false（写了 false，或 op 省略且 value 是 false）。 */
	bool ExpectsFalse(const FJsonObject& InJson)
	{
		bool bValue = true;
		FString Op;
		InJson.TryGetStringField(TEXT("op"), Op);
		return (Op.IsEmpty() || Op == TEXT("is")) && InJson.TryGetBoolField(TEXT("value"), bValue) && !bValue;
	}

	FString PropOf(const FJsonObject& InJson)
	{
		FString Prop;
		InJson.TryGetStringField(TEXT("prop"), Prop);
		return Prop.IsEmpty() ? FString(TEXT("@:exists")) : Prop;
	}

	FString RefText(const FJsonObject& InJson)
	{
		FGameTestTarget Target;
		Target.Obj = InJson.TryGetField(TEXT("obj"));
		return Target.Describe();
	}

	class FCheckObj final : public IGameTestCheck
	{
	public:
		virtual FName GetKey() const override { return TEXT("obj"); }
		virtual FString GetSummary() const override { return TEXT("对象的属性或算出状态（引用 + 判断项）"); }
		virtual bool Matches(const FJsonObject& InJson) const override { return InJson.HasField(TEXT("obj")); }
		virtual const TArray<FString>& GetFields() const override { return ObjFields(); }
		virtual bool UsesCompareKeys() const override { return false; }

		virtual void ValidateIn(const FJsonObject& InJson, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink) const override
		{
			{
				FGameTestDiagSink::FScope Scope(InSink, TEXT("obj"));
				GameTestRefCheck::Ref(InJson.TryGetField(TEXT("obj")), InScope, InSink);
			}
			GameTestRefCheck::Ident(InJson, InScope, InSink, /*bInPropRequired=*/false);
		}

		virtual void Evaluate(const FJsonObject& InJson, FGameTestFlowContext& InContext, FGameTestCheckResult& OutResult) const override
		{
			FGameTestIdentItem Item = FGameTestIdentItem::FromJson(InJson);
			FString PropText = PropOf(InJson);
			// prop 里可以嵌 `$变量`（被 call 的子脚本按参数读不同部件：`@Claim_$what:text`），按文字替换，规则同 part
			if (PropText.Contains(TEXT("$")))
			{
				FString Expanded;
				FString EvalError;
				if (!GameTestRefs::EvalText(PropText, InContext, Expanded, EvalError))
				{
					OutResult.Unresolved = FString::Printf(TEXT("「%s」取不到值：%s"), *PropText, *EvalError);
					return;
				}
				PropText = Expanded;
				Item.Prop = Expanded;
			}
			const FGameTestRefResult Ref = GameTestRefResolve::Resolve(InJson.TryGetField(TEXT("obj")), InContext);
			FGameTestPropPath Path;
			FString PathError;
			const bool bParsed = FGameTestPropPath::Parse(PropText, Path, PathError);
			const bool bObjectLevelExists = bParsed && !Path.bItem && Path.Segs.Num() == 0 && Path.State.Equals(TEXT("exists"), ESearchCase::IgnoreCase);
			if (!Ref.IsOk() && Ref.Code != TEXT("ref_not_found"))
			{
				// 歧义、类写错、组件没写 in：写法或场景问题，不当成「不存在」
				OutResult.Unresolved = Ref.Error;
				return;
			}
			const FGameTestJudgeResult Judged = GameTestJudge::Test(Ref.Get(), Item, InContext, Ref.GetItem());
			OutResult.Actual = Judged.Read.Value;
			OutResult.bHolds = Judged.bHolds;
			if (!Ref.IsOk() && !bObjectLevelExists)
			{
				// 对象本身不在：部件层的反面判断不算数（界面还没打开 ≠ 按钮不在）
				OutResult.bScreenAbsent = true;
				OutResult.Unresolved = Ref.Error;
				return;
			}
			if (Judged.bUnresolved)
			{
				OutResult.Unresolved = Judged.Why;
				return;
			}
			OutResult.Say = Judged.bHolds ? FString::Printf(TEXT("%s 的 %s = %s"), *RefText(InJson), *PropText, *Judged.Read.Value.ToDisplay(60))
										  : FString::Printf(TEXT("%s 的 %s"), *RefText(InJson), *Judged.Why);
		}

		virtual bool IsNegative(const FJsonObject& InJson) const override
		{
			FGameTestPropPath Path;
			FString Error;
			if (!FGameTestPropPath::Parse(PropOf(InJson), Path, Error))
			{
				return false;
			}
			const bool bPresence = Path.State.Equals(TEXT("exists"), ESearchCase::IgnoreCase) || Path.State.Equals(TEXT("onScreen"), ESearchCase::IgnoreCase)
				|| Path.State.Equals(TEXT("shown"), ESearchCase::IgnoreCase);
			return bPresence && ExpectsFalse(InJson);
		}

		virtual FString Describe(const FJsonObject& InJson) const override
		{
			FGameTestIdentItem Item = FGameTestIdentItem::FromJson(InJson);
			Item.Prop = PropOf(InJson);
			return FString::Printf(TEXT("%s 的 %s"), *RefText(InJson), *Item.Describe());
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCheckObj()
	{
		GameTestChecks::Get().Register(MakeShared<GameTestCheckObjImpl::FCheckObj>());
	}
}
