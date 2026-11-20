// Copyright Epic Games, Inc. All Rights Reserved.

// 主模块自带的深度检查：接口名和参数名、invoke 的函数、取值来源里的类名、objects 的定位器。
// 选择器（读 WBP 资产的控件树）在 GameTestEditor 里登记。

#include "Api/GameTestApi.h"
#include "Api/GameTestParamKind.h"
#include "Api/GameTestProjectApis.h"
#include "Core/Flow/Deep/GameTestDeepCheck.h"
#include "Core/Flow/GameTestNode.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Ref/GameTestRef.h"
#include "Observe/Sources/GameTestObjectLocator.h"
#include "Observe/Sources/GameTestObjectRoots.h"
#include "Observe/Sources/GameTestPathText.h"
#include "Observe/Sources/GameTestReflectCall.h"
#include "Observe/Sources/GameTestReflectValue.h"
#include "Observe/Sources/GameTestValueSource.h"

namespace GameTestDeepBuiltin
{
	FString StringField(const FJsonObject& InJson, const TCHAR* InField)
	{
		FString Value;
		InJson.TryGetStringField(InField, Value);
		return Value;
	}

	/** 写错名字时的候选：「你是想写 Widget.IsShown 吗」。 */
	FString SuggestApi(const FString& InName)
	{
		TArray<FString> Candidates;
		GameTestApis::Get().Suggest(InName, Candidates);
		return Candidates.Num() > 0 ? FString::Printf(TEXT("，是不是 %s"), *FString::Join(Candidates, TEXT(" / "))) : FString();
	}

	/** 接口：条件 `{"api": 名字, "args": {…}}`、api 步骤。名字在不在、改没改名、参数名对不对、必填的给没给。 */
	class FApiCheck final : public IGameTestDeepCheck
	{
	public:
		virtual FName GetKey() const override { return TEXT("api"); }

		virtual void CheckScript(const FGameTestScript&, FGameTestDeepEnv&) const override
		{
			// 面板可能刚写了 _api.json 的 Lua 段
			GameTestProjectApis::Refresh();
		}

		virtual void CheckObject(const FJsonObject& InJson, FGameTestDeepEnv& InEnv) const override
		{
			const FString Name = StringField(InJson, TEXT("api"));
			if (Name.IsEmpty() || Name.StartsWith(TEXT("$")) || Name.StartsWith(TEXT("=")))
			{
				return;
			}
			++InEnv.Stats.Apis;
			const IGameTestApi* Api = GameTestApis::Get().Find(FName(*Name));
			if (Api == nullptr)
			{
				if (const FString* NewName = GameTestApis::GetRenamed().Find(Name))
				{
					InEnv.Sink.Error(TEXT("api_renamed"), FString::Printf(TEXT("接口 %s 已改名，改用 %s"), *Name, **NewName));
				}
				else
				{
					InEnv.Sink.Error(TEXT("api_missing"), FString::Printf(TEXT("没有接口 %s%s（gt.api 看全部）"), *Name, *SuggestApi(Name)));
				}
				return;
			}
			const TSharedPtr<FJsonObject>* Args = nullptr;
			if (!InJson.TryGetObjectField(TEXT("args"), Args) || Args == nullptr || !Args->IsValid())
			{
				return;
			}
			const FGameTestApiDesc& Desc = Api->GetDesc();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Args)->Values)
			{
				if (!Desc.Args.ContainsByPredicate([&Pair](const FGameTestApiArg& Arg) { return Arg.Name.Equals(Pair.Key, ESearchCase::IgnoreCase); }))
				{
					InEnv.Sink.Error(TEXT("unknown_field"), FString::Printf(TEXT("%s 没有参数 %s（签名：%s）"), *Name, *Pair.Key, *Desc.Signature()));
				}
			}
			for (const FGameTestApiArg& Arg : Desc.Args)
			{
				if (!Arg.bOptional && !(*Args)->HasField(Arg.Name))
				{
					InEnv.Sink.Error(TEXT("arg_missing"), FString::Printf(TEXT("%s 缺参数 %s（签名：%s）"), *Name, *Arg.Name, *Desc.Signature()));
				}
			}
		}
	};

	/** obj.call（2.0.0 前叫 invoke）：函数在不在、能不能从脚本调、参数名对不对。对象要开着游戏才知道是谁时只查静态函数、@sub 和引用的类。 */
	class FInvokeCheck final : public IGameTestDeepCheck
	{
	public:
		virtual FName GetKey() const override { return TEXT("obj.call"); }

		virtual void CheckObject(const FJsonObject& InJson, FGameTestDeepEnv& InEnv) const override
		{
			if (StringField(InJson, TEXT("action")) != TEXT("obj.call"))
			{
				return;
			}
			const FString Function = StringField(InJson, TEXT("function"));
			const TSharedPtr<FJsonValue> Object = InJson.TryGetField(TEXT("obj"));
			if (InJson.HasField(TEXT("part")))
			{
				// 调的是部件上的函数：部件的类要读控件树才知道，运行时再查
				return;
			}
			if (Function.IsEmpty() || Function.StartsWith(TEXT("$")) || Function.StartsWith(TEXT("=")))
			{
				return;
			}
			++InEnv.Stats.Functions;
			UFunction* Found = nullptr;
			FString Error;
			if (!Object.IsValid() || Object->IsNull())
			{
				UObject* Target = nullptr;
				Found = GameTestReflectCall::FindStatic(Function, Target, Error);
			}
			else if (const UClass* Class = ClassOfObject(Object, InEnv, Error))
			{
				Found = GameTestReflectValue::FindFunction(Class, Function);
				if (Found == nullptr)
				{
					Error = FString::Printf(TEXT("%s 上没有函数 %s"), *Class->GetName(), *Function);
				}
			}
			else
			{
				// 对象要运行时才知道（Lua 表、$变量、没开游戏时的 @pc）：这里不报，运行时再查
				return;
			}
			if (Found == nullptr)
			{
				InEnv.Sink.Error(TEXT("fn_missing"), Error);
				return;
			}
			FString Why;
			if (!GameTestParamKind::IsCallable(*Found, Why))
			{
				InEnv.Sink.Error(TEXT("fn_not_callable"), FString::Printf(TEXT("%s 不能从脚本调：%s"), *Found->GetName(), *Why));
				return;
			}
			CheckArgNames(InJson, *Found, InEnv);
		}

	private:
		/** 对象参数在不开游戏时能确定的类：`@sub.类名`；开着游戏时按来源取一次。 */
		static const UClass* ClassOfObject(const TSharedPtr<FJsonValue>& InObject, FGameTestDeepEnv& InEnv, FString& OutError)
		{
			// 对象引用（命名 / 内联）：按 class 找已加载的类
			FString RefText;
			TSharedPtr<FJsonObject> Inline;
			const EGameTestRefForm Form = GameTestRefForm::Classify(InObject, RefText, Inline);
			if (Form == EGameTestRefForm::Named && InEnv.Script.Objects.IsValid())
			{
				const TSharedPtr<FJsonObject>* Def = nullptr;
				if (InEnv.Script.Objects->TryGetObjectField(RefText, Def) && Def != nullptr)
				{
					Inline = *Def;
				}
			}
			if ((Form == EGameTestRefForm::Named || Form == EGameTestRefForm::Inline) && Inline.IsValid())
			{
				FString ClassName;
				Inline->TryGetStringField(TEXT("class"), ClassName);
				// 蓝图类没加载时找不到：不报，运行时再查
				return ClassName.IsEmpty() ? nullptr : GameTestObjectRoots::FindClass(ClassName);
			}
			FString Text;
			if (!InObject->TryGetString(Text) || !Text.StartsWith(TEXT("@")) || Text.StartsWith(TEXT("@lua")))
			{
				return nullptr;
			}
			if (Text.StartsWith(TEXT("@sub.")))
			{
				FString ClassName = Text.Mid(5);
				ClassName.Split(TEXT("."), &ClassName, nullptr);
				const UClass* Class = GameTestObjectRoots::FindClass(ClassName);
				if (Class == nullptr)
				{
					OutError = FString::Printf(TEXT("没有子系统类 %s"), *ClassName);
				}
				return Class;
			}
			if (!InEnv.bGameRunning)
			{
				return nullptr;
			}
			const UObject* Resolved = GameTestPathText::ResolveSourceText(Text, InEnv.Context).GetObject();
			return Resolved ? Resolved->GetClass() : nullptr;
		}

		static void CheckArgNames(const FJsonObject& InJson, const UFunction& InFunction, FGameTestDeepEnv& InEnv)
		{
			const TSharedPtr<FJsonObject>* Args = nullptr;
			if (!InJson.TryGetObjectField(TEXT("args"), Args) || Args == nullptr || !Args->IsValid())
			{
				return;
			}
			const TArray<FGameTestParamInfo> Params = GameTestParamKind::Describe(InFunction);
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Args)->Values)
			{
				if (!Params.ContainsByPredicate([&Pair](const FGameTestParamInfo& Param) { return Param.Name.Equals(Pair.Key, ESearchCase::IgnoreCase); }))
				{
					TArray<FString> Names;
					for (const FGameTestParamInfo& Param : Params)
					{
						Names.Add(Param.Name);
					}
					InEnv.Sink.Error(TEXT("unknown_field"), FString::Printf(TEXT("%s 没有参数 %s（它的参数：%s）"), *InFunction.GetName(), *Pair.Key,
						*FString::Join(Names, TEXT("、"))));
				}
			}
		}
	};

	/** 取值来源：对象里直接写的 `@来源…` 字符串，交给来源自己的 Validate（类名在不在、选择器写法）。 */
	class FSourcePathCheck final : public IGameTestDeepCheck
	{
	public:
		virtual FName GetKey() const override { return TEXT("sources"); }

		virtual void CheckObject(const FJsonObject& InJson, FGameTestDeepEnv& InEnv) const override
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : InJson.Values)
			{
				FString Text;
				// target 里的 @ 是控件句柄（@btnOk），prop 是属性路径（@self.X、@部件.X），都不是来源
				if (Pair.Key == TEXT("target") || Pair.Key == TEXT("prop") || !Pair.Value.IsValid() || !Pair.Value->TryGetString(Text) || !Text.StartsWith(TEXT("@")))
				{
					continue;
				}
				FName Source;
				FString Selector, Error;
				TArray<FGameTestPathSegment> Segments;
				if (!GameTestPathText::ParseSource(Text, Source, Selector, Segments, Error))
				{
					continue;
				}
				const IGameTestValueSource* Found = GameTestValueSources::Get().Find(Source);
				if (Found == nullptr)
				{
					continue;
				}
				++InEnv.Stats.Paths;
				FGameTestDiagSink::FScope Scope(InEnv.Sink, Pair.Key);
				Found->Validate(Selector, Segments, InEnv.Sink);
			}
		}
	};

	/** objects：引用写法、类名在不在（蓝图类没加载时找不到，只给警告）。 */
	class FObjectsCheck final : public IGameTestDeepCheck
	{
	public:
		virtual FName GetKey() const override { return TEXT("objects"); }

		virtual void CheckObject(const FJsonObject&, FGameTestDeepEnv&) const override {}

		virtual void CheckScript(const FGameTestScript& InScript, FGameTestDeepEnv& InEnv) const override
		{
			if (!InScript.Objects.IsValid())
			{
				return;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : InScript.Objects->Values)
			{
				FGameTestDiagSink::FScope Scope(InEnv.Sink, TEXT("objects.") + Pair.Key);
				++InEnv.Stats.Objects;
				const TSharedPtr<FJsonObject> Json = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
				FGameTestObjectLocator Locator;
				TArray<FString> Errors;
				if (!Json.IsValid() || !FGameTestObjectLocator::Parse(*Json, Locator, Errors))
				{
					InEnv.Sink.Error(TEXT("bad_type"), Errors.Num() > 0 ? FString::Join(Errors, TEXT("；")) : FString(TEXT("引用要写成对象 {class, ident, …}")));
					continue;
				}
				if (GameTestObjectRoots::FindClass(Locator.Class) == nullptr)
				{
					InEnv.Sink.Warning(TEXT("ref_bad_class"),
						FString::Printf(TEXT("没找到类 %s（写错了，或蓝图类还没加载过）"), *Locator.Class));
				}
			}
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterDeepChecks()
	{
		using namespace GameTestDeepBuiltin;
		TGameTestRegistry<IGameTestDeepCheck>& Checks = GameTestDeepChecks::Get();
		Checks.Register(MakeShared<FApiCheck>());
		Checks.Register(MakeShared<FInvokeCheck>());
		Checks.Register(MakeShared<FSourcePathCheck>());
		Checks.Register(MakeShared<FObjectsCheck>());
	}
}
