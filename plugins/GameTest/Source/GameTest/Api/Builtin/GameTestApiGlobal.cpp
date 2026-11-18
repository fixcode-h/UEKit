// Copyright Epic Games, Inc. All Rights Reserved.

// 自带接口：子系统、控制台变量与命令、配置表、ini、资产、存档（协议第 8.1 节）。

#include "Api/Builtin/GameTestApiArgs.h"
#include "Api/GameTestApi.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "GameFramework/CheatManager.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/PackageName.h"
#include "Observe/Sources/GameTestObjectRoots.h"
#include "Observe/Sources/GameTestPathText.h"
#include "Observe/Sources/GameTestReflectValue.h"
#if WITH_EDITOR
#include "Editor.h"
#endif

namespace GA = GameTestApiArgs;
using GameTestApiArgs::Opt;
using GameTestApiArgs::Req;

namespace
{
	using FCall = FGameTestApiCall;
	using FCtx = FGameTestFlowContext;
	using EState = EGameTestCallState;

	/** `/Game/Data/DT_Item` 补成 `/Game/Data/DT_Item.DT_Item`：脚本里常只写包路径。 */
	FString ObjectPathOf(const FString& InPath)
	{
		if (InPath.Contains(TEXT(".")) || !InPath.StartsWith(TEXT("/")))
		{
			return InPath;
		}
		return InPath + TEXT(".") + FPackageName::GetShortName(InPath);
	}

	UDataTable* NeedTable(FCall& C, FCtx& X)
	{
		UDataTable* Table = Cast<UDataTable>(C.Arg(0).GetObject());
		if (Table == nullptr)
		{
			const FString Path = ObjectPathOf(GA::Text(C.Arg(0)));
			// 配置表是测试数据和期望值的来源：没加载就加载（读表不改游戏状态）
			Table = LoadObject<UDataTable>(nullptr, *Path, nullptr, LOAD_NoWarn);
			if (Table == nullptr)
			{
				GA::ArgError(C, 0, TEXT("table"), FString::Printf(TEXT("找不到配置表 %s（写 /Game/… 路径）"), *Path));
			}
		}
		return Table;
	}

	FGameTestValue RowValue(const UDataTable& InTable, const uint8* InRow)
	{
		TMap<FString, FGameTestValue> Fields;
		if (const UScriptStruct* Struct = InTable.GetRowStruct())
		{
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				Fields.Add(It->GetName(), GameTestReflectValue::FromProperty(*It, It->ContainerPtrToValuePtr<void>(InRow)));
			}
		}
		return FGameTestValue::FromMap(MoveTemp(Fields));
	}

	EState Row(FCall& C, FCtx& X, bool bOneField)
	{
		const UDataTable* Table = NeedTable(C, X);
		if (Table == nullptr)
		{
			return EState::Failed;
		}
		const FString RowName = GA::Text(C.Arg(1));
		const uint8* RowData = Table->FindRowUnchecked(FName(*RowName));
		if (RowData == nullptr)
		{
			return C.Fail(TEXT("path_missing"), FString::Printf(TEXT("%s 里没有行 %s"), *Table->GetName(), *RowName));
		}
		if (!bOneField)
		{
			return C.Done(RowValue(*Table, RowData));
		}
		const FString Field = GA::Text(C.Arg(2));
		const FProperty* Prop = GameTestReflectValue::FindProperty(Table->GetRowStruct(), Field);
		if (Prop == nullptr)
		{
			return C.Fail(TEXT("path_missing"), FString::Printf(TEXT("%s 的行没有字段 %s"), *Table->GetName(), *Field));
		}
		return C.Done(GameTestReflectValue::FromProperty(Prop, Prop->ContainerPtrToValuePtr<void>(RowData)));
	}

	/** ini 文件名：Game / Engine / Input / GameUserSettings，或完整路径。 */
	FString IniOf(const FString& InFile)
	{
		const FString File = InFile.IsEmpty() ? FString(TEXT("Game")) : InFile;
		if (File.Equals(TEXT("Game"), ESearchCase::IgnoreCase)) { return GGameIni; }
		if (File.Equals(TEXT("Engine"), ESearchCase::IgnoreCase)) { return GEngineIni; }
		if (File.Equals(TEXT("Input"), ESearchCase::IgnoreCase)) { return GInputIni; }
		if (File.Equals(TEXT("GameUserSettings"), ESearchCase::IgnoreCase)) { return GGameUserSettingsIni; }
		return File;
	}

	EState ConsoleRun(FCall& C, FCtx& X)
	{
		const FString Command = GA::Text(C.Arg(0));
		if (Command.IsEmpty())
		{
			GA::ArgError(C, 0, TEXT("cmd"), TEXT("要写控制台指令"));
			return EState::Failed;
		}
		FString Error;
		UWorld* World = GA::ClientWorld(C.Arg(1), X, Error);
		if (World == nullptr)
		{
			return C.Fail(TEXT("engine_unavailable"), Error);
		}
		// 和 console 动作同一条路：编辑器里交给 GEditor（PIE 的命令也要它认），游戏里交给 GEngine
		FStringOutputDevice Output;
#if WITH_EDITOR
		if (GEditor != nullptr)
		{
			GEditor->Exec(World, *Command, Output);
		}
		else
#endif
		if (GEngine != nullptr)
		{
			GEngine->Exec(World, *Command, Output);
		}
		return C.Done(FGameTestValue::FromString(FString(*Output).Left(4000)));
	}

	EState CheatRun(FCall& C, FCtx& X)
	{
		const FString Command = GA::Text(C.Arg(0));
		FString Error;
		APlayerController* Controller = X.World ? Cast<APlayerController>(GameTestObjectRoots::Player(*X.World, TEXT("pc"), Error)) : nullptr;
		if (Controller == nullptr)
		{
			return C.Fail(TEXT("engine_unavailable"), Error.IsEmpty() ? FString(TEXT("游戏没在跑")) : Error);
		}
		if (Controller->CheatManager == nullptr)
		{
			return C.Fail(TEXT("fn_not_callable"), TEXT("CheatManager 没开（Shipping 包、或 GameMode 不允许作弊）；控制台指令用 Console.Run"));
		}
		// ConsoleCommand 先找 PlayerController / CheatManager 上的 Exec 函数，和在控制台里敲一样
		return C.Done(FGameTestValue::FromString(Controller->ConsoleCommand(Command, true).Left(4000)));
	}
}

namespace GameTestBuiltin
{
	void RegisterApiGlobal()
	{
		GameTestApis::Add(TEXT("Subsystem.Get"), TEXT("子系统与全局"), TEXT("子系统对象（GameInstance / World / 本地玩家 / 引擎子系统都找）"),
			{ Req(TEXT("cls"), TEXT("cls"), TEXT("UQuestSubsystem，前缀可省")) }, TEXT("obj"), true,
			[](FCall& C, FCtx& X)
			{
				if (GA::World(X, C) == nullptr)
				{
					return EState::Failed;
				}
				FString Error;
				UObject* Found = GameTestObjectRoots::Subsystem(*X.World, GA::Text(C.Arg(0)), Error);
				return Found ? C.Done(FGameTestValue::FromObject(Found)) : C.Fail(TEXT("object_not_found"), Error);
			});
		GameTestApis::Add(TEXT("CVar.Get"), TEXT("子系统与全局"), TEXT("控制台变量的值（数字、布尔按类型给）"),
			{ Req(TEXT("name"), TEXT("string")) }, TEXT("any"), true,
			[](FCall& C, FCtx& X)
			{
				// 和 @cvar.名字 同一份解析
				const FGameTestValue Value = GameTestPathText::ResolveSourceText(TEXT("@cvar.") + GA::Text(C.Arg(0)), X);
				return Value.IsUnresolved() ? C.Fail(Value.GetCode(), Value.GetString()) : C.Done(Value);
			});
		GameTestApis::Add(TEXT("CVar.Set"), TEXT("子系统与全局"), TEXT("改控制台变量（按控制台的优先级写）"),
			{ Req(TEXT("name"), TEXT("string")), Req(TEXT("value"), TEXT("any")) }, TEXT(""), false,
			[](FCall& C, FCtx&)
			{
				const FString Name = GA::Text(C.Arg(0));
				IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(*Name);
				if (Var == nullptr)
				{
					return C.Fail(TEXT("path_missing"), FString::Printf(TEXT("没有控制台变量 %s"), *Name));
				}
				const FGameTestValue& Value = C.Arg(1);
				const FString Text = Value.Type == EGameTestValueType::Bool ? FString(Value.GetBool() ? TEXT("1") : TEXT("0")) : GA::Text(Value);
				Var->Set(*Text, ECVF_SetByConsole);
				return C.Done(FGameTestValue::Null());
			});
		GameTestApis::Add(TEXT("Console.Run"), TEXT("子系统与全局"), TEXT("执行控制台指令，返回输出（同 console 动作）"),
			{ Req(TEXT("cmd"), TEXT("string")), Opt(TEXT("client"), TEXT("int")) }, TEXT("string"), false, &ConsoleRun);
		GameTestApis::Add(TEXT("Cheat.Run"), TEXT("子系统与全局"), TEXT("走 CheatManager 执行作弊指令（Exec 函数），返回输出"),
			{ Req(TEXT("cmd"), TEXT("string")) }, TEXT("string"), false, &CheatRun);

		const TCHAR* DataCat = TEXT("配置与数据");
		GameTestApis::Add(TEXT("DataTable.Row"), DataCat, TEXT("配置表的一行（字段 → 值）；测试数据和期望值从表里取"),
			{ Req(TEXT("table"), TEXT("string"), TEXT("/Game/Data/DT_Item")), Req(TEXT("row"), TEXT("string")) }, TEXT("{字段: 值}"), true,
			[](FCall& C, FCtx& X) { return Row(C, X, false); });
		GameTestApis::Add(TEXT("DataTable.Field"), DataCat, TEXT("配置表一行里的一个字段"),
			{ Req(TEXT("table"), TEXT("string")), Req(TEXT("row"), TEXT("string")), Req(TEXT("field"), TEXT("string")) }, TEXT("any"), true,
			[](FCall& C, FCtx& X) { return Row(C, X, true); });
		GameTestApis::Add(TEXT("DataTable.Rows"), DataCat, TEXT("配置表的全部行名"),
			{ Req(TEXT("table"), TEXT("string")) }, TEXT("[string]"), true,
			[](FCall& C, FCtx& X)
			{
				const UDataTable* Table = NeedTable(C, X);
				if (Table == nullptr)
				{
					return EState::Failed;
				}
				TArray<FGameTestValue> Names;
				for (const FName& Name : Table->GetRowNames())
				{
					Names.Add(FGameTestValue::FromString(Name.ToString()));
				}
				return C.Done(FGameTestValue::FromArray(MoveTemp(Names)));
			});
		GameTestApis::Add(TEXT("Config.Get"), DataCat, TEXT("ini 里的值（file：Game（默认）/ Engine / Input / GameUserSettings）"),
			{ Req(TEXT("section"), TEXT("string")), Req(TEXT("key"), TEXT("string")), Opt(TEXT("file"), TEXT("string")) }, TEXT("string"), true,
			[](FCall& C, FCtx&)
			{
				FString Value;
				const FString Section = GA::Text(C.Arg(0));
				const FString Key = GA::Text(C.Arg(1));
				if (GConfig == nullptr || !GConfig->GetString(*Section, *Key, Value, IniOf(GA::Text(C.Arg(2)))))
				{
					return C.Fail(TEXT("path_missing"), FString::Printf(TEXT("ini 里没有 [%s] %s"), *Section, *Key));
				}
				return C.Done(FGameTestValue::FromString(Value));
			});

		const TCHAR* AssetCat = TEXT("资产");
		GameTestApis::Add(TEXT("Asset.Exists"), AssetCat, TEXT("资产包在不在（不加载）"), { Req(TEXT("path"), TEXT("string")) }, TEXT("bool"), true,
			[](FCall& C, FCtx&)
			{
				FString Package = GA::Text(C.Arg(0));
				Package.Split(TEXT("."), &Package, nullptr);
				return C.Done(FGameTestValue::FromBool(FPackageName::IsValidLongPackageName(Package) && FPackageName::DoesPackageExist(Package)));
			});
		GameTestApis::Add(TEXT("Asset.Load"), AssetCat, TEXT("加载资产并返回（已加载的直接给）"), { Req(TEXT("path"), TEXT("string")) }, TEXT("obj"), true,
			[](FCall& C, FCtx&)
			{
				const FString Path = ObjectPathOf(GA::Text(C.Arg(0)));
				UObject* Loaded = LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn);
				return Loaded ? C.Done(FGameTestValue::FromObject(Loaded))
					: C.Fail(TEXT("object_not_found"), FString::Printf(TEXT("加载不了 %s（路径写错，或没打进包）"), *Path));
			});

		const TCHAR* SaveCat = TEXT("存档");
		GameTestApis::Add(TEXT("SaveGame.Exists"), SaveCat, TEXT("存档槽有没有存档"),
			{ Req(TEXT("slot"), TEXT("string")), Opt(TEXT("user"), TEXT("int"), TEXT("默认 0")) }, TEXT("bool"), true,
			[](FCall& C, FCtx&)
			{
				double User = 0.0;
				GA::Number(C.Arg(1), 0.0, User);
				return C.Done(FGameTestValue::FromBool(UGameplayStatics::DoesSaveGameExist(GA::Text(C.Arg(0)), static_cast<int32>(User))));
			});
		GameTestApis::Add(TEXT("SaveGame.Delete"), SaveCat, TEXT("删掉存档槽（重置状态用）；本来就没有不算失败"),
			{ Req(TEXT("slot"), TEXT("string")), Opt(TEXT("user"), TEXT("int")) }, TEXT(""), false,
			[](FCall& C, FCtx&)
			{
				double User = 0.0;
				GA::Number(C.Arg(1), 0.0, User);
				const FString Slot = GA::Text(C.Arg(0));
				if (UGameplayStatics::DoesSaveGameExist(Slot, static_cast<int32>(User)) && !UGameplayStatics::DeleteGameInSlot(Slot, static_cast<int32>(User)))
				{
					return C.Fail(TEXT("unresolved"), FString::Printf(TEXT("删不掉存档 %s（文件被占用？）"), *Slot));
				}
				return C.Done(FGameTestValue::Null());
			});
	}
}
