// Copyright Epic Games, Inc. All Rights Reserved.

// 项目接口：_api.json 里的每一项登记成一个 IGameTestApi（Src = cpp / lua）。
// C++ 按 class + function 反射调（静态函数，目标是类默认对象），Lua 按 module + function 经 UELink 调。

#include "Api/GameTestApi.h"
#include "Api/GameTestApiFile.h"
#include "Api/GameTestProjectApis.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Observe/Lua/GameTestLuaBridge.h"
#include "Observe/Sources/GameTestReflectCall.h"
#include "Observe/Sources/GameTestReflectValue.h"

namespace
{
	/** 参数类型 → 面板输入控件用的类型（第 8 节）：认不出的给 any，面板给通用输入框。 */
	FString ArgType(const FJsonObject& InArg)
	{
		bool bObject = false;
		if (InArg.TryGetBoolField(TEXT("object"), bObject) && bObject)
		{
			return TEXT("obj");
		}
		const FString Type = InArg.GetStringField(TEXT("type"));
		if (Type == TEXT("bool") || Type == TEXT("boolean"))
		{
			return TEXT("bool");
		}
		if (Type.StartsWith(TEXT("int")) || Type.StartsWith(TEXT("uint")) || Type == TEXT("integer") || Type == TEXT("byte"))
		{
			return TEXT("int");
		}
		if (Type == TEXT("float") || Type == TEXT("double") || Type == TEXT("number"))
		{
			return TEXT("number");
		}
		if (Type == TEXT("FString") || Type == TEXT("FName") || Type == TEXT("FText") || Type == TEXT("string"))
		{
			return TEXT("string");
		}
		if (Type == TEXT("FVector"))
		{
			return TEXT("vector");
		}
		return TEXT("any");
	}

	class FProjectApi final : public IGameTestApi
	{
	public:
		explicit FProjectApi(const FGameTestProjectApi& InItem)
			: Item(InItem)
		{
			Desc.Name = Item.Name;
			Desc.Category = Item.Category.IsEmpty() ? FString(TEXT("项目")) : Item.Category;
			Desc.Summary = Item.Note;
			Desc.Display = Item.Display;
			Desc.Returns = Item.Returns;
			Desc.bReadOnly = Item.bReadOnly;
			Desc.Src = Item.Src;
			Desc.File = Item.File;
			Desc.Line = Item.Line;
			Desc.Module = TEXT("Project");
			for (const TSharedPtr<FJsonValue>& Value : Item.Args)
			{
				const TSharedPtr<FJsonObject> Arg = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Arg.IsValid())
				{
					continue;
				}
				// 输出参数调完进返回值、世界参数自动填：都不用脚本给
				bool bOut = false;
				FString Auto;
				if ((Arg->TryGetBoolField(TEXT("out"), bOut) && bOut) || Arg->TryGetStringField(TEXT("auto"), Auto))
				{
					continue;
				}
				FGameTestApiArg One;
				One.Name = Arg->GetStringField(TEXT("name"));
				One.Type = ArgType(*Arg);
				Arg->TryGetStringField(TEXT("note"), One.Note);
				bool bOptional = false;
				One.bOptional = Arg->TryGetBoolField(TEXT("optional"), bOptional) && bOptional;
				Desc.Args.Add(MoveTemp(One));
			}
		}

		virtual const FGameTestApiDesc& GetDesc() const override { return Desc; }

		virtual EGameTestCallState Call(FGameTestApiCall& InOutCall, FGameTestFlowContext& InContext) const override
		{
			const FGameTestValue Result = Item.Src == TEXT("lua") ? CallLua(InOutCall, InContext) : CallCpp(InOutCall, InContext);
			return Result.IsUnresolved() ? InOutCall.Fail(Result.GetCode(), Result.GetString()) : InOutCall.Done(Result);
		}

	private:
		FGameTestValue CallCpp(const FGameTestApiCall& InCall, FGameTestFlowContext& InContext) const
		{
			UObject* Target = nullptr;
			FString Error;
			UFunction* Function = GameTestReflectCall::FindStatic(Item.Class + TEXT(".") + Item.Function, Target, Error);
			if (Function == nullptr)
			{
				return FGameTestValue::Unresolved(FString::Printf(TEXT("%s：%s（代码改了？在编辑器里重新扫描项目接口）"), *Item.Name, *Error), TEXT("fn_missing"));
			}
			// 按名字传：Desc.Args 已经去掉了输出参数和世界参数，位置和函数参数对不上
			TMap<FString, FGameTestValue> Named;
			for (int32 Index = 0; Index < Desc.Args.Num(); ++Index)
			{
				if (!(Desc.Args[Index].bOptional && InCall.Arg(Index).IsNull()))
				{
					Named.Add(Desc.Args[Index].Name, InCall.Arg(Index));
				}
			}
			GameTestReflectCall::FArgs Args;
			Args.Named = &Named;
			return GameTestReflectCall::Call(Target, Function, Args, InContext);
		}

		FGameTestValue CallLua(const FGameTestApiCall& InCall, FGameTestFlowContext& InContext) const
		{
			if (InContext.IsReadOnly() ? !InContext.Options.bAllowLuaInCond : !InContext.Options.bAllowLuaInvoke)
			{
				return FGameTestValue::Unresolved(InContext.IsReadOnly()
						? TEXT("条件、表达式里调 Lua 函数没打开（_settings.json 的 allowLuaInCond）")
						: TEXT("步骤调 Lua 函数被关掉了（_settings.json 的 allowLuaInvoke）"),
					TEXT("lua_disabled"));
			}
			TArray<TSharedPtr<FJsonValue>> Args;
			for (const FGameTestValue& Arg : InCall.Args)
			{
				Args.Add(Arg.ToJson());
			}
			// 末尾没给的可选参数不传（Lua 里就是 nil），免得 null 占位
			while (Args.Num() > 0 && (!Args.Last().IsValid() || Args.Last()->IsNull()))
			{
				Args.Pop();
			}
			TArray<FGameTestLuaValue> Results;
			FString Error;
			const TArray<TSharedPtr<FJsonValue>> Keys = { MakeShared<FJsonValueString>(Item.Function) };
			if (!GameTestLuaBridge::Call(InContext.World, nullptr, Item.Module, Keys, Args, Results, Error))
			{
				return FGameTestValue::Unresolved(FString::Printf(TEXT("%s：%s"), *Item.Name, *Error), TEXT("fn_missing"));
			}
			if (Results.Num() <= 1)
			{
				return Results.Num() == 1 ? Results[0].ToValue() : FGameTestValue::Null();
			}
			TArray<FGameTestValue> Items;
			for (const FGameTestLuaValue& Result : Results)
			{
				Items.Add(Result.ToValue());
			}
			return FGameTestValue::FromArray(MoveTemp(Items));
		}

		FGameTestProjectApi Item;
		FGameTestApiDesc Desc;
	};

	FDateTime& LastStamp()
	{
		static FDateTime Stamp = FDateTime::MinValue();
		return Stamp;
	}

	bool& Loaded()
	{
		static bool bLoaded = false;
		return bLoaded;
	}
}

FString GameTestProjectApis::Reload()
{
	const FString Path = GameTestApiFile::GetPath();
	LastStamp() = IFileManager::Get().GetTimeStamp(*Path);
	Loaded() = true;

	GameTestApis::RemoveWhere([](const FGameTestApiDesc& InDesc) { return InDesc.Src != TEXT("builtin") && InDesc.Module == TEXT("Project"); });
	GameTestApis::GetRenamed().Reset();

	TArray<FGameTestProjectApi> Items;
	TMap<FString, FString> Renamed;
	FString Error;
	if (!GameTestApiFile::Load(Items, Renamed, Error))
	{
		return Error;
	}
	for (const FGameTestProjectApi& Item : Items)
	{
		// 项目接口不能顶掉自带接口：名字都以「项目.」开头，正常撞不上；撞上了跳过，自带的行为不能被一个文件改掉
		const IGameTestApi* Existing = GameTestApis::Get().Find(FName(*Item.Name));
		if (Existing != nullptr && Existing->GetDesc().Src == TEXT("builtin"))
		{
			continue;
		}
		GameTestApis::Get().Register(MakeShared<FProjectApi>(Item));
	}
	GameTestApis::GetRenamed() = MoveTemp(Renamed);
	return FString();
}

FString GameTestProjectApis::Refresh()
{
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*GameTestApiFile::GetPath());
	if (Loaded() && Stamp == LastStamp())
	{
		return FString();
	}
	return Reload();
}

namespace GameTestBuiltin
{
	void RegisterApiProject()
	{
		// 注册表初始化时读一次；之后 C++ 段由 GameTestEditor 写完通知，Lua 段（面板写）由 Refresh 按时间戳发现
		GameTestProjectApis::Reload();
		GameTestApiFile::OnChanged().AddLambda([]() { GameTestProjectApis::Reload(); });
	}
}
