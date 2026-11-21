// Copyright Epic Games, Inc. All Rights Reserved.

// 自带接口：对象（协议第 8.1 节 Object.*）。找对象走定位器（第 7 节），读写走反射或 UELink 的 set / lua.set，
// 调函数走 GameTestReflectCall（和 invoke 同一份）。

#include "AgentPanelBridge.h"
#include "Api/Builtin/GameTestApiArgs.h"
#include "Api/GameTestApi.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Lua/GameTestLuaBridge.h"
#include "Observe/Sources/GameTestObjectLocator.h"
#include "Observe/Sources/GameTestObjectRoots.h"
#include "Observe/Sources/GameTestReflectCall.h"
#include "Observe/Sources/GameTestReflectValue.h"
#include "Observe/Sources/GameTestValueSource.h"

namespace GA = GameTestApiArgs;
using GameTestApiArgs::Opt;
using GameTestApiArgs::Req;

namespace
{
	using FCall = FGameTestApiCall;
	using FCtx = FGameTestFlowContext;
	using EState = EGameTestCallState;

	/** 定位一次；找到写 payload.object。 */
	EGameTestLocate LocateOnce(const FGameTestObjectLocator& InLocator, FCtx& InContext, FGameTestLocateResult& OutResult)
	{
		const EGameTestLocate State = GameTestObjectLocate::Locate(InLocator, InContext, OutResult);
		if (State == EGameTestLocate::Found && InContext.StepPayload.IsValid())
		{
			InContext.StepPayload->SetObjectField(TEXT("ref"), OutResult.ToJson(FString(), InLocator));
		}
		return State;
	}

	EState FinishLocate(FCall& InOutCall, EGameTestLocate InState, const FGameTestLocateResult& InResult)
	{
		switch (InState)
		{
		case EGameTestLocate::Found:
			return InOutCall.Done(FGameTestValue::FromObject(InResult.Picked));
		case EGameTestLocate::Ambiguous:
			return InOutCall.Fail(TEXT("ref_ambiguous"), InResult.Message);
		case EGameTestLocate::NotFound:
			return InOutCall.Fail(TEXT("ref_not_found"), InResult.Message);
		default:
			return InOutCall.Fail(TEXT("arg_type"), InResult.Message);
		}
	}

	EState Find(FCall& C, FCtx& X)
	{
		TSharedRef<FGameTestObjectLocator> Locator = MakeShared<FGameTestObjectLocator>();
		FString Error;
		if (!GA::Locator(C.Arg(0), *Locator, Error))
		{
			GA::ArgError(C, 0, TEXT("loc"), Error);
			return EState::Failed;
		}
		FGameTestLocateResult Result;
		const EGameTestLocate State = LocateOnce(*Locator, X, Result);
		// 只有步骤里（不在条件 / 表达式里）才等：对象常常晚一两帧才生成（切图、刷怪）。超时由步骤的 timeoutMs 管。
		if (State != EGameTestLocate::NotFound || X.IsReadOnly())
		{
			return FinishLocate(C, State, Result);
		}
		FCall* Call = &C;
		FCtx* Context = &X;
		C.Poll = [Call, Context, Locator](double)
		{
			FGameTestLocateResult Again;
			const EGameTestLocate Next = LocateOnce(*Locator, *Context, Again);
			return Next == EGameTestLocate::NotFound ? EState::Pending : FinishLocate(*Call, Next, Again);
		};
		return EState::Pending;
	}

	EState FindAll(FCall& C, FCtx& X, bool bCountOnly, bool bExistsOnly)
	{
		FGameTestObjectLocator Locator;
		FString Error;
		if (!GA::Locator(C.Arg(0), Locator, Error))
		{
			GA::ArgError(C, 0, TEXT("loc"), Error);
			return EState::Failed;
		}
		TArray<UObject*> Objects;
		if (!GameTestObjectLocate::FindAll(Locator, X, Objects, Error))
		{
			return C.Fail(TEXT("ref_not_found"), Error);
		}
		if (bCountOnly)
		{
			return C.Done(FGameTestValue::FromNumber(Objects.Num()));
		}
		if (bExistsOnly)
		{
			return C.Done(FGameTestValue::FromBool(Objects.Num() > 0));
		}
		TArray<FGameTestValue> Items;
		Items.Reserve(Objects.Num());
		for (UObject* Object : Objects)
		{
			Items.Add(FGameTestValue::FromObject(Object));
		}
		return C.Done(FGameTestValue::FromArray(MoveTemp(Items)));
	}

	/** 取对象参数，取不到写 arg_object 错误。 */
	UObject* NeedObject(FCall& C, int32 InIndex, FCtx& X)
	{
		FString Error;
		UObject* Object = GA::Object(C.Arg(InIndex), X, Error);
		if (Object == nullptr)
		{
			C.Fail(TEXT("arg_object"), FString::Printf(TEXT("第 %d 个参数 obj：%s"), InIndex + 1, *Error));
		}
		return Object;
	}

	EState Set(FCall& C, FCtx& X)
	{
		UObject* Object = NeedObject(C, 0, X);
		if (Object == nullptr)
		{
			return EState::Failed;
		}
		const FString Path = GA::Text(C.Arg(1));
		if (Path.IsEmpty())
		{
			GA::ArgError(C, 1, TEXT("path"), TEXT("要写属性名或路径（Health、Stats.Level、#hp）"));
			return EState::Failed;
		}
		const TSharedPtr<FJsonValue> Value = C.Arg(2).ToJson();
		FString Error;
		// Lua 字段：`#hp`（属性路径 v5 的写法，`@#hp` 也认）；旧的 `@lua.hp` 报 path_old
		if (Path.StartsWith(TEXT("#")) || Path.StartsWith(TEXT("@#")))
		{
			const FString LuaPath = Path.Mid(Path.StartsWith(TEXT("@#")) ? 2 : 1);
			if (!GameTestLuaBridge::Write(X.World, Object, GameTestLuaBridge::SplitPath(LuaPath), Value, Error))
			{
				return C.Fail(TEXT("path_missing"), Error);
			}
			return C.Done(FGameTestValue::Null());
		}
		if (Path.StartsWith(TEXT("@lua."), ESearchCase::IgnoreCase) || Path.StartsWith(TEXT("@self."), ESearchCase::IgnoreCase))
		{
			return C.Fail(TEXT("path_old"), FString::Printf(TEXT("「%s」是旧写法：Lua 字段写 #字段，属性直接写属性名（或 @.属性）"), *Path));
		}
		// UELink 的 set 认的是属性名路径（Stats.Level）：v5 的 `@.` 前缀去掉
		const FString PropPath = Path.StartsWith(TEXT("@.")) ? Path.Mid(2) : Path;
		// 属性写入复用 UELink 的 set：类型转换、运行时走 setter（UMG 的可见性、颜色才会刷新到屏幕）都在那边。
		const TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("path"), Object->GetPathName());
		Args->SetStringField(TEXT("prop"), PropPath);
		Args->SetField(TEXT("value"), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
		Args->SetBoolField(TEXT("load"), false);
		TSharedPtr<FJsonObject> Out;
		if (!FAgentPanelBridge::Get().CallOp(TEXT("set"), Args, Out, Error))
		{
			return C.Fail(TEXT("arg_type"), FString::Printf(TEXT("改 %s.%s 失败：%s"), *GA::NameOf(Object), *Path, *Error));
		}
		bool bReplicated = false;
		if (Out.IsValid() && Out->TryGetBoolField(TEXT("replicated"), bReplicated) && bReplicated && X.StepPayload.IsValid())
		{
			// 网络同步的属性在客户端改了会被服务器的值盖回去：提示写在报告里，结果对不上时一眼看到原因。
			X.StepPayload->SetStringField(TEXT("note"), TEXT("这是网络同步属性：在客户端改会被服务器盖回，要改就对服务器世界的对象改（client: \"server\"）"));
		}
		return C.Done(FGameTestValue::Null());
	}

	EState CallFn(FCall& C, FCtx& X)
	{
		UObject* Object = NeedObject(C, 0, X);
		if (Object == nullptr)
		{
			return EState::Failed;
		}
		const FString Name = GA::Text(C.Arg(1));
		const FGameTestValue& Args = C.Arg(2);
		if (Args.Type == EGameTestValueType::Map)
		{
			// 按参数名给只对反射函数有意义（Lua 函数按顺序）
			UFunction* Function = GameTestReflectValue::FindFunction(Object->GetClass(), Name);
			if (Function == nullptr)
			{
				return C.Fail(TEXT("fn_missing"), FString::Printf(TEXT("%s 上没有函数 %s（Lua 函数的参数按顺序给数组）"), *GA::NameOf(Object), *Name));
			}
			GameTestReflectCall::FArgs Named;
			Named.Named = &Args.GetMap();
			const FGameTestValue Result = GameTestReflectCall::Call(Object, Function, Named, X);
			return Result.IsUnresolved() ? C.Fail(Result.GetCode(), Result.GetString()) : C.Done(Result);
		}
		TArray<FGameTestValue> Positional;
		if (Args.Type == EGameTestValueType::Array)
		{
			Positional = Args.GetArray();
		}
		else if (!Args.IsNull())
		{
			Positional.Add(Args);
		}
		// 按顺序：成员访问器依次试（反射函数 → Lua 实例表上的函数），和表达式里 obj.Fn(a, b) 一样
		const FGameTestValue Result = GameTestPath::CallMethod(FGameTestValue::FromObject(Object), Name, Positional, X);
		return Result.IsUnresolved() ? C.Fail(Result.GetCode(), Result.GetString()) : C.Done(Result);
	}
}

namespace GameTestBuiltin
{
	void RegisterApiObject()
	{
		const TCHAR* Cat = TEXT("对象");
		GameTestApis::Add(TEXT("Object.Find"), Cat, TEXT("按定位器找一个对象（第 7 节规则）；步骤里找不到会等到超时"),
			{ Req(TEXT("loc"), TEXT("loc"), TEXT("类名或 {class, in, key, component, near}")) }, TEXT("obj"), true, &Find);
		GameTestApis::Add(TEXT("Object.FindAll"), Cat, TEXT("按定位器找全部对象，不等待"),
			{ Req(TEXT("loc"), TEXT("loc")) }, TEXT("[obj]"), true,
			[](FCall& C, FCtx& X) { return FindAll(C, X, false, false); });
		GameTestApis::Add(TEXT("Object.Count"), Cat, TEXT("按定位器数有几个，不等待"),
			{ Req(TEXT("loc"), TEXT("loc")) }, TEXT("int"), true,
			[](FCall& C, FCtx& X) { return FindAll(C, X, true, false); });
		GameTestApis::Add(TEXT("Object.Exists"), Cat, TEXT("按定位器看有没有，不等待"),
			{ Req(TEXT("loc"), TEXT("loc")) }, TEXT("bool"), true,
			[](FCall& C, FCtx& X) { return FindAll(C, X, false, true); });

		GameTestApis::Add(TEXT("Object.IsValid"), Cat, TEXT("对象还在不在（销毁了、空的、取不到都是 false）"),
			{ Req(TEXT("obj"), TEXT("obj")) }, TEXT("bool"), true,
			[](FCall& C, FCtx& X)
			{
				FString Ignore;
				return C.Done(FGameTestValue::FromBool(GA::Object(C.Arg(0), X, Ignore) != nullptr));
			});
		GameTestApis::Add(TEXT("Object.Class"), Cat, TEXT("对象的类名（蓝图类带 _C）"),
			{ Req(TEXT("obj"), TEXT("obj")) }, TEXT("string"), true,
			[](FCall& C, FCtx& X)
			{
				UObject* Object = NeedObject(C, 0, X);
				return Object ? C.Done(FGameTestValue::FromString(Object->GetClass()->GetName())) : EState::Failed;
			});
		GameTestApis::Add(TEXT("Object.Name"), Cat, TEXT("对象的名字（编辑器里的 Actor 用大纲里的显示名）"),
			{ Req(TEXT("obj"), TEXT("obj")) }, TEXT("string"), true,
			[](FCall& C, FCtx& X)
			{
				UObject* Object = NeedObject(C, 0, X);
				return Object ? C.Done(FGameTestValue::FromString(GA::NameOf(Object))) : EState::Failed;
			});
		GameTestApis::Add(TEXT("Object.IsA"), Cat, TEXT("对象是不是这个类或它的子类"),
			{ Req(TEXT("obj"), TEXT("obj")), Req(TEXT("cls"), TEXT("cls")) }, TEXT("bool"), true,
			[](FCall& C, FCtx& X)
			{
				UObject* Object = NeedObject(C, 0, X);
				if (Object == nullptr)
				{
					return EState::Failed;
				}
				if (const UClass* Class = Cast<UClass>(C.Arg(1).GetObject()))
				{
					return C.Done(FGameTestValue::FromBool(Object->IsA(Class)));
				}
				return C.Done(FGameTestValue::FromBool(GameTestObjectRoots::ClassMatches(Object->GetClass(), GA::Text(C.Arg(1)))));
			});

		GameTestApis::Add(TEXT("Object.Get"), Cat, TEXT("读对象的属性或路径（反射；#字段 读绑的 Lua 实例表，也可以写完整的属性路径 @/子控件.X、@:状态）"),
			{ Req(TEXT("obj"), TEXT("obj")), Req(TEXT("path"), TEXT("string"), TEXT("Health、Stats.Level、Items[0].Id、#hp")) },
			TEXT("any"), true,
			[](FCall& C, FCtx& X)
			{
				UObject* Object = NeedObject(C, 0, X);
				if (Object == nullptr)
				{
					return EState::Failed;
				}
				const FGameTestValue Value = GameTestObjectLocate::ReadKey(Object, GA::Text(C.Arg(1)), X);
				return Value.IsUnresolved() ? C.Fail(Value.GetCode(), Value.GetString()) : C.Done(Value);
			});
		GameTestApis::Add(TEXT("Object.Set"), Cat,
			TEXT("改对象的属性（运行时走属性的 setter）；#字段 改 Lua 实例表。网络同步属性要对服务器世界的对象改"),
			{ Req(TEXT("obj"), TEXT("obj")), Req(TEXT("path"), TEXT("string")), Req(TEXT("value"), TEXT("any")) }, TEXT(""), false, &Set);
		GameTestApis::Add(TEXT("Object.Call"), Cat,
			TEXT("调对象上的函数，等于 invoke：args 给对象按参数名、给数组按顺序（Lua 函数）；条件里只能调只读函数"),
			{ Req(TEXT("obj"), TEXT("obj")), Req(TEXT("fn"), TEXT("string")), Opt(TEXT("args"), TEXT("any")) }, TEXT("any"), true, &CallFn);
	}
}
