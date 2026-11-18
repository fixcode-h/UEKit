// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Expr/GameTestExprFunction.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

/**
 * 表达式内置函数：类型转换、数值、文字、向量、时间（协议第 3 节）。控件类的在 GameTestExprWidget.cpp。
 */
namespace
{
	using FArgs = TArray<FGameTestValue>;

	FGameTestValue BasicTypeError(const FString& InWhat)
	{
		return FGameTestValue::Unresolved(InWhat, TEXT("expr_type"));
	}

	bool Num(const FGameTestValue& InValue, double& OutNumber)
	{
		return InValue.TryNumber(OutNumber);
	}

	FString ArgText(const FGameTestValue& InValue)
	{
		return InValue.IsString() ? InValue.GetString() : InValue.ToDisplay(100000);
	}

	/** 向量参数：向量本身，或 Actor（取位置）。 */
	bool Vec(const FGameTestValue& InValue, FVector& OutVector)
	{
		if (InValue.Type == EGameTestValueType::Vector)
		{
			OutVector = InValue.GetVector();
			return true;
		}
		if (const AActor* Actor = Cast<AActor>(InValue.GetObject()))
		{
			OutVector = Actor->GetActorLocation();
			return true;
		}
		// {X, Y, Z} 这样的对象也认：JSON 里写的向量读进来就是 Map。
		FGameTestValue X;
		FGameTestValue Y;
		FGameTestValue Z;
		double Xn = 0.0;
		double Yn = 0.0;
		double Zn = 0.0;
		if (InValue.TryGetField(TEXT("X"), X) && InValue.TryGetField(TEXT("Y"), Y) && X.TryNumber(Xn) && Y.TryNumber(Yn))
		{
			if (InValue.TryGetField(TEXT("Z"), Z))
			{
				Z.TryNumber(Zn);
			}
			OutVector = FVector(Xn, Yn, Zn);
			return true;
		}
		return false;
	}

	void RegisterConvert()
	{
		GameTestExprFunctions::Add(TEXT("len"), 1, 1, TEXT("类型"), TEXT("len(x)"), TEXT("数组元素数、对象字段数、文字字数"),
			[](const FArgs& A, FGameTestFlowContext&)
			{
				const int32 Len = A[0].Len();
				return Len >= 0 ? FGameTestValue::FromNumber(Len) : BasicTypeError(FString::Printf(TEXT("len 不能用在 %s 上"), *A[0].TypeName()));
			});
		GameTestExprFunctions::Add(TEXT("str"), 1, 1, TEXT("类型"), TEXT("str(x)"), TEXT("转文字"),
			[](const FArgs& A, FGameTestFlowContext&) { return FGameTestValue::FromString(ArgText(A[0])); });
		GameTestExprFunctions::Add(TEXT("num"), 1, 1, TEXT("类型"), TEXT("num(x)"), TEXT("转数字（整段是数字才行，从文字里取数用 number(sel)）"),
			[](const FArgs& A, FGameTestFlowContext&)
			{
				double N = 0.0;
				return Num(A[0], N) ? FGameTestValue::FromNumber(N) : BasicTypeError(FString::Printf(TEXT("%s 不是数字"), *A[0].ToDisplay(40)));
			});
		GameTestExprFunctions::Add(TEXT("int"), 1, 1, TEXT("类型"), TEXT("int(x)"), TEXT("转整数（向零取整）"),
			[](const FArgs& A, FGameTestFlowContext&)
			{
				double N = 0.0;
				return Num(A[0], N) ? FGameTestValue::FromNumber(FMath::TruncToDouble(N)) : BasicTypeError(FString::Printf(TEXT("%s 不是数字"), *A[0].ToDisplay(40)));
			});
		GameTestExprFunctions::Add(TEXT("bool"), 1, 1, TEXT("类型"), TEXT("bool(x)"), TEXT("真值（非 0、非空、true）"),
			[](const FArgs& A, FGameTestFlowContext&) { return FGameTestValue::FromBool(A[0].Truthy()); });
	}

	void RegisterMath()
	{
		GameTestExprFunctions::Add(TEXT("round"), 1, 2, TEXT("数值"), TEXT("round(x, n?)"), TEXT("四舍五入到 n 位小数（默认整数）"),
			[](const FArgs& A, FGameTestFlowContext&)
			{
				double X = 0.0;
				double Digits = 0.0;
				if (!Num(A[0], X) || (A.Num() > 1 && !Num(A[1], Digits)))
				{
					return BasicTypeError(TEXT("round 的参数要是数字"));
				}
				const double Scale = FMath::Pow(10.0, FMath::Clamp(FMath::RoundToDouble(Digits), 0.0, 10.0));
				return FGameTestValue::FromNumber(FMath::RoundHalfFromZero(X * Scale) / Scale);
			});
		auto Unary = [](const TCHAR* InName, const TCHAR* InSig, const TCHAR* InSummary, double (*InFn)(double))
		{
			GameTestExprFunctions::Add(InName, 1, 1, TEXT("数值"), InSig, InSummary, [InFn, InName](const FArgs& A, FGameTestFlowContext&)
			{
				double X = 0.0;
				return Num(A[0], X) ? FGameTestValue::FromNumber(InFn(X)) : BasicTypeError(FString::Printf(TEXT("%s 的参数要是数字"), InName));
			});
		};
		Unary(TEXT("floor"), TEXT("floor(x)"), TEXT("向下取整"), [](double X) { return FMath::FloorToDouble(X); });
		Unary(TEXT("ceil"), TEXT("ceil(x)"), TEXT("向上取整"), [](double X) { return FMath::CeilToDouble(X); });
		Unary(TEXT("abs"), TEXT("abs(x)"), TEXT("绝对值"), [](double X) { return FMath::Abs(X); });
		auto MinMax = [](const TCHAR* InName, bool bMax)
		{
			GameTestExprFunctions::Add(InName, 1, -1, TEXT("数值"), bMax ? TEXT("max(a, b, …)") : TEXT("min(a, b, …)"),
				bMax ? TEXT("最大值（也可以给一个数组）") : TEXT("最小值（也可以给一个数组）"), [bMax](const FArgs& A, FGameTestFlowContext&)
			{
				const TArray<FGameTestValue>& Items = A.Num() == 1 && A[0].Type == EGameTestValueType::Array ? A[0].GetArray() : A;
				if (Items.Num() == 0)
				{
					return BasicTypeError(TEXT("min / max 至少要一个数"));
				}
				double Best = 0.0;
				for (int32 Index = 0; Index < Items.Num(); ++Index)
				{
					double X = 0.0;
					if (!Num(Items[Index], X))
					{
						return BasicTypeError(FString::Printf(TEXT("%s 不是数字"), *Items[Index].ToDisplay(40)));
					}
					Best = Index == 0 ? X : (bMax ? FMath::Max(Best, X) : FMath::Min(Best, X));
				}
				return FGameTestValue::FromNumber(Best);
			});
		};
		MinMax(TEXT("min"), false);
		MinMax(TEXT("max"), true);
		GameTestExprFunctions::Add(TEXT("clamp"), 3, 3, TEXT("数值"), TEXT("clamp(x, a, b)"), TEXT("把 x 限制在 [a, b]"),
			[](const FArgs& A, FGameTestFlowContext&)
			{
				double X = 0.0;
				double Lo = 0.0;
				double Hi = 0.0;
				if (!Num(A[0], X) || !Num(A[1], Lo) || !Num(A[2], Hi))
				{
					return BasicTypeError(TEXT("clamp 的参数要是数字"));
				}
				return FGameTestValue::FromNumber(FMath::Clamp(X, FMath::Min(Lo, Hi), FMath::Max(Lo, Hi)));
			});
	}

	void RegisterText()
	{
		auto Pair = [](const TCHAR* InName, const TCHAR* InSig, const TCHAR* InSummary, bool (*InFn)(const FString&, const FString&))
		{
			GameTestExprFunctions::Add(InName, 2, 2, TEXT("文字"), InSig, InSummary, [InFn](const FArgs& A, FGameTestFlowContext&)
			{
				return FGameTestValue::FromBool(InFn(ArgText(A[0]), ArgText(A[1])));
			});
		};
		GameTestExprFunctions::Add(TEXT("contains"), 2, 2, TEXT("文字"), TEXT("contains(a, b)"), TEXT("a 里有 b（文字忽略大小写；数组按元素）"),
			[](const FArgs& A, FGameTestFlowContext&)
			{
				if (A[0].Type == EGameTestValueType::Array)
				{
					return FGameTestValue::FromBool(A[0].GetArray().ContainsByPredicate([&A](const FGameTestValue& Item) { return Item.LooseEquals(A[1]); }));
				}
				return FGameTestValue::FromBool(ArgText(A[0]).Contains(ArgText(A[1]), ESearchCase::IgnoreCase));
			});
		Pair(TEXT("startsWith"), TEXT("startsWith(a, b)"), TEXT("a 以 b 开头（忽略大小写）"),
			[](const FString& L, const FString& R) { return L.StartsWith(R, ESearchCase::IgnoreCase); });
		Pair(TEXT("endsWith"), TEXT("endsWith(a, b)"), TEXT("a 以 b 结尾（忽略大小写）"),
			[](const FString& L, const FString& R) { return L.EndsWith(R, ESearchCase::IgnoreCase); });
		GameTestExprFunctions::Add(TEXT("lower"), 1, 1, TEXT("文字"), TEXT("lower(s)"), TEXT("转小写"),
			[](const FArgs& A, FGameTestFlowContext&) { return FGameTestValue::FromString(ArgText(A[0]).ToLower()); });
		GameTestExprFunctions::Add(TEXT("upper"), 1, 1, TEXT("文字"), TEXT("upper(s)"), TEXT("转大写"),
			[](const FArgs& A, FGameTestFlowContext&) { return FGameTestValue::FromString(ArgText(A[0]).ToUpper()); });
		GameTestExprFunctions::Add(TEXT("trim"), 1, 1, TEXT("文字"), TEXT("trim(s)"), TEXT("去掉首尾空白"),
			[](const FArgs& A, FGameTestFlowContext&) { return FGameTestValue::FromString(ArgText(A[0]).TrimStartAndEnd()); });
		GameTestExprFunctions::Add(TEXT("replace"), 3, 3, TEXT("文字"), TEXT("replace(s, a, b)"), TEXT("把 s 里的 a 都换成 b"),
			[](const FArgs& A, FGameTestFlowContext&) { return FGameTestValue::FromString(ArgText(A[0]).Replace(*ArgText(A[1]), *ArgText(A[2]))); });
		GameTestExprFunctions::Add(TEXT("split"), 2, 2, TEXT("文字"), TEXT("split(s, sep)"), TEXT("按 sep 拆成数组"),
			[](const FArgs& A, FGameTestFlowContext&)
			{
				TArray<FString> Parts;
				ArgText(A[0]).ParseIntoArray(Parts, *ArgText(A[1]), false);
				TArray<FGameTestValue> Items;
				for (const FString& Part : Parts)
				{
					Items.Add(FGameTestValue::FromString(Part));
				}
				return FGameTestValue::FromArray(MoveTemp(Items));
			});
		GameTestExprFunctions::Add(TEXT("join"), 2, 2, TEXT("文字"), TEXT("join(arr, sep)"), TEXT("把数组用 sep 连起来"),
			[](const FArgs& A, FGameTestFlowContext&)
			{
				TArray<FString> Parts;
				for (const FGameTestValue& Item : A[0].GetArray())
				{
					Parts.Add(ArgText(Item));
				}
				return FGameTestValue::FromString(FString::Join(Parts, *ArgText(A[1])));
			});
		GameTestExprFunctions::Add(TEXT("format"), 1, -1, TEXT("文字"), TEXT("format(fmt, …)"), TEXT("按 {0} {1} 填参数"),
			[](const FArgs& A, FGameTestFlowContext&)
			{
				FString Out = ArgText(A[0]);
				for (int32 Index = 1; Index < A.Num(); ++Index)
				{
					Out.ReplaceInline(*FString::Printf(TEXT("{%d}"), Index - 1), *ArgText(A[Index]));
				}
				return FGameTestValue::FromString(Out);
			});
	}

	void RegisterVectorAndTime()
	{
		GameTestExprFunctions::Add(TEXT("vec"), 2, 3, TEXT("向量"), TEXT("vec(x, y, z?)"), TEXT("拼一个向量"),
			[](const FArgs& A, FGameTestFlowContext&)
			{
				double X = 0.0;
				double Y = 0.0;
				double Z = 0.0;
				if (!Num(A[0], X) || !Num(A[1], Y) || (A.Num() > 2 && !Num(A[2], Z)))
				{
					return BasicTypeError(TEXT("vec 的参数要是数字"));
				}
				return FGameTestValue::FromVector(FVector(X, Y, Z));
			});
		GameTestExprFunctions::Add(TEXT("dist"), 2, 2, TEXT("向量"), TEXT("dist(a, b)"), TEXT("两点距离（向量或 Actor）"),
			[](const FArgs& A, FGameTestFlowContext&)
			{
				FVector L;
				FVector R;
				if (!Vec(A[0], L) || !Vec(A[1], R))
				{
					return BasicTypeError(TEXT("dist 的参数要是向量或 Actor"));
				}
				return FGameTestValue::FromNumber(FVector::Dist(L, R));
			});
		GameTestExprFunctions::Add(TEXT("now"), 0, 0, TEXT("时间"), TEXT("now()"), TEXT("游戏时间（秒）"),
			[](const FArgs&, FGameTestFlowContext& Context)
			{
				return Context.World ? FGameTestValue::FromNumber(Context.World->GetTimeSeconds())
									 : FGameTestValue::Unresolved(TEXT("没有游戏世界"));
			});
	}
}

namespace GameTestBuiltin
{
	void RegisterExprBasic()
	{
		RegisterConvert();
		RegisterMath();
		RegisterText();
		RegisterVectorAndTime();
	}
}
