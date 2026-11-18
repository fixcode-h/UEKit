// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Expr/GameTestExpr.h"
#include "Core/Expr/GameTestExprAst.h"
#include "Observe/Sources/GameTestValueSource.h"

/**
 * 表达式的词法和语法分析（协议第 3 节的文法）。
 *
 * 优先级从低到高：`?:` → `or` → `and` → `not` → 比较 / `in` → `+ -` → `* / %` → 一元 `-` → 成员 / 下标 / 调用。
 * `&&`、`||`、`!` 当作 `and`、`or`、`not` 的别名：Agent 写惯了 C 系写法，拒掉只会多一轮来回。
 */
namespace
{
	enum class ETok : uint8
	{
		End,
		Number,
		String,
		Ident,
		Var,
		Source,
		Punct,
	};

	struct FTok
	{
		ETok Kind = ETok::End;
		FString Text;
		/** Source 的 `[...]` 原文。 */
		FString Selector;
		double Number = 0.0;
		int32 Pos = 0;
	};

	bool IsIdentChar(TCHAR C)
	{
		// 非 ASCII 字符都算名字的一部分：项目接口名是 `项目.QuestTest.X`，变量也可能起中文名。
		return FChar::IsAlnum(C) || C == TEXT('_') || C >= 0x80;
	}

	bool IsIdentStart(TCHAR C)
	{
		return FChar::IsAlpha(C) || C == TEXT('_') || C >= 0x80;
	}

	class FLexer
	{
	public:
		FLexer(const FString& InText)
			: Text(InText)
		{
		}

		bool Run(TArray<FTok>& OutTokens, FString& OutError)
		{
			while (true)
			{
				SkipSpace();
				if (At >= Text.Len())
				{
					FTok& End = OutTokens.AddDefaulted_GetRef();
					End.Pos = At;
					return true;
				}
				FTok Tok;
				Tok.Pos = At;
				if (!Next(Tok, OutError))
				{
					return false;
				}
				OutTokens.Add(MoveTemp(Tok));
			}
		}

	private:
		void SkipSpace()
		{
			while (At < Text.Len() && FChar::IsWhitespace(Text[At]))
			{
				++At;
			}
		}

		bool Fail(FString& OutError, const FString& InWhat) const
		{
			OutError = FString::Printf(TEXT("第 %d 个字附近：%s"), At + 1, *InWhat);
			return false;
		}

		FString ReadName()
		{
			const int32 Start = At;
			while (At < Text.Len() && IsIdentChar(Text[At]))
			{
				++At;
			}
			return Text.Mid(Start, At - Start);
		}

		bool Next(FTok& Tok, FString& OutError)
		{
			const TCHAR C = Text[At];
			if (FChar::IsDigit(C) || (C == TEXT('.') && At + 1 < Text.Len() && FChar::IsDigit(Text[At + 1])))
			{
				return ReadNumber(Tok, OutError);
			}
			if (C == TEXT('"') || C == TEXT('\''))
			{
				return ReadString(Tok, OutError);
			}
			if (C == TEXT('$'))
			{
				++At;
				Tok.Kind = ETok::Var;
				Tok.Text = ReadName();
				return Tok.Text.IsEmpty() ? Fail(OutError, TEXT("$ 后面要跟变量名")) : true;
			}
			if (C == TEXT('@'))
			{
				return ReadSource(Tok, OutError);
			}
			if (IsIdentStart(C))
			{
				Tok.Kind = ETok::Ident;
				Tok.Text = ReadName();
				return true;
			}
			return ReadPunct(Tok, OutError);
		}

		bool ReadNumber(FTok& Tok, FString& OutError)
		{
			const int32 Start = At;
			while (At < Text.Len() && (FChar::IsDigit(Text[At]) || Text[At] == TEXT('.')))
			{
				++At;
			}
			// 科学计数法（1e-3）：配置表导出的浮点常这么写。
			if (At < Text.Len() && (Text[At] == TEXT('e') || Text[At] == TEXT('E')))
			{
				++At;
				if (At < Text.Len() && (Text[At] == TEXT('-') || Text[At] == TEXT('+')))
				{
					++At;
				}
				while (At < Text.Len() && FChar::IsDigit(Text[At]))
				{
					++At;
				}
			}
			const FString Raw = Text.Mid(Start, At - Start);
			if (Raw.Len() > 0 && At < Text.Len() && IsIdentStart(Text[At]))
			{
				return Fail(OutError, FString::Printf(TEXT("数字 %s 后面紧跟着字母"), *Raw));
			}
			Tok.Kind = ETok::Number;
			Tok.Number = FCString::Atod(*Raw);
			Tok.Text = Raw;
			return true;
		}

		bool ReadString(FTok& Tok, FString& OutError)
		{
			const TCHAR Quote = Text[At++];
			FString Out;
			while (At < Text.Len() && Text[At] != Quote)
			{
				TCHAR C = Text[At++];
				if (C == TEXT('\\') && At < Text.Len())
				{
					const TCHAR E = Text[At++];
					C = E == TEXT('n') ? TEXT('\n') : (E == TEXT('t') ? TEXT('\t') : E);
				}
				Out.AppendChar(C);
			}
			if (At >= Text.Len())
			{
				return Fail(OutError, TEXT("字符串缺结尾的引号"));
			}
			++At;
			Tok.Kind = ETok::String;
			Tok.Text = Out;
			return true;
		}

		bool ReadSource(FTok& Tok, FString& OutError)
		{
			++At;
			Tok.Kind = ETok::Source;
			Tok.Text = ReadName();
			if (Tok.Text.IsEmpty())
			{
				return Fail(OutError, TEXT("@ 后面要跟来源名（@gi、@sub、@actor…）"));
			}
			// 只有要选择器的来源才把紧跟的 [...] 当选择器（@actor[tag=Boss]），别的当下标。
			const IGameTestValueSource* Source = GameTestValueSources::Get().Find(FName(*Tok.Text));
			const bool bWantsSelector = Source ? Source->UsesSelector() : Tok.Text.Equals(TEXT("actor"), ESearchCase::IgnoreCase);
			if (bWantsSelector && At < Text.Len() && Text[At] == TEXT('['))
			{
				return ReadSelector(Tok, OutError);
			}
			return true;
		}

		bool ReadSelector(FTok& Tok, FString& OutError)
		{
			int32 Depth = 0;
			const int32 Start = At + 1;
			TCHAR Quote = 0;
			for (; At < Text.Len(); ++At)
			{
				const TCHAR C = Text[At];
				if (Quote)
				{
					Quote = C == Quote ? 0 : Quote;
					continue;
				}
				if (C == TEXT('"') || C == TEXT('\''))
				{
					Quote = C;
				}
				else if (C == TEXT('['))
				{
					++Depth;
				}
				else if (C == TEXT(']') && --Depth == 0)
				{
					Tok.Selector = Text.Mid(Start, At - Start).TrimStartAndEnd();
					++At;
					return true;
				}
			}
			return Fail(OutError, TEXT("@来源[...] 缺右方括号"));
		}

		bool ReadPunct(FTok& Tok, FString& OutError)
		{
			static const TCHAR* Two[] = { TEXT("=="), TEXT("!="), TEXT(">="), TEXT("<="), TEXT("&&"), TEXT("||") };
			for (const TCHAR* Op : Two)
			{
				if (Text.Mid(At, 2) == Op)
				{
					Tok.Kind = ETok::Punct;
					Tok.Text = Op;
					At += 2;
					return true;
				}
			}
			const TCHAR C = Text[At];
			if (FCString::Strchr(TEXT("+-*/%<>()[]{},.?:!"), C))
			{
				Tok.Kind = ETok::Punct;
				Tok.Text = FString(1, &C);
				++At;
				return true;
			}
			if (C == TEXT('='))
			{
				return Fail(OutError, TEXT("比较相等写 ==（表达式里没有赋值）"));
			}
			return Fail(OutError, FString::Printf(TEXT("不认识的字符 %c"), C));
		}

		const FString& Text;
		int32 At = 0;
	};

	using FNodeRef = TSharedRef<FGameTestExprNode>;

	class FParser
	{
	public:
		FParser(const TArray<FTok>& InTokens)
			: Tokens(InTokens)
		{
		}

		TSharedPtr<FGameTestExprNode> Run(FString& OutError)
		{
			TSharedPtr<FGameTestExprNode> Root = Ternary();
			if (Root.IsValid() && Peek().Kind != ETok::End)
			{
				Fail(FString::Printf(TEXT("多出来的 %s"), *Peek().Text));
			}
			if (!Error.IsEmpty())
			{
				OutError = Error;
				return nullptr;
			}
			return Root;
		}

	private:
		const FTok& Peek(int32 InAhead = 0) const
		{
			return Tokens[FMath::Min(Cursor + InAhead, Tokens.Num() - 1)];
		}

		bool IsPunct(const TCHAR* InText, int32 InAhead = 0) const
		{
			const FTok& Tok = Peek(InAhead);
			return Tok.Kind == ETok::Punct && Tok.Text == InText;
		}

		bool IsWord(const TCHAR* InWord) const
		{
			const FTok& Tok = Peek();
			return Tok.Kind == ETok::Ident && Tok.Text.Equals(InWord, ESearchCase::IgnoreCase);
		}

		bool Accept(const TCHAR* InPunct)
		{
			if (IsPunct(InPunct))
			{
				++Cursor;
				return true;
			}
			return false;
		}

		bool Expect(const TCHAR* InPunct, const TCHAR* InWhat)
		{
			if (Accept(InPunct))
			{
				return true;
			}
			Fail(FString::Printf(TEXT("缺%s"), InWhat));
			return false;
		}

		TSharedPtr<FGameTestExprNode> Fail(const FString& InWhat)
		{
			if (Error.IsEmpty())
			{
				Error = FString::Printf(TEXT("第 %d 个字附近：%s"), Peek().Pos + 1, *InWhat);
			}
			return nullptr;
		}

		FNodeRef Make(EGameTestExprKind InKind, int32 InPos) const
		{
			FNodeRef Node = MakeShared<FGameTestExprNode>();
			Node->Kind = InKind;
			Node->Pos = InPos;
			return Node;
		}

		TSharedPtr<FGameTestExprNode> MakeBinary(const FString& InOp, const TSharedPtr<FGameTestExprNode>& L, const TSharedPtr<FGameTestExprNode>& R, int32 InPos)
		{
			if (!L.IsValid() || !R.IsValid())
			{
				return nullptr;
			}
			FNodeRef Node = Make(EGameTestExprKind::Binary, InPos);
			Node->Op = InOp;
			Node->Children = { L.ToSharedRef(), R.ToSharedRef() };
			return Node;
		}

		TSharedPtr<FGameTestExprNode> Ternary()
		{
			TSharedPtr<FGameTestExprNode> Cond = Or();
			if (!Cond.IsValid() || !IsPunct(TEXT("?")))
			{
				return Cond;
			}
			const int32 Pos = Peek().Pos;
			++Cursor;
			TSharedPtr<FGameTestExprNode> A = Ternary();
			if (!A.IsValid() || !Expect(TEXT(":"), TEXT(" ?: 的冒号")))
			{
				return nullptr;
			}
			TSharedPtr<FGameTestExprNode> B = Ternary();
			if (!B.IsValid())
			{
				return nullptr;
			}
			FNodeRef Node = Make(EGameTestExprKind::Ternary, Pos);
			Node->Children = { Cond.ToSharedRef(), A.ToSharedRef(), B.ToSharedRef() };
			return Node;
		}

		TSharedPtr<FGameTestExprNode> Or()
		{
			TSharedPtr<FGameTestExprNode> Left = And();
			while (Left.IsValid() && (IsWord(TEXT("or")) || IsPunct(TEXT("||"))))
			{
				const int32 Pos = Peek().Pos;
				++Cursor;
				Left = MakeBinary(TEXT("or"), Left, And(), Pos);
			}
			return Left;
		}

		TSharedPtr<FGameTestExprNode> And()
		{
			TSharedPtr<FGameTestExprNode> Left = Not();
			while (Left.IsValid() && (IsWord(TEXT("and")) || IsPunct(TEXT("&&"))))
			{
				const int32 Pos = Peek().Pos;
				++Cursor;
				Left = MakeBinary(TEXT("and"), Left, Not(), Pos);
			}
			return Left;
		}

		TSharedPtr<FGameTestExprNode> Not()
		{
			if (IsWord(TEXT("not")) || IsPunct(TEXT("!")))
			{
				const int32 Pos = Peek().Pos;
				++Cursor;
				TSharedPtr<FGameTestExprNode> Inner = Not();
				if (!Inner.IsValid())
				{
					return nullptr;
				}
				FNodeRef Node = Make(EGameTestExprKind::Unary, Pos);
				Node->Op = TEXT("not");
				Node->Children = { Inner.ToSharedRef() };
				return Node;
			}
			return Compare();
		}

		TSharedPtr<FGameTestExprNode> Compare()
		{
			TSharedPtr<FGameTestExprNode> Left = Additive();
			if (!Left.IsValid())
			{
				return nullptr;
			}
			static const TCHAR* Ops[] = { TEXT("=="), TEXT("!="), TEXT(">="), TEXT("<="), TEXT(">"), TEXT("<") };
			for (const TCHAR* Op : Ops)
			{
				if (IsPunct(Op))
				{
					const int32 Pos = Peek().Pos;
					++Cursor;
					return MakeBinary(Op, Left, Additive(), Pos);
				}
			}
			// `x in [..]`、`x not in [..]`
			if (IsWord(TEXT("in")))
			{
				const int32 Pos = Peek().Pos;
				++Cursor;
				return MakeBinary(TEXT("in"), Left, Additive(), Pos);
			}
			if (IsWord(TEXT("not")) && Peek(1).Kind == ETok::Ident && Peek(1).Text.Equals(TEXT("in"), ESearchCase::IgnoreCase))
			{
				const int32 Pos = Peek().Pos;
				Cursor += 2;
				return MakeBinary(TEXT("not in"), Left, Additive(), Pos);
			}
			return Left;
		}

		TSharedPtr<FGameTestExprNode> Additive()
		{
			TSharedPtr<FGameTestExprNode> Left = Multiplicative();
			while (Left.IsValid() && (IsPunct(TEXT("+")) || IsPunct(TEXT("-"))))
			{
				const FString Op = Peek().Text;
				const int32 Pos = Peek().Pos;
				++Cursor;
				Left = MakeBinary(Op, Left, Multiplicative(), Pos);
			}
			return Left;
		}

		TSharedPtr<FGameTestExprNode> Multiplicative()
		{
			TSharedPtr<FGameTestExprNode> Left = Unary();
			while (Left.IsValid() && (IsPunct(TEXT("*")) || IsPunct(TEXT("/")) || IsPunct(TEXT("%"))))
			{
				const FString Op = Peek().Text;
				const int32 Pos = Peek().Pos;
				++Cursor;
				Left = MakeBinary(Op, Left, Unary(), Pos);
			}
			return Left;
		}

		TSharedPtr<FGameTestExprNode> Unary()
		{
			if (IsPunct(TEXT("-")))
			{
				const int32 Pos = Peek().Pos;
				++Cursor;
				TSharedPtr<FGameTestExprNode> Inner = Unary();
				if (!Inner.IsValid())
				{
					return nullptr;
				}
				// 负数字面量直接折叠，`-2` 不必每次求值都算一遍取反。
				if (Inner->Kind == EGameTestExprKind::Literal && Inner->Value.IsNumber())
				{
					Inner->Value = FGameTestValue::FromNumber(-Inner->Value.GetNumber());
					Inner->Pos = Pos;
					return Inner;
				}
				FNodeRef Node = Make(EGameTestExprKind::Unary, Pos);
				Node->Op = TEXT("-");
				Node->Children = { Inner.ToSharedRef() };
				return Node;
			}
			return Postfix(Primary());
		}

		/** `.成员`、`[下标]`、`.Fn(参数)` 接在任何基本式后面。基本式不是路径时包成「根是表达式」的路径。 */
		TSharedPtr<FGameTestExprNode> Postfix(TSharedPtr<FGameTestExprNode> InBase)
		{
			if (!InBase.IsValid())
			{
				return nullptr;
			}
			TSharedPtr<FGameTestExprNode> Path = InBase;
			while (IsPunct(TEXT(".")) || IsPunct(TEXT("[")))
			{
				if (Path->Kind != EGameTestExprKind::Path)
				{
					FNodeRef Wrap = Make(EGameTestExprKind::Path, Path->Pos);
					Wrap->Root = EGameTestPathRoot::Expr;
					Wrap->Children = { Path.ToSharedRef() };
					Path = Wrap;
				}
				FGameTestExprPostfix Step;
				if (Accept(TEXT(".")))
				{
					if (Peek().Kind != ETok::Ident)
					{
						return Fail(TEXT(". 后面要跟成员名"));
					}
					Step.Name = Peek().Text;
					++Cursor;
					Step.Kind = FGameTestExprPostfix::EKind::Member;
					if (IsPunct(TEXT("(")))
					{
						Step.Kind = FGameTestExprPostfix::EKind::Call;
						if (!Args(TEXT(")"), Step.Args))
						{
							return nullptr;
						}
					}
				}
				else
				{
					++Cursor;
					TSharedPtr<FGameTestExprNode> Index = Ternary();
					if (!Index.IsValid() || !Expect(TEXT("]"), TEXT("右方括号")))
					{
						return nullptr;
					}
					Step.Kind = FGameTestExprPostfix::EKind::Index;
					Step.Args.Add(Index.ToSharedRef());
				}
				Path->Postfix.Add(MoveTemp(Step));
			}
			return Path;
		}

		/** 读 `(a, b)` 或 `[a, b]`：当前在开括号上。 */
		bool Args(const TCHAR* InClose, TArray<FNodeRef>& OutArgs)
		{
			++Cursor;
			if (Accept(InClose))
			{
				return true;
			}
			while (true)
			{
				TSharedPtr<FGameTestExprNode> Arg = Ternary();
				if (!Arg.IsValid())
				{
					return false;
				}
				OutArgs.Add(Arg.ToSharedRef());
				if (Accept(InClose))
				{
					return true;
				}
				if (!Expect(TEXT(","), TEXT("逗号或右括号")))
				{
					return false;
				}
			}
		}

		TSharedPtr<FGameTestExprNode> Primary()
		{
			const FTok& Tok = Peek();
			switch (Tok.Kind)
			{
			case ETok::Number:
			{
				FNodeRef Node = Make(EGameTestExprKind::Literal, Tok.Pos);
				Node->Value = FGameTestValue::FromNumber(Tok.Number);
				++Cursor;
				return Node;
			}
			case ETok::String:
			{
				FNodeRef Node = Make(EGameTestExprKind::Literal, Tok.Pos);
				Node->Value = FGameTestValue::FromString(Tok.Text);
				++Cursor;
				return Node;
			}
			case ETok::Var:
			case ETok::Source:
			{
				FNodeRef Node = Make(EGameTestExprKind::Path, Tok.Pos);
				Node->Root = Tok.Kind == ETok::Var ? EGameTestPathRoot::Var : EGameTestPathRoot::Source;
				Node->RootName = Tok.Text;
				Node->Selector = Tok.Selector;
				++Cursor;
				return Node;
			}
			case ETok::Ident:
				return Word();
			case ETok::Punct:
				return Bracketed();
			default:
				return Fail(TEXT("表达式不完整"));
			}
		}

		/** `true` / `false` / `null`，或函数调用（名字可以带点：`Widget.IsShown(...)`）。 */
		TSharedPtr<FGameTestExprNode> Word()
		{
			const FTok& Tok = Peek();
			const int32 Pos = Tok.Pos;
			if (Tok.Text == TEXT("true") || Tok.Text == TEXT("false") || Tok.Text == TEXT("null"))
			{
				FNodeRef Node = Make(EGameTestExprKind::Literal, Pos);
				Node->Value = Tok.Text == TEXT("null") ? FGameTestValue::Null() : FGameTestValue::FromBool(Tok.Text == TEXT("true"));
				++Cursor;
				return Node;
			}
			FString Name = Tok.Text;
			++Cursor;
			while (IsPunct(TEXT(".")) && Peek(1).Kind == ETok::Ident)
			{
				Name += TEXT(".") + Peek(1).Text;
				Cursor += 2;
			}
			if (!IsPunct(TEXT("(")))
			{
				return Fail(FString::Printf(TEXT("%s 不是变量也不是函数：变量写 $%s，字符串加引号，函数要带括号"), *Name, *Name));
			}
			FNodeRef Node = Make(EGameTestExprKind::Call, Pos);
			Node->Function = Name;
			if (!Args(TEXT(")"), Node->Children))
			{
				return nullptr;
			}
			return Node;
		}

		TSharedPtr<FGameTestExprNode> Bracketed()
		{
			const int32 Pos = Peek().Pos;
			if (Accept(TEXT("(")))
			{
				TSharedPtr<FGameTestExprNode> Inner = Ternary();
				if (!Inner.IsValid() || !Expect(TEXT(")"), TEXT("右括号")))
				{
					return nullptr;
				}
				return Inner;
			}
			if (IsPunct(TEXT("[")))
			{
				FNodeRef Node = Make(EGameTestExprKind::ArrayLit, Pos);
				return Args(TEXT("]"), Node->Children) ? TSharedPtr<FGameTestExprNode>(Node) : nullptr;
			}
			if (Accept(TEXT("{")))
			{
				return MapLiteral(Pos);
			}
			return Fail(FString::Printf(TEXT("这里不能是 %s"), *Peek().Text));
		}

		TSharedPtr<FGameTestExprNode> MapLiteral(int32 InPos)
		{
			FNodeRef Node = Make(EGameTestExprKind::MapLit, InPos);
			if (Accept(TEXT("}")))
			{
				return Node;
			}
			while (true)
			{
				const FTok& Key = Peek();
				if (Key.Kind != ETok::String && Key.Kind != ETok::Ident)
				{
					return Fail(TEXT("对象的键要是名字或字符串"));
				}
				Node->Keys.Add(Key.Text);
				++Cursor;
				if (!Expect(TEXT(":"), TEXT("键后面的冒号")))
				{
					return nullptr;
				}
				TSharedPtr<FGameTestExprNode> Value = Ternary();
				if (!Value.IsValid())
				{
					return nullptr;
				}
				Node->Children.Add(Value.ToSharedRef());
				if (Accept(TEXT("}")))
				{
					return Node;
				}
				if (!Expect(TEXT(","), TEXT("逗号或右花括号")))
				{
					return nullptr;
				}
			}
		}

		const TArray<FTok>& Tokens;
		int32 Cursor = 0;
		FString Error;
	};
}

TSharedPtr<const FGameTestExpr> FGameTestExpr::Parse(const FString& InText, FString& OutError)
{
	if (InText.Len() > MaxLength)
	{
		OutError = FString::Printf(TEXT("表达式太长（%d 字，上限 %d）：复杂逻辑写成项目接口"), InText.Len(), MaxLength);
		return nullptr;
	}
	if (InText.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("表达式是空的");
		return nullptr;
	}
	TArray<FTok> Tokens;
	FLexer Lexer(InText);
	if (!Lexer.Run(Tokens, OutError))
	{
		return nullptr;
	}
	FParser Parser(Tokens);
	const TSharedPtr<FGameTestExprNode> Root = Parser.Run(OutError);
	if (!Root.IsValid())
	{
		return nullptr;
	}
	return MakeShared<const FGameTestExpr>(InText, Root.ToSharedRef());
}

FGameTestExpr::FGameTestExpr(const FString& InText, TSharedRef<FGameTestExprNode> InRoot)
	: Text(InText)
	, Root(InRoot)
{
}

FGameTestExpr::~FGameTestExpr() = default;
