// Copyright Epic Games, Inc. All Rights Reserved.

#include "CQTest.h"

#include "Core/ForgeJsonText.h"

// 守卫须与 CQTest 宏的实体条件一致（WITH_AUTOMATION_WORKER）；用错宏时会展开成没有注册的
// 空壳结构体——测试不报错、不警告，只是静默消失。
#if WITH_AUTOMATION_WORKER

/**
 * 注释剥离是「配置可读」的唯一实现点。
 *
 * 引擎自带的 JSON 解析器是严格 JSON（Runtime/Json/Public/Serialization/JsonReader.h
 * 的 IsWhitespace 只认空白、NextToken 无 `/` 分支），所以剥离必须由我们自己做对：
 * 剥多了会吃掉字符串里的内容，剥少了配置根本解析不过。
 */
TEST_CLASS_WITH_FLAGS(ForgeJsonTextTest, "AssetForge.JsonText",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	static int32 CountNewlines(const FString& InText)
	{
		int32 Count = 0;
		for (const TCHAR Char : InText)
		{
			if (Char == TEXT('\n'))
			{
				++Count;
			}
		}
		return Count;
	}

	TEST_METHOD(行注释被剥离后可解析)
	{
		const FString Source = TEXT("{\n  // 这是说明\n  \"a\": 1\n}");
		const FString Stripped = ForgeJsonText::StripComments(Source);

		TSharedPtr<FJsonObject> Object;
		FString Error;
		ASSERT_THAT(IsTrue(ForgeJsonText::ParseObject(Stripped, Object, Error),
			TEXT("剥离行注释后应能被引擎的 JSON 解析器接受")));
		ASSERT_THAT(IsTrue(Object.IsValid() && Object->GetIntegerField(TEXT("a")) == 1,
			TEXT("字段值应原样保留")));
	}

	TEST_METHOD(块注释被剥离后可解析)
	{
		const FString Source = TEXT("{\n  /* 多行\n     说明 */\n  \"a\": 2\n}");
		const FString Stripped = ForgeJsonText::StripComments(Source);

		TSharedPtr<FJsonObject> Object;
		FString Error;
		ASSERT_THAT(IsTrue(ForgeJsonText::ParseObject(Stripped, Object, Error),
			TEXT("剥离块注释后应能被解析")));
		ASSERT_THAT(IsTrue(Object.IsValid() && Object->GetIntegerField(TEXT("a")) == 2,
			TEXT("块注释之后的字段应保留")));
	}

	TEST_METHOD(字符串里的双斜杠不受影响)
	{
		const FString Source = TEXT("{ \"url\": \"http://a/b\" }");
		const FString Stripped = ForgeJsonText::StripComments(Source);

		TSharedPtr<FJsonObject> Object;
		FString Error;
		ASSERT_THAT(IsTrue(ForgeJsonText::ParseObject(Stripped, Object, Error),
			TEXT("字符串里的 // 不该让内容被当作注释")));
		ASSERT_THAT(IsTrue(Object.IsValid() && Object->GetStringField(TEXT("url")) == TEXT("http://a/b"),
			TEXT("字符串值应逐字保留")));
	}

	TEST_METHOD(字符串里的块注释标记不受影响)
	{
		const FString Source = TEXT("{ \"a\": \"x /* y */ z\" }");
		const FString Stripped = ForgeJsonText::StripComments(Source);

		TSharedPtr<FJsonObject> Object;
		FString Error;
		ASSERT_THAT(IsTrue(ForgeJsonText::ParseObject(Stripped, Object, Error),
			TEXT("字符串里的 /* */ 不该被当作注释")));
		ASSERT_THAT(IsTrue(Object.IsValid() && Object->GetStringField(TEXT("a")) == TEXT("x /* y */ z"),
			TEXT("字符串值应逐字保留")));
	}

	TEST_METHOD(转义引号不提前结束字符串)
	{
		// 反斜杠转义若没处理，`\"` 会被当成字符串结束，后面的注释剥离就会跑偏。
		const FString Source = TEXT("{ \"a\": \"say \\\" // not a comment\" }");
		const FString Stripped = ForgeJsonText::StripComments(Source);

		TSharedPtr<FJsonObject> Object;
		FString Error;
		ASSERT_THAT(IsTrue(ForgeJsonText::ParseObject(Stripped, Object, Error),
			TEXT("转义引号后的内容仍应是字符串")));
		ASSERT_THAT(IsTrue(Object.IsValid() && Object->GetStringField(TEXT("a")) == TEXT("say \" // not a comment"),
			TEXT("转义引号与其中的斜杠都应逐字保留")));
	}

	TEST_METHOD(换行数保持不变)
	{
		const FString Source = TEXT("{\n/* 注释\n第二行\n*/\n\"a\": 1\n}");
		const FString Stripped = ForgeJsonText::StripComments(Source);

		ASSERT_THAT(IsTrue(CountNewlines(Source) == CountNewlines(Stripped),
			TEXT("剥离注释必须保持换行数，否则报错行号会对不上原文件")));
	}

	TEST_METHOD(未闭合的块注释不越界)
	{
		// 写配置的人漏掉 `*/` 是常见笔误，剥离器不能因此读越界。
		const FString Source = TEXT("{ \"a\": 1 } /* 忘了收尾");
		const FString Stripped = ForgeJsonText::StripComments(Source);

		ASSERT_THAT(IsTrue(Stripped.Contains(TEXT("\"a\""), ESearchCase::CaseSensitive),
			TEXT("未闭合的块注释之后不应崩溃，前面的内容应保留")));
	}

	TEST_METHOD(紧凑序列化可用于比较)
	{
		const TSharedPtr<FJsonObject> First = MakeShared<FJsonObject>();
		First->SetNumberField(TEXT("a"), 1);

		const TSharedPtr<FJsonObject> Second = MakeShared<FJsonObject>();
		Second->SetNumberField(TEXT("a"), 1);

		ASSERT_THAT(IsTrue(ForgeJsonText::ToCondensedString(First) == ForgeJsonText::ToCondensedString(Second),
			TEXT("同内容的两个对象应序列化成相同字符串——差异比较依赖这一点")));
	}

	/** 一段 JSON → 规范形态。对象引用归一就发生在这条路径上。 */
	static FString Canonicalize(const FString& InText)
	{
		TSharedPtr<FJsonObject> Object;
		FString Error;
		if (!ForgeJsonText::ParseObject(InText, Object, Error))
		{
			return FString();
		}

		return ForgeJsonText::ToCanonicalString(Object);
	}

	TEST_METHOD(对象引用的两种写法等价)
	{
		// 手写配置写纯路径、引擎导出写 `类名'路径'`：导入两种都收，比较也必须都认
		// （否则这份能导入成功的配置会被判成「每次都不同」，资产永远收敛不了）。
		const FString Plain = Canonicalize(TEXT("{ \"t\": \"/Game/A/T_X.T_X\" }"));
		const FString ExportForm = Canonicalize(TEXT("{ \"t\": \"/Script/Engine.Texture2D'/Game/A/T_X.T_X'\" }"));

		ASSERT_THAT(IsTrue(Plain == ExportForm, TEXT("两种写法必须判成同一个引用")));
		ASSERT_THAT(IsTrue(Plain.Contains(TEXT("/Game/A/T_X"), ESearchCase::CaseSensitive),
			TEXT("归一后的形态应是纯路径")));
	}

	TEST_METHOD(带子对象后缀的引用不归一末段)
	{
		// `/Game/A/B.C` 里的 C 是子对象，不是「末段与前一段同名」，不能剥。
		const FString SubObject = Canonicalize(TEXT("{ \"t\": \"/Game/A/B.C\" }"));

		ASSERT_THAT(IsTrue(SubObject.Contains(TEXT("/Game/A/B.C"), ESearchCase::CaseSensitive),
			TEXT("子对象路径必须原样保留")));
	}

	TEST_METHOD(普通字符串不被当成引用)
	{
		const FString Text = Canonicalize(TEXT("{ \"t\": \"it's fine'\" }"));

		ASSERT_THAT(IsTrue(Text.Contains(TEXT("it's fine'"), ESearchCase::CaseSensitive),
			TEXT("不像资产路径的字符串应逐字保留")));
	}
};

#endif // WITH_AUTOMATION_WORKER
