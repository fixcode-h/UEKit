// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Script/GameTestRefs.h"

#include "Components/Widget.h"
#include "Core/Expr/GameTestExpr.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"
#include "Observe/Sources/GameTestValueSource.h"

namespace
{
	enum class EStrKind : uint8
	{
		Literal,
		/** `\=…`：字面的 `=…`。 */
		Escaped,
		/** `=表达式` */
		Expr,
		/** 整个值就是 `$名字.路径` */
		WholeRef,
		/** 文字里嵌着 `$名字` / `${表达式}` */
		Interp,
	};

	bool IsAsciiIdent(TCHAR C)
	{
		return (C < 0x80 && FChar::IsAlnum(C)) || C == TEXT('_');
	}

	/**
	 * 读一个名字。嵌在文字里时「$item.Name的数量」不能把「的数量」读进名字：
	 * 名字以 ASCII 开头就只读 ASCII，以中文开头才连着读中文（变量起中文名的情况）。
	 */
	int32 ReadName(const FString& InText, int32 InAt)
	{
		if (InAt >= InText.Len())
		{
			return InAt;
		}
		const bool bAscii = InText[InAt] < 0x80;
		int32 At = InAt;
		while (At < InText.Len() && (bAscii ? IsAsciiIdent(InText[At]) : (InText[At] >= 0x80 || IsAsciiIdent(InText[At]))))
		{
			++At;
		}
		return At;
	}

	/** 从 `$` 开始读一个引用（`$a.b[0].c`、`${表达式}`），返回结尾位置（不含）；不是引用返回 InAt。 */
	int32 ScanRef(const FString& InText, int32 InAt)
	{
		if (InAt + 1 >= InText.Len() || InText[InAt] != TEXT('$'))
		{
			return InAt;
		}
		if (InText[InAt + 1] == TEXT('{'))
		{
			const int32 Close = InText.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, InAt + 2);
			return Close == INDEX_NONE ? InAt : Close + 1;
		}
		int32 At = ReadName(InText, InAt + 1);
		if (At == InAt + 1 || FChar::IsDigit(InText[InAt + 1]))
		{
			return InAt;
		}
		while (At < InText.Len())
		{
			if (InText[At] == TEXT('.') && At + 1 < InText.Len() && !FChar::IsDigit(InText[At + 1]) && ReadName(InText, At + 1) > At + 1)
			{
				At = ReadName(InText, At + 1);
			}
			else if (InText[At] == TEXT('['))
			{
				const int32 Close = InText.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1);
				if (Close == INDEX_NONE)
				{
					break;
				}
				At = Close + 1;
			}
			else
			{
				break;
			}
		}
		return At;
	}

	/** 引用的表达式文字：`${a + 1}` → `a + 1`，`$a.b` → `$a.b`。 */
	FString RefExpr(const FString& InRef)
	{
		return InRef.StartsWith(TEXT("${")) ? InRef.Mid(2, InRef.Len() - 3) : InRef;
	}

	EStrKind Classify(const FString& InText)
	{
		if (InText.StartsWith(TEXT("\\=")))
		{
			return EStrKind::Escaped;
		}
		if (InText.StartsWith(TEXT("=")))
		{
			return EStrKind::Expr;
		}
		if (InText.StartsWith(TEXT("$")) && ScanRef(InText, 0) == InText.Len())
		{
			return EStrKind::WholeRef;
		}
		for (int32 At = 0; At < InText.Len(); ++At)
		{
			if (InText[At] == TEXT('\\') && At + 1 < InText.Len() && InText[At + 1] == TEXT('$'))
			{
				++At;
				continue;
			}
			if (InText[At] == TEXT('$') && ScanRef(InText, At) > At)
			{
				return EStrKind::Interp;
			}
		}
		return EStrKind::Literal;
	}

	/** 把文字里的引用逐个换掉。`InReplace` 返回替换文字，返回 false 表示取不到（OutError 已写）。 */
	bool Interpolate(const FString& InText, TFunctionRef<bool(const FString& /*Ref*/, FString& /*Out*/)> InReplace, FString& OutText)
	{
		OutText.Reset();
		for (int32 At = 0; At < InText.Len();)
		{
			if (InText[At] == TEXT('\\') && At + 1 < InText.Len() && InText[At + 1] == TEXT('$'))
			{
				OutText.AppendChar(TEXT('$'));
				At += 2;
				continue;
			}
			const int32 End = InText[At] == TEXT('$') ? ScanRef(InText, At) : At;
			if (End > At)
			{
				FString Piece;
				if (!InReplace(InText.Mid(At, End - At), Piece))
				{
					return false;
				}
				OutText += Piece;
				At = End;
				continue;
			}
			OutText.AppendChar(InText[At++]);
		}
		return true;
	}

	FGameTestValue EvalString(const FString& InText, FGameTestFlowContext& InContext)
	{
		switch (Classify(InText))
		{
		case EStrKind::Escaped:
			return FGameTestValue::FromString(InText.RightChop(1));
		case EStrKind::Expr:
			return GameTestExprCache::Eval(InText.RightChop(1), InContext);
		case EStrKind::WholeRef:
			return GameTestExprCache::Eval(RefExpr(InText), InContext);
		case EStrKind::Interp:
		{
			FGameTestValue Failed;
			FString Out;
			const bool bOk = Interpolate(InText, [&InContext, &Failed](const FString& InRef, FString& OutPiece)
			{
				const FGameTestValue Value = GameTestExprCache::Eval(RefExpr(InRef), InContext);
				if (Value.IsUnresolved())
				{
					Failed = Value;
					return false;
				}
				OutPiece = Value.ToDisplay(100000);
				return true;
			}, Out);
			return bOk ? FGameTestValue::FromString(Out) : Failed;
		}
		default:
			return FGameTestValue::FromString(InText);
		}
	}

	/** `{"$obj": "名字"}`：只有这一个键才算引用，`{"$obj": …, "x": 1}` 是普通对象。 */
	bool TryGetObjRef(const TSharedPtr<FJsonObject>& InObject, FString& OutName)
	{
		return InObject.IsValid() && InObject->Values.Num() == 1 && InObject->TryGetStringField(TEXT("$obj"), OutName);
	}

	void ValidateText(const FString& InText, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink)
	{
		auto Check = [&InScope, &InSink](const FString& InExprText)
		{
			FString Error;
			const TSharedPtr<const FGameTestExpr> Expr = GameTestExprCache::Get(InExprText, Error);
			if (!Expr.IsValid())
			{
				InSink.Error(TEXT("expr_syntax"), FString::Printf(TEXT("「%s」：%s"), *InExprText.Left(60), *Error));
				return;
			}
			Expr->Validate(InScope, InSink);
		};
		switch (Classify(InText))
		{
		case EStrKind::Expr:
			Check(InText.RightChop(1));
			break;
		case EStrKind::WholeRef:
			Check(RefExpr(InText));
			break;
		case EStrKind::Interp:
		{
			FString Ignored;
			Interpolate(InText, [&Check](const FString& InRef, FString& OutPiece)
			{
				Check(RefExpr(InRef));
				return true;
			}, Ignored);
			break;
		}
		default:
			break;
		}
	}

	void CollectText(const FString& InText, TSet<FString>& OutVars)
	{
		auto Collect = [&OutVars](const FString& InExprText)
		{
			FString Error;
			if (const TSharedPtr<const FGameTestExpr> Expr = GameTestExprCache::Get(InExprText, Error))
			{
				Expr->CollectVars(OutVars);
			}
		};
		switch (Classify(InText))
		{
		case EStrKind::Expr:
			Collect(InText.RightChop(1));
			break;
		case EStrKind::WholeRef:
			Collect(RefExpr(InText));
			break;
		case EStrKind::Interp:
		{
			FString Ignored;
			Interpolate(InText, [&Collect](const FString& InRef, FString& OutPiece)
			{
				Collect(RefExpr(InRef));
				return true;
			}, Ignored);
			break;
		}
		default:
			break;
		}
	}
}

namespace GameTestRefs
{
	FGameTestValue EvalJson(const TSharedPtr<FJsonValue>& InJson, FGameTestFlowContext& InContext)
	{
		if (!InJson.IsValid())
		{
			return FGameTestValue::Null();
		}
		switch (InJson->Type)
		{
		case EJson::String:
			return EvalString(InJson->AsString(), InContext);
		case EJson::Array:
		{
			TArray<FGameTestValue> Items;
			for (const TSharedPtr<FJsonValue>& Item : InJson->AsArray())
			{
				Items.Add(EvalJson(Item, InContext));
				if (Items.Last().IsUnresolved())
				{
					return Items.Last();
				}
			}
			return FGameTestValue::FromArray(MoveTemp(Items));
		}
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Object = InJson->AsObject();
			FString ObjName;
			if (TryGetObjRef(Object, ObjName))
			{
				const FGameTestPathSegment Segment = FGameTestPathSegment::Member(ObjName);
				return GameTestPath::ResolveSource(TEXT("obj"), FString(), MakeArrayView(&Segment, 1), InContext);
			}
			TMap<FString, FGameTestValue> Fields;
			if (Object.IsValid())
			{
				for (const auto& Pair : Object->Values)
				{
					FGameTestValue Value = EvalJson(Pair.Value, InContext);
					if (Value.IsUnresolved())
					{
						return Value;
					}
					Fields.Add(GAMETEST_JSON_KEY(Pair.Key), MoveTemp(Value));
				}
			}
			return FGameTestValue::FromMap(MoveTemp(Fields));
		}
		default:
			return FGameTestValue::FromJson(InJson);
		}
	}

	bool EvalText(const FString& InText, FGameTestFlowContext& InContext, FString& OutText, FString& OutError)
	{
		const FGameTestValue Value = EvalString(InText, InContext);
		if (Value.IsUnresolved())
		{
			OutError = Value.GetString();
			return false;
		}
		OutText = Value.IsString() ? Value.GetString() : Value.ToDisplay(100000);
		return true;
	}

	bool EvalSelector(const FString& InSelector, FGameTestFlowContext& InContext, FString& OutSelector, FString& OutError)
	{
		if (Classify(InSelector) == EStrKind::Literal)
		{
			OutSelector = InSelector.Replace(TEXT("\\$"), TEXT("$"));
			return true;
		}
		if (Classify(InSelector) == EStrKind::Expr)
		{
			return EvalText(InSelector, InContext, OutSelector, OutError);
		}
		return Interpolate(InSelector, [&InContext, &OutError](const FString& InRef, FString& OutPiece)
		{
			// `$item` 这类：循环把条目控件另存在 `__w_<名字>`（`$item` 本身是条目的数据对象），选择器里用控件。
			FGameTestValue Value;
			const bool bWidgetCompanion = !InRef.StartsWith(TEXT("${")) && InContext.FindVar(TEXT("__w_") + InRef.Mid(1), Value)
				&& Value.GetObject() != nullptr;
			if (!bWidgetCompanion)
			{
				Value = GameTestExprCache::Eval(RefExpr(InRef), InContext);
			}
			if (Value.IsUnresolved())
			{
				OutError = Value.GetString();
				return false;
			}
			if (UWidget* Widget = Cast<UWidget>(Value.GetObject()))
			{
				if (!InContext.Handles)
				{
					OutError = TEXT("句柄表不可用，选择器里不能引用控件变量");
					return false;
				}
				// 句柄名按对象编号起：同一个控件反复引用只占一个句柄，不会越攒越多。
				const FString Handle = FString::Printf(TEXT("__v_%u"), Widget->GetUniqueID());
				InContext.Handles->Add(Handle, Widget);
				OutPiece = TEXT("@") + Handle;
				return true;
			}
			OutPiece = Value.ToDisplay(100000);
			return true;
		}, OutSelector);
	}

	bool HasRefs(const TSharedPtr<FJsonValue>& InJson)
	{
		if (!InJson.IsValid())
		{
			return false;
		}
		switch (InJson->Type)
		{
		case EJson::String:
			return Classify(InJson->AsString()) != EStrKind::Literal;
		case EJson::Array:
			for (const TSharedPtr<FJsonValue>& Item : InJson->AsArray())
			{
				if (HasRefs(Item))
				{
					return true;
				}
			}
			return false;
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Object = InJson->AsObject();
			FString Ignored;
			if (TryGetObjRef(Object, Ignored))
			{
				return true;
			}
			if (Object.IsValid())
			{
				for (const auto& Pair : Object->Values)
				{
					if (HasRefs(Pair.Value))
					{
						return true;
					}
				}
			}
			return false;
		}
		default:
			return false;
		}
	}

	void Validate(const TSharedPtr<FJsonValue>& InJson, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink)
	{
		if (!InJson.IsValid())
		{
			return;
		}
		if (InJson->Type == EJson::String)
		{
			ValidateText(InJson->AsString(), InScope, InSink);
		}
		else if (InJson->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : InJson->AsArray())
			{
				Validate(Item, InScope, InSink);
			}
		}
		else if (InJson->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Object = InJson->AsObject();
			if (Object.IsValid())
			{
				for (const auto& Pair : Object->Values)
				{
					Validate(Pair.Value, InScope, InSink);
				}
			}
		}
	}

	void CollectVars(const TSharedPtr<FJsonValue>& InJson, TSet<FString>& OutVars)
	{
		if (!InJson.IsValid())
		{
			return;
		}
		if (InJson->Type == EJson::String)
		{
			CollectText(InJson->AsString(), OutVars);
		}
		else if (InJson->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : InJson->AsArray())
			{
				CollectVars(Item, OutVars);
			}
		}
		else if (InJson->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Object = InJson->AsObject();
			if (Object.IsValid())
			{
				for (const auto& Pair : Object->Values)
				{
					CollectVars(Pair.Value, OutVars);
				}
			}
		}
	}
}
