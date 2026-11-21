// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Expr/GameTestExprFunction.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRefResolve.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * 表达式里读对象的属性：`prop(引用, "属性路径")`（docs/game-test-ref.md 第 4 节）。
 * 引用写 objects 里的名字（"shop"）、变量里的对象（$item）或内联引用的 JSON 文字（'{"class": "WBP_Toast"}'）。
 * 2.0.0 起取代按选择器读控件的 text(sel)、number(sel)、visible(sel)…（expr_removed）。
 */
namespace GameTestExprPropImpl
{
	/** 表达式参数 → 引用的 JSON 写法；对象值直接给。 */
	TSharedPtr<FJsonValue> RefOf(const FGameTestValue& InArg, UObject*& OutDirect)
	{
		OutDirect = InArg.GetObject();
		if (OutDirect != nullptr || !InArg.IsString())
		{
			return nullptr;
		}
		const FString Text = InArg.GetString().TrimStartAndEnd();
		if (Text.StartsWith(TEXT("{")))
		{
			TSharedPtr<FJsonValue> Parsed;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			return FJsonSerializer::Deserialize(Reader, Parsed) ? Parsed : nullptr;
		}
		return MakeShared<FJsonValueString>(Text);
	}
}

namespace GameTestBuiltin
{
	void RegisterExprProp()
	{
		GameTestExprFunctions::Add(TEXT("prop"), 2, 2, TEXT("对象"), TEXT("prop(引用, \"@/子控件:状态\")"),
			TEXT("读对象的属性或算出状态（引用写 objects 里的名字、$变量 或内联引用的 JSON 文字）；对象不在时 @:exists 是 false，其余读不到"),
			[](const TArray<FGameTestValue>& A, FGameTestFlowContext& Context)
			{
				const FString Path = A[1].IsString() ? A[1].GetString() : A[1].ToDisplay(200);
				UObject* Direct = nullptr;
				const TSharedPtr<FJsonValue> Ref = GameTestExprPropImpl::RefOf(A[0], Direct);
				if (Direct == nullptr && !Ref.IsValid())
				{
					return FGameTestValue::Unresolved(TEXT("prop 的第一个参数要是引用（objects 里的名字、$变量、内联引用的 JSON 文字）"));
				}
				UObject* Object = Direct;
				UObject* Item = Direct != nullptr ? GameTestPropRead::ListItemOf(Direct) : nullptr;
				FString Missing;
				if (Object == nullptr)
				{
					const FGameTestRefResult Resolved = GameTestRefResolve::Resolve(Ref, Context);
					Object = Resolved.Get();
					Item = Resolved.GetItem();
					Missing = Resolved.Error;
				}
				const FGameTestPropRead Read = GameTestPropRead::ReadText(Object, Path, Context, Item);
				if (!Read.IsResolved() && Object == nullptr && !Missing.IsEmpty())
				{
					return FGameTestValue::Unresolved(Missing, TEXT("ref_not_found"));
				}
				return Read.Value;
			});
	}
}
