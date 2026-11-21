// Copyright Epic Games, Inc. All Rights Reserved.

#include "Api/GameTestApi.h"
#include "Core/Expr/GameTestExpr.h"
#include "Core/Expr/GameTestExprAst.h"
#include "Core/Expr/GameTestExprFunction.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Sources/GameTestValueSource.h"

/**
 * 表达式求值和静态检查。规则见协议第 3 节：
 * - 取不到（Unresolved）一路往外传，`or` 有一边成立就成立、`and` 有一边不成立就不成立（短路），
 *   这样「对象不在 or 血量 < 10」这种写法在对象不在时也能得出结论。
 * - `+` 两边有字符串就拼接；数字和数字字符串比较按数字。
 */
namespace
{
	using FNode = FGameTestExprNode;

	FGameTestValue Eval(const FNode& InNode, FGameTestFlowContext& InContext);

	FGameTestValue TypeError(const FString& InWhat)
	{
		return FGameTestValue::Unresolved(InWhat, TEXT("expr_type"));
	}

	FGameTestValue EvalPath(const FNode& InNode, FGameTestFlowContext& InContext)
	{
		// 先把后缀里的下标和参数求出来，路径走法只看值。
		TArray<FGameTestPathSegment> Segments;
		for (const FGameTestExprPostfix& Step : InNode.Postfix)
		{
			TArray<FGameTestValue> Args;
			for (const TSharedRef<FNode>& Arg : Step.Args)
			{
				Args.Add(Eval(*Arg, InContext));
				if (Args.Last().IsUnresolved())
				{
					return Args.Last();
				}
			}
			switch (Step.Kind)
			{
			case FGameTestExprPostfix::EKind::Index:
				Segments.Add(FGameTestPathSegment::At(Args[0]));
				break;
			case FGameTestExprPostfix::EKind::Call:
				Segments.Add(FGameTestPathSegment::Call(Step.Name, MoveTemp(Args)));
				break;
			default:
				Segments.Add(FGameTestPathSegment::Member(Step.Name));
				break;
			}
		}
		switch (InNode.Root)
		{
		case EGameTestPathRoot::Source:
			return GameTestPath::ResolveSource(FName(*InNode.RootName), InNode.Selector, Segments, InContext);
		case EGameTestPathRoot::Var:
		{
			FGameTestValue Base;
			if (!InContext.FindVar(InNode.RootName, Base))
			{
				return FGameTestValue::Unresolved(FString::Printf(TEXT("没有变量 $%s"), *InNode.RootName), TEXT("var_undefined"));
			}
			return GameTestPath::Walk(Base, Segments, InContext, TEXT("$") + InNode.RootName);
		}
		default:
			return InNode.Children.Num() > 0 ? GameTestPath::Walk(Eval(*InNode.Children[0], InContext), Segments, InContext, TEXT("(…)"))
											 : FGameTestValue::Null();
		}
	}

	FGameTestValue EvalCall(const FNode& InNode, FGameTestFlowContext& InContext)
	{
		const FGameTestExprFunction* Function = GameTestExprFunctions::Get().Find(FName(*InNode.Function));
		TArray<FGameTestValue> Args;
		for (const TSharedRef<FNode>& Arg : InNode.Children)
		{
			Args.Add(Eval(*Arg, InContext));
			if (Args.Last().IsUnresolved() && !(Function && Function->bAcceptsUnresolved))
			{
				return Args.Last();
			}
		}
		if (Function)
		{
			if (Args.Num() < Function->MinArgs || (Function->MaxArgs >= 0 && Args.Num() > Function->MaxArgs))
			{
				return TypeError(FString::Printf(TEXT("%s 的参数个数不对：%s"), *InNode.Function, *Function->Signature));
			}
			return Function->Call ? Function->Call(Args, InContext) : FGameTestValue::Null();
		}
		// 不是内置函数就当接口：GameTestApis::CallNow 负责只读、不能等待这些检查。
		return GameTestApis::CallNow(InNode.Function, Args, InContext);
	}

	FGameTestValue Arith(const FString& InOp, const FGameTestValue& L, const FGameTestValue& R)
	{
		if (InOp == TEXT("+"))
		{
			if (L.IsString() || R.IsString())
			{
				return FGameTestValue::FromString(L.ToDisplay(100000) + R.ToDisplay(100000));
			}
			if (L.Type == EGameTestValueType::Vector && R.Type == EGameTestValueType::Vector)
			{
				return FGameTestValue::FromVector(L.GetVector() + R.GetVector());
			}
			if (L.Type == EGameTestValueType::Array && R.Type == EGameTestValueType::Array)
			{
				TArray<FGameTestValue> Items = L.GetArray();
				Items.Append(R.GetArray());
				return FGameTestValue::FromArray(MoveTemp(Items));
			}
		}
		if (InOp == TEXT("-") && L.Type == EGameTestValueType::Vector && R.Type == EGameTestValueType::Vector)
		{
			return FGameTestValue::FromVector(L.GetVector() - R.GetVector());
		}
		double A = 0.0;
		double B = 0.0;
		if (InOp == TEXT("*") && L.Type == EGameTestValueType::Vector && R.TryNumber(B))
		{
			return FGameTestValue::FromVector(L.GetVector() * B);
		}
		if (!L.TryNumber(A) || !R.TryNumber(B))
		{
			return TypeError(FString::Printf(TEXT("%s 两边要是数字，实际是 %s 和 %s"), *InOp, *L.TypeName(), *R.TypeName()));
		}
		if (InOp == TEXT("+")) return FGameTestValue::FromNumber(A + B);
		if (InOp == TEXT("-")) return FGameTestValue::FromNumber(A - B);
		if (InOp == TEXT("*")) return FGameTestValue::FromNumber(A * B);
		if (B == 0.0)
		{
			return TypeError(TEXT("除以 0"));
		}
		return FGameTestValue::FromNumber(InOp == TEXT("/") ? A / B : FMath::Fmod(A, B));
	}

	bool ContainsValue(const FGameTestValue& InHay, const FGameTestValue& InNeedle)
	{
		if (InHay.Type == EGameTestValueType::Array)
		{
			return InHay.GetArray().ContainsByPredicate([&InNeedle](const FGameTestValue& Item) { return Item.LooseEquals(InNeedle); });
		}
		if (InHay.Type == EGameTestValueType::Map)
		{
			FGameTestValue Ignored;
			return InHay.TryGetField(InNeedle.ToDisplay(1000), Ignored);
		}
		return InHay.ToDisplay(100000).Contains(InNeedle.ToDisplay(100000), ESearchCase::IgnoreCase);
	}

	FGameTestValue EvalLogic(const FNode& InNode, FGameTestFlowContext& InContext)
	{
		const bool bOr = InNode.Op == TEXT("or");
		const FGameTestValue L = Eval(*InNode.Children[0], InContext);
		if (!L.IsUnresolved() && L.Truthy() == bOr)
		{
			return FGameTestValue::FromBool(bOr);
		}
		const FGameTestValue R = Eval(*InNode.Children[1], InContext);
		if (!R.IsUnresolved() && R.Truthy() == bOr)
		{
			return FGameTestValue::FromBool(bOr);
		}
		if (L.IsUnresolved())
		{
			return L;
		}
		if (R.IsUnresolved())
		{
			return R;
		}
		return FGameTestValue::FromBool(!bOr);
	}

	FGameTestValue EvalBinary(const FNode& InNode, FGameTestFlowContext& InContext)
	{
		const FString& Op = InNode.Op;
		if (Op == TEXT("and") || Op == TEXT("or"))
		{
			return EvalLogic(InNode, InContext);
		}
		const FGameTestValue L = Eval(*InNode.Children[0], InContext);
		if (L.IsUnresolved())
		{
			return L;
		}
		const FGameTestValue R = Eval(*InNode.Children[1], InContext);
		if (R.IsUnresolved())
		{
			return R;
		}
		if (Op == TEXT("==")) return FGameTestValue::FromBool(L.LooseEquals(R));
		if (Op == TEXT("!=")) return FGameTestValue::FromBool(!L.LooseEquals(R));
		if (Op == TEXT("in")) return FGameTestValue::FromBool(ContainsValue(R, L));
		if (Op == TEXT("not in")) return FGameTestValue::FromBool(!ContainsValue(R, L));
		if (Op == TEXT(">") || Op == TEXT(">=") || Op == TEXT("<") || Op == TEXT("<="))
		{
			int32 Order = 0;
			if (!L.Compare(R, Order))
			{
				return TypeError(FString::Printf(TEXT("%s 和 %s 比不了大小"), *L.ToDisplay(40), *R.ToDisplay(40)));
			}
			const bool bHolds = Op == TEXT(">") ? Order > 0 : Op == TEXT(">=") ? Order >= 0 : Op == TEXT("<") ? Order < 0 : Order <= 0;
			return FGameTestValue::FromBool(bHolds);
		}
		return Arith(Op, L, R);
	}

	FGameTestValue Eval(const FNode& InNode, FGameTestFlowContext& InContext)
	{
		switch (InNode.Kind)
		{
		case EGameTestExprKind::Literal:
			return InNode.Value;
		case EGameTestExprKind::Path:
			return EvalPath(InNode, InContext);
		case EGameTestExprKind::Call:
			return EvalCall(InNode, InContext);
		case EGameTestExprKind::Unary:
		{
			const FGameTestValue Inner = Eval(*InNode.Children[0], InContext);
			if (Inner.IsUnresolved())
			{
				return Inner;
			}
			if (InNode.Op == TEXT("not"))
			{
				return FGameTestValue::FromBool(!Inner.Truthy());
			}
			if (Inner.Type == EGameTestValueType::Vector)
			{
				return FGameTestValue::FromVector(-Inner.GetVector());
			}
			double Number = 0.0;
			return Inner.TryNumber(Number) ? FGameTestValue::FromNumber(-Number) : TypeError(TEXT("负号后面要是数字"));
		}
		case EGameTestExprKind::Binary:
			return EvalBinary(InNode, InContext);
		case EGameTestExprKind::Ternary:
		{
			const FGameTestValue Cond = Eval(*InNode.Children[0], InContext);
			if (Cond.IsUnresolved())
			{
				return Cond;
			}
			return Eval(*InNode.Children[Cond.Truthy() ? 1 : 2], InContext);
		}
		case EGameTestExprKind::ArrayLit:
		{
			TArray<FGameTestValue> Items;
			for (const TSharedRef<FNode>& Child : InNode.Children)
			{
				Items.Add(Eval(*Child, InContext));
				if (Items.Last().IsUnresolved())
				{
					return Items.Last();
				}
			}
			return FGameTestValue::FromArray(MoveTemp(Items));
		}
		case EGameTestExprKind::MapLit:
		{
			TMap<FString, FGameTestValue> Fields;
			for (int32 Index = 0; Index < InNode.Children.Num(); ++Index)
			{
				const FGameTestValue Value = Eval(*InNode.Children[Index], InContext);
				if (Value.IsUnresolved())
				{
					return Value;
				}
				Fields.Add(InNode.Keys[Index], Value);
			}
			return FGameTestValue::FromMap(MoveTemp(Fields));
		}
		}
		return FGameTestValue::Null();
	}

	//~ 静态检查 ------------------------------------------------------------

	void ValidateNode(const FNode& InNode, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink);

	/** 2.0.0 删掉的按选择器读控件的函数 → prop() 里对应的属性路径（报 expr_removed 时给出改法）。 */
	const TMap<FString, FString>& ExprRemovedFunctions()
	{
		static const TMap<FString, FString> Map = {
			{ TEXT("text"), TEXT("@/部件:text") }, { TEXT("number"), TEXT("@/部件:text") /* 再套 num() 或改用数字属性 */ },
			{ TEXT("visible"), TEXT("@/部件:onScreen") }, { TEXT("enabled"), TEXT("@/部件:enabled") }, { TEXT("exists"), TEXT("@:exists") },
			{ TEXT("checked"), TEXT("@/部件:checked") }, { TEXT("count"), TEXT("@/部件:count") }, { TEXT("onScreen"), TEXT("@:onScreen") },
		};
		return Map;
	}

	void ValidateCall(const FNode& InNode, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink)
	{
		const FString At = FString::Printf(TEXT("第 %d 个字附近："), InNode.Pos + 1);
		if (const FGameTestExprFunction* Function = GameTestExprFunctions::Get().Find(FName(*InNode.Function)))
		{
			const int32 Num = InNode.Children.Num();
			if (Num < Function->MinArgs || (Function->MaxArgs >= 0 && Num > Function->MaxArgs))
			{
				InSink.Error(TEXT("expr_type"), At + FString::Printf(TEXT("%s 的参数个数不对，写法 %s"), *InNode.Function, *Function->Signature));
			}
			return;
		}
		const IGameTestApi* Api = GameTestApis::Get().Find(FName(*InNode.Function));
		if (!Api && ExprRemovedFunctions().Contains(InNode.Function))
		{
			InSink.Error(TEXT("expr_removed"), At + FString::Printf(TEXT("%s(sel) 按选择器读控件，2.0.0 起删掉了：改成 prop(引用, \"%s\")，引用写 objects 里的名字或 $变量（见 docs/game-test-ref.md 第 4 节）"),
				*InNode.Function, *ExprRemovedFunctions()[InNode.Function]));
			return;
		}
		if (!Api)
		{
			if (const FString* NewName = GameTestApis::GetRenamed().Find(InNode.Function))
			{
				InSink.Error(TEXT("api_renamed"), At + FString::Printf(TEXT("接口 %s 已改名，改用 %s"), *InNode.Function, **NewName));
				return;
			}
			TArray<FString> Suggestions;
			GameTestExprFunctions::Get().Suggest(InNode.Function, Suggestions);
			GameTestApis::Get().Suggest(InNode.Function, Suggestions);
			InSink.Error(TEXT("api_missing"), At + FString::Printf(TEXT("没有函数或接口 %s%s"), *InNode.Function,
				Suggestions.Num() > 0 ? *FString::Printf(TEXT("（是不是 %s）"), *FString::Join(Suggestions, TEXT("、"))) : TEXT("")));
			return;
		}
		const FGameTestApiDesc& Desc = Api->GetDesc();
		if ((InScope.bReadOnly && !Desc.bReadOnly) || Desc.bMayWait)
		{
			InSink.Error(TEXT("expr_not_readonly"), At + FString::Printf(TEXT("%s 会改游戏或要等待，不能写在表达式里，改成 api 步骤"), *Desc.Name));
		}
		if (Desc.Src == TEXT("lua") && InScope.bReadOnly && !InScope.bAllowLuaInCond)
		{
			InSink.Error(TEXT("lua_disabled"), At + FString::Printf(TEXT("%s 是 Lua 接口，条件里调 Lua 要在运行设置里打开 allowLuaInCond"), *Desc.Name));
		}
		const int32 Required = Desc.Args.FilterByPredicate([](const FGameTestApiArg& Arg) { return !Arg.bOptional; }).Num();
		if (InNode.Children.Num() < Required || InNode.Children.Num() > Desc.Args.Num())
		{
			InSink.Error(TEXT("arg_missing"), At + FString::Printf(TEXT("参数个数不对，写法 %s"), *Desc.Signature()));
		}
	}

	void ValidatePath(const FNode& InNode, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink)
	{
		const FString At = FString::Printf(TEXT("第 %d 个字附近："), InNode.Pos + 1);
		if (InNode.Root == EGameTestPathRoot::Var && InScope.KnownVars && !InScope.KnownVars->Contains(InNode.RootName))
		{
			InSink.Error(TEXT("var_undefined"), At + FString::Printf(TEXT("没有定义 $%s：在 vars / params 里声明，或先 set"), *InNode.RootName));
		}
		if (InNode.Root == EGameTestPathRoot::Source)
		{
			const IGameTestValueSource* Source = GameTestValueSources::Get().Find(FName(*InNode.RootName));
			if (!Source)
			{
				TArray<FString> Suggestions;
				GameTestValueSources::Get().Suggest(InNode.RootName, Suggestions);
				InSink.Error(TEXT("path_missing"), At + FString::Printf(TEXT("不认识的来源 @%s%s"), *InNode.RootName,
					Suggestions.Num() > 0 ? *FString::Printf(TEXT("（是不是 @%s）"), *FString::Join(Suggestions, TEXT("、@"))) : TEXT("")));
			}
			else
			{
				const bool bCalls = InNode.Postfix.ContainsByPredicate([](const FGameTestExprPostfix& Step) { return Step.Kind == FGameTestExprPostfix::EKind::Call; });
				if (Source->IsLua() && bCalls && InScope.bReadOnly && !InScope.bAllowLuaInCond)
				{
					InSink.Error(TEXT("lua_disabled"), At + TEXT("条件、表达式里调 Lua 函数要在运行设置里打开 allowLuaInCond"));
				}
				if (Source->UsesSelector() && InNode.Selector.IsEmpty())
				{
					InSink.Error(TEXT("expr_syntax"), At + FString::Printf(TEXT("@%s 要带 [选择器]，写法 %s"), *InNode.RootName, *Source->GetSyntax()));
				}
				// 下标、参数此刻还没值，交给来源查名字。
				TArray<FGameTestPathSegment> Segments;
				for (const FGameTestExprPostfix& Step : InNode.Postfix)
				{
					Segments.Add(Step.Kind == FGameTestExprPostfix::EKind::Index ? FGameTestPathSegment::At(FGameTestValue::Null())
						: Step.Kind == FGameTestExprPostfix::EKind::Call ? FGameTestPathSegment::Call(Step.Name, TArray<FGameTestValue>())
						: FGameTestPathSegment::Member(Step.Name));
				}
				Source->Validate(InNode.Selector, Segments, InSink);
			}
		}
		for (const FGameTestExprPostfix& Step : InNode.Postfix)
		{
			for (const TSharedRef<FNode>& Arg : Step.Args)
			{
				ValidateNode(*Arg, InScope, InSink);
			}
		}
	}

	void ValidateNode(const FNode& InNode, const FGameTestExprScope& InScope, FGameTestDiagSink& InSink)
	{
		if (InNode.Kind == EGameTestExprKind::Call)
		{
			ValidateCall(InNode, InScope, InSink);
		}
		else if (InNode.Kind == EGameTestExprKind::Path)
		{
			ValidatePath(InNode, InScope, InSink);
		}
		for (const TSharedRef<FNode>& Child : InNode.Children)
		{
			ValidateNode(*Child, InScope, InSink);
		}
	}

	void CollectVarsIn(const FNode& InNode, TSet<FString>& OutVars)
	{
		if (InNode.Kind == EGameTestExprKind::Path && InNode.Root == EGameTestPathRoot::Var)
		{
			OutVars.Add(InNode.RootName);
		}
		for (const FGameTestExprPostfix& Step : InNode.Postfix)
		{
			for (const TSharedRef<FNode>& Arg : Step.Args)
			{
				CollectVarsIn(*Arg, OutVars);
			}
		}
		for (const TSharedRef<FNode>& Child : InNode.Children)
		{
			CollectVarsIn(*Child, OutVars);
		}
	}

	struct FCacheEntry
	{
		TSharedPtr<const FGameTestExpr> Expr;
		FString Error;
	};

	TMap<FString, FCacheEntry>& Cache()
	{
		static TMap<FString, FCacheEntry> Entries;
		return Entries;
	}
}

FGameTestValue FGameTestExpr::Evaluate(FGameTestFlowContext& InContext) const
{
	FGameTestFlowContext::FReadOnlyScope ReadOnly(InContext);
	return Eval(*Root, InContext);
}

void FGameTestExpr::Validate(const FGameTestExprScope& InScope, FGameTestDiagSink& InSink) const
{
	ValidateNode(*Root, InScope, InSink);
}

void FGameTestExpr::CollectVars(TSet<FString>& OutVars) const
{
	CollectVarsIn(*Root, OutVars);
}

TSharedPtr<const FGameTestExpr> GameTestExprCache::Get(const FString& InText, FString& OutError)
{
	if (const FCacheEntry* Found = Cache().Find(InText))
	{
		OutError = Found->Error;
		return Found->Expr;
	}
	// 场景里的表达式数量有限，缓存只为不每帧重复解析；万一被灌进大量不同的文字（Agent 反复 resolve），清掉重来。
	if (Cache().Num() > 4096)
	{
		Cache().Reset();
	}
	FCacheEntry& Entry = Cache().Add(InText);
	Entry.Expr = FGameTestExpr::Parse(InText, Entry.Error);
	OutError = Entry.Error;
	return Entry.Expr;
}

FGameTestValue GameTestExprCache::Eval(const FString& InText, FGameTestFlowContext& InContext)
{
	FString Error;
	const TSharedPtr<const FGameTestExpr> Expr = Get(InText, Error);
	if (!Expr.IsValid())
	{
		return FGameTestValue::Unresolved(FString::Printf(TEXT("表达式写错了：%s"), *Error), TEXT("expr_syntax"));
	}
	return Expr->Evaluate(InContext);
}

void GameTestExprCache::Reset()
{
	Cache().Reset();
}
