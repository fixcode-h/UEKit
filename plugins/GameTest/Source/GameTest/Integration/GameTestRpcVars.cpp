// Copyright Epic Games, Inc. All Rights Reserved.

// gt.vars：某个对象（界面、类、子系统、玩家、定位到的对象、Lua 模块）能读写什么——属性和函数，
// 按 C++ → 蓝图 → Lua 分组，C++ / 蓝图再按声明的类分小节（离它最近的在前）。面板的「变量」选择器和 Agent 写条件时用。
//
// 范围（scope）按表分派：加一种范围就是在 Scopes() 里加一行，不改列表逻辑。

#include "Api/GameTestParamKind.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestWorld.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Integration/GameTestRpcScript.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Observe/GameTestWidgetNode.h"
#include "Observe/Lua/GameTestLuaBridge.h"
#include "Observe/Sources/GameTestObjectLocator.h"
#include "Observe/Ref/GameTestRef.h"
#include "Observe/Ref/GameTestRefResolve.h"
#include "Observe/Sources/GameTestObjectRoots.h"
#include "Observe/Sources/GameTestReflectCall.h"
#include "Observe/Sources/GameTestReflectValue.h"
#include "Engine/World.h"

namespace
{
	constexpr int32 VarsDefaultLimit = 60;
	constexpr int32 VarsMaxLimit = 200;
	constexpr int32 VarsMaxChars = 12000;

	/** 要列的对象：类一定有，实例只有游戏开着（或范围本身是实例）时有。 */
	struct FOwner
	{
		UClass* Class = nullptr;
		TWeakObjectPtr<UObject> Instance;
		/** 条目 path 的前缀（`@pc`、`@sub.UQuestSubsystem`）；界面是 `@`（属性路径 v5，拼成 `@.成员`）。为空时 path 就是成员名。 */
		FString Ref;
		/** scope=lua 时的模块。 */
		FString LuaModule;
	};

	/** 一条成员，先收齐再分页（分页跨组）。 */
	struct FEntry
	{
		int32 Group = 0;
		TSharedPtr<FJsonObject> Json;
	};

	struct FVarGroup
	{
		TSharedPtr<FJsonObject> Head;
	};

	FString StrOr(const FJsonObject& InArgs, const TCHAR* InField, const FString& InDefault)
	{
		FString Value;
		return GameTestJson::TryGetString(InArgs, InField, Value) && !Value.IsEmpty() ? Value : InDefault;
	}

	using FScopeFn = bool (*)(const FJsonObject&, FGameTestFlowContext&, FOwner&, FString&);

	bool ScopeWidget(const FJsonObject& InArgs, FGameTestFlowContext& InContext, FOwner& OutOwner, FString& OutError)
	{
		FString Target;
		if (InArgs.TryGetStringField(TEXT("target"), Target) && !Target.IsEmpty())
		{
			const FGameTestWidgetLook Look = GameTestWidgetProbe::Look(Target, InContext);
			UWidget* Widget = Look.bResolved && Look.Node.IsValid() ? Look.Node->Widget.Get() : nullptr;
			if (Widget == nullptr)
			{
				OutError = Look.Error.IsEmpty() ? FString::Printf(TEXT("控件 %s 不在屏上"), *Target) : Look.Error;
				return false;
			}
			// 选到的是界面里的控件时列它自己（按钮的属性）；要看整个界面的变量就选屏（WBP_X:.）
			OutOwner.Instance = Widget;
			OutOwner.Class = Widget->GetClass();
			// 属性路径 v5：对象自己的属性写 @.X
			OutOwner.Ref = TEXT("@");
			return true;
		}
		FString ClassName;
		InArgs.TryGetStringField(TEXT("class"), ClassName);
		OutOwner.Class = GameTestObjectRoots::FindClass(ClassName);
		OutOwner.Ref = TEXT("@");
		if (OutOwner.Class == nullptr)
		{
			OutError = FString::Printf(TEXT("找不到界面类 %s（写 target 选屏上的界面，或 class 写 WBP_X_C / 路径）"), *ClassName);
		}
		return OutOwner.Class != nullptr;
	}

	bool ScopeClass(const FJsonObject& InArgs, FGameTestFlowContext&, FOwner& OutOwner, FString& OutError)
	{
		const FString Name = StrOr(InArgs, TEXT("name"), FString());
		OutOwner.Class = GameTestObjectRoots::FindClass(Name);
		if (OutOwner.Class == nullptr)
		{
			OutError = FString::Printf(TEXT("找不到类 %s（蓝图类要加载过；写 BP_X_C 或 /Game/…/BP_X.BP_X_C）"), *Name);
		}
		return OutOwner.Class != nullptr;
	}

	bool ScopeSubsystem(const FJsonObject& InArgs, FGameTestFlowContext& InContext, FOwner& OutOwner, FString& OutError)
	{
		const FString Name = StrOr(InArgs, TEXT("name"), FString());
		if (InContext.World == nullptr)
		{
			// 没开游戏也能看类：子系统的属性和函数不靠实例
			OutOwner.Class = GameTestObjectRoots::FindClass(Name);
		}
		else if (UObject* Found = GameTestObjectRoots::Subsystem(*InContext.World, Name, OutError))
		{
			OutOwner.Instance = Found;
			OutOwner.Class = Found->GetClass();
		}
		OutOwner.Ref = TEXT("@sub.") + Name;
		if (OutOwner.Class == nullptr && OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("找不到子系统 %s"), *Name);
		}
		return OutOwner.Class != nullptr;
	}

	bool ScopePlayer(const FJsonObject& InArgs, FGameTestFlowContext& InContext, FOwner& OutOwner, FString& OutError)
	{
		const FString Which = StrOr(InArgs, TEXT("which"), TEXT("pc"));
		if (InContext.World == nullptr)
		{
			OutError = TEXT("游戏没在跑：玩家对象要开着游戏才看得到");
			return false;
		}
		UObject* Found = GameTestObjectRoots::Player(*InContext.World, Which, OutError);
		if (Found == nullptr)
		{
			return false;
		}
		OutOwner.Instance = Found;
		OutOwner.Class = Found->GetClass();
		OutOwner.Ref = TEXT("@") + Which.ToLower();
		return true;
	}

	/** scope=object：参数是对象引用 `ref`（2.0.0 起，docs/game-test-ref.md 7.6；内联引用对象，或类名字符串）。 */
	bool ScopeObject(const FJsonObject& InArgs, FGameTestFlowContext& InContext, FOwner& OutOwner, FString& OutError)
	{
		const TSharedPtr<FJsonValue> RefJson = InArgs.TryGetField(TEXT("ref"));
		FGameTestRef Ref;
		const TSharedPtr<FJsonObject>* RefObject = nullptr;
		FString ClassText;
		if (RefJson.IsValid() && RefJson->TryGetObject(RefObject) && RefObject != nullptr && RefObject->IsValid())
		{
			FGameTestObjectLocator Locator;
			TArray<FString> Errors;
			if (!FGameTestObjectLocator::Parse(**RefObject, Locator, Errors))
			{
				OutError = FString::Join(Errors, TEXT("；"));
				return false;
			}
			Ref = Locator.Ref;
		}
		else if (RefJson.IsValid() && RefJson->TryGetString(ClassText) && !ClassText.IsEmpty())
		{
			Ref.Class = ClassText;
		}
		else
		{
			OutError = InArgs.HasField(TEXT("locator")) ? TEXT("scope=object 的参数 2.0.0 起改成 ref（对象引用 {class, in?, ident?}），locator 不认了")
				: TEXT("scope=object 要写 ref（对象引用 {class, in?, ident?}）");
			return false;
		}
		const FGameTestRefResult Result = InContext.World != nullptr ? GameTestRefResolve::ResolveRef(Ref, InContext) : FGameTestRefResult();
		if (Result.IsOk())
		{
			OutOwner.Instance = Result.Get();
			OutOwner.Class = Result.Get()->GetClass();
			return true;
		}
		// 没开游戏、或现在找不到：退回按类列（面板写引用时先看类有什么）
		OutOwner.Class = GameTestObjectRoots::FindClass(Ref.Class);
		if (OutOwner.Class == nullptr)
		{
			OutError = Result.Error.IsEmpty() ? FString::Printf(TEXT("找不到类 %s"), *Ref.Class) : Result.Error;
		}
		return OutOwner.Class != nullptr;
	}

	bool ScopeLua(const FJsonObject& InArgs, FGameTestFlowContext&, FOwner& OutOwner, FString& OutError)
	{
		OutOwner.LuaModule = StrOr(InArgs, TEXT("module"), FString());
		OutOwner.Ref = TEXT("@luaMod.") + OutOwner.LuaModule;
		if (OutOwner.LuaModule.IsEmpty())
		{
			OutError = TEXT("scope=lua 要写 module（require 路径）");
			return false;
		}
		return true;
	}

	const TMap<FString, FScopeFn>& Scopes()
	{
		static const TMap<FString, FScopeFn> Table = {
			{ TEXT("widget"), &ScopeWidget },
			{ TEXT("class"), &ScopeClass },
			{ TEXT("subsystem"), &ScopeSubsystem },
			{ TEXT("player"), &ScopePlayer },
			{ TEXT("object"), &ScopeObject },
			{ TEXT("lua"), &ScopeLua },
		};
		return Table;
	}

	/** 引擎自带的基类：成员多又很少用到，面板默认折起来。 */
	bool IsEngineClass(const UClass* InClass)
	{
		static const TSet<FString> Packages = { TEXT("/Script/CoreUObject"), TEXT("/Script/Engine"), TEXT("/Script/UMG"), TEXT("/Script/Slate"),
			TEXT("/Script/SlateCore"), TEXT("/Script/AIModule"), TEXT("/Script/GameplayAbilities"), TEXT("/Script/CommonUI"),
			TEXT("/Script/EnhancedInput"), TEXT("/Script/NavigationSystem"), TEXT("/Script/PhysicsCore") };
		return InClass != nullptr && Packages.Contains(InClass->GetOutermost()->GetName());
	}

	/** 像动画、计时的成员（Anim / Time / Tick / Hover）：写条件很少用，面板画淡一点。 */
	bool IsDim(const FString& InName)
	{
		static const TCHAR* Words[] = { TEXT("Anim"), TEXT("Time"), TEXT("Tick"), TEXT("Hover"), TEXT("Timer"), TEXT("Tween") };
		for (const TCHAR* Word : Words)
		{
			if (InName.Contains(Word))
			{
				return true;
			}
		}
		return false;
	}

	/** 值给 JSON：标量原样，大的给一句显示文字（列表是给人挑变量的，不是导数据）。 */
	TSharedPtr<FJsonValue> ShortValue(const FGameTestValue& InValue)
	{
		switch (InValue.Type)
		{
		case EGameTestValueType::Bool:
		case EGameTestValueType::Number:
		case EGameTestValueType::Null:
			return InValue.ToJson();
		default:
			return MakeShared<FJsonValueString>(InValue.ToDisplay(120));
		}
	}

	FString JoinPath(const FString& InRef, const FString& InName)
	{
		return InRef.IsEmpty() ? InName : InRef + TEXT(".") + InName;
	}

	struct FCollect
	{
		const FOwner& Owner;
		bool bValues = false;
		bool bFunctions = true;
		FString Filter;
		TArray<FVarGroup> Groups;
		TArray<FEntry> Entries;

		bool Wanted(const FString& InName) const
		{
			return Filter.IsEmpty() || InName.Contains(Filter);
		}

		int32 AddGroup(const TCHAR* InSrc, const FString& InTitle, const UClass* InClass)
		{
			FVarGroup Group;
			Group.Head = MakeShared<FJsonObject>();
			Group.Head->SetStringField(TEXT("src"), InSrc);
			Group.Head->SetStringField(TEXT("title"), InTitle);
			if (InClass != nullptr)
			{
				Group.Head->SetStringField(TEXT("class"), InClass->GetPathName());
				if (IsEngineClass(InClass))
				{
					Group.Head->SetBoolField(TEXT("collapsed"), true);
				}
				if (Cast<UBlueprintGeneratedClass>(InClass) != nullptr)
				{
					FString Asset = InClass->GetPathName();
					Asset.RemoveFromEnd(TEXT("_C"));
					Group.Head->SetStringField(TEXT("asset"), Asset);
				}
			}
			return Groups.Add(Group);
		}

		void AddProperty(int32 InGroup, const FProperty& InProp)
		{
			const FString Name = InProp.GetName();
			if (!Wanted(Name))
			{
				return;
			}
			const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Name);
			Item->SetStringField(TEXT("path"), JoinPath(Owner.Ref, Name));
			Item->SetStringField(TEXT("type"), InProp.GetCPPType());
			Item->SetStringField(TEXT("kind"), TEXT("prop"));
			const bool bWritable = InProp.HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_Edit) && !InProp.HasAnyPropertyFlags(CPF_BlueprintReadOnly);
			Item->SetBoolField(TEXT("readOnly"), !bWritable);
#if WITH_EDITOR
			const FString Tip = InProp.GetMetaData(TEXT("ToolTip"));
			if (!Tip.IsEmpty())
			{
				Item->SetStringField(TEXT("note"), Tip.Left(200));
			}
#endif
			if (IsDim(Name))
			{
				Item->SetBoolField(TEXT("dim"), true);
			}
			UObject* Instance = Owner.Instance.Get();
			if (bValues && Instance != nullptr && Instance->IsA(InProp.GetOwnerClass()))
			{
				Item->SetField(TEXT("value"), ShortValue(GameTestReflectValue::FromProperty(&InProp, InProp.ContainerPtrToValuePtr<void>(Instance))));
			}
			Entries.Add({ InGroup, Item });
		}

		void AddFunction(int32 InGroup, const UFunction& InFunction)
		{
			const FString Name = InFunction.GetName();
			// 蓝图编译出来的内部函数（事件图、绑定），脚本里用不上
			if (!Wanted(Name) || Name.StartsWith(TEXT("ExecuteUbergraph")) || Name.StartsWith(TEXT("BndEvt__")) || Name.StartsWith(TEXT("__")))
			{
				return;
			}
			if (!InFunction.HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_Exec | FUNC_BlueprintEvent))
			{
				return;
			}
			const TArray<FGameTestParamInfo> Params = GameTestParamKind::Describe(InFunction);
			TArray<FString> ArgText;
			for (const FGameTestParamInfo& Param : Params)
			{
				if (!Param.bOut)
				{
					ArgText.Add(FString::Printf(TEXT("%s: %s"), *Param.Name, *Param.Type));
				}
			}
			const FString Returns = GameTestParamKind::ReturnType(InFunction);
			const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Name);
			Item->SetStringField(TEXT("path"), JoinPath(Owner.Ref, Name + TEXT("()")));
			Item->SetStringField(TEXT("type"), Returns);
			Item->SetStringField(TEXT("kind"), TEXT("fn"));
			Item->SetBoolField(TEXT("readOnly"), GameTestReflectValue::IsReadOnlyFunction(InFunction));
			const FString Tail = Returns.IsEmpty() ? FString() : TEXT(" → ") + Returns;
			Item->SetStringField(TEXT("sig"), FString::Printf(TEXT("%s(%s)%s"), *Name, *FString::Join(ArgText, TEXT(", ")), *Tail));
			Item->SetArrayField(TEXT("args"), GameTestParamKind::ToJson(Params));
			FString Why;
			const bool bCallable = GameTestParamKind::IsCallable(InFunction, Why);
			Item->SetBoolField(TEXT("callable"), bCallable);
			if (!bCallable)
			{
				Item->SetStringField(TEXT("why"), Why);
			}
			if (IsDim(Name))
			{
				Item->SetBoolField(TEXT("dim"), true);
			}
			Entries.Add({ InGroup, Item });
		}

		/** 沿继承链往上：每个类一组（只列这个类自己声明的）。C++ 组在前、蓝图组在后，各自离它最近的在前。 */
		void CollectReflected()
		{
			if (Owner.Class == nullptr)
			{
				return;
			}
			TArray<UClass*> Native, Blueprint;
			for (UClass* Class = Owner.Class; Class != nullptr; Class = Class->GetSuperClass())
			{
				(Cast<UBlueprintGeneratedClass>(Class) ? Blueprint : Native).Add(Class);
			}
			auto CollectClass = [this](UClass* InClass, const TCHAR* InSrc)
			{
				const int32 Group = AddGroup(InSrc, InClass->GetName(), InClass);
				for (TFieldIterator<FProperty> It(InClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
				{
					AddProperty(Group, **It);
				}
				if (bFunctions)
				{
					for (TFieldIterator<UFunction> It(InClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
					{
						AddFunction(Group, **It);
					}
				}
			};
			for (UClass* Class : Native)
			{
				CollectClass(Class, TEXT("cpp"));
			}
			for (UClass* Class : Blueprint)
			{
				CollectClass(Class, TEXT("bp"));
			}
		}

		/** Lua 组：游戏开着时由实例表（或模块表）给出；文件扫描由面板做、按同样结构合并。 */
		void CollectLua(UWorld* InWorld)
		{
			if (!GameTestLuaBridge::IsAvailable() || (Owner.LuaModule.IsEmpty() && !Owner.Instance.IsValid()))
			{
				return;
			}
			FGameTestLuaValue Table;
			FString Error;
			bool bOk = false;
			FString Prefix;
			if (!Owner.LuaModule.IsEmpty())
			{
				const TArray<TSharedPtr<FJsonValue>> Keys = { MakeShared<FJsonValueString>(TEXT("package")), MakeShared<FJsonValueString>(TEXT("loaded")),
					MakeShared<FJsonValueString>(Owner.LuaModule) };
				bOk = GameTestLuaBridge::Read(InWorld, nullptr, Keys, Table, Error, bFunctions);
				Prefix = Owner.Ref;
			}
			else
			{
				bOk = GameTestLuaBridge::Read(InWorld, Owner.Instance.Get(), {}, Table, Error, bFunctions);
				// 绑在对象上的实例表：属性路径 v5 写 @#字段（JoinPath 之后再把 `@#.` 收成 `@#`）
				Prefix = TEXT("@#");
			}
			if (!bOk || Table.Type() != TEXT("table"))
			{
				return;
			}
			const int32 Group = AddGroup(TEXT("lua"), Owner.LuaModule.IsEmpty() ? FString(TEXT("Lua 实例表")) : Owner.LuaModule, nullptr);
			for (const TSharedPtr<FJsonValue>& Value : Table.Fields)
			{
				const TSharedPtr<FJsonObject> Field = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Key;
				if (!Field.IsValid() || !Field->TryGetStringField(TEXT("k"), Key) || !Wanted(Key))
				{
					continue;
				}
				FString Type;
				Field->TryGetStringField(TEXT("t"), Type);
				const bool bFunction = Type == TEXT("function");
				const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Key);
				const FString Joined = JoinPath(Prefix, bFunction ? Key + TEXT("()") : Key);
				Item->SetStringField(TEXT("path"), Prefix == TEXT("@#") ? TEXT("@#") + Key : Joined);
				Item->SetStringField(TEXT("type"), Type);
				Item->SetStringField(TEXT("kind"), bFunction ? TEXT("fn") : TEXT("prop"));
				Item->SetBoolField(TEXT("readOnly"), false);
				if (IsDim(Key))
				{
					Item->SetBoolField(TEXT("dim"), true);
				}
				if (bValues && !bFunction)
				{
					FGameTestLuaValue One;
					One.Desc = Field;
					Item->SetField(TEXT("value"), One.ToPlain());
				}
				Entries.Add({ Group, Item });
			}
		}
	};

	/** UnLua 绑的模块名：对象上有 GetModuleName（UnLuaInterface）就调一次，没有就算了。 */
	FString VarsLuaModuleOf(UObject* InObject, FGameTestFlowContext& InContext)
	{
		UFunction* Function = InObject ? InObject->FindFunction(TEXT("GetModuleName")) : nullptr;
		if (Function == nullptr)
		{
			return FString();
		}
		const TMap<FString, FGameTestValue> None;
		GameTestReflectCall::FArgs Args;
		Args.Named = &None;
		const FGameTestValue Result = GameTestReflectCall::Call(InObject, Function, Args, InContext);
		return Result.IsString() ? Result.GetString() : FString();
	}
}

namespace GameTestRpcScript
{
	bool Vars(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		const FString Scope = StrOr(*InArgs, TEXT("scope"), TEXT("widget"));
		const FScopeFn* Resolve = Scopes().Find(Scope);
		if (Resolve == nullptr)
		{
			TArray<FString> Names;
			Scopes().GetKeys(Names);
			OutError = FString::Printf(TEXT("scope 只能是 %s"), *FString::Join(Names, TEXT(" / ")));
			return false;
		}
		FGameTestFlowContext Context;
		Context.World = GameTestWorld::FindGameWorld();

		// 子系统不写 name：列出全部子系统，给面板先挑一个
		if (Scope == TEXT("subsystem") && StrOr(*InArgs, TEXT("name"), FString()).IsEmpty())
		{
			TArray<UObject*> Subsystems;
			if (Context.World != nullptr)
			{
				GameTestObjectRoots::Subsystems(*Context.World, Subsystems);
			}
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const UObject* Subsystem : Subsystems)
			{
				const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Subsystem->GetClass()->GetName());
				Item->SetStringField(TEXT("path"), TEXT("@sub.") + Subsystem->GetClass()->GetName());
				Item->SetStringField(TEXT("type"), Subsystem->GetClass()->GetSuperClass() ? Subsystem->GetClass()->GetSuperClass()->GetName() : FString());
				Item->SetStringField(TEXT("kind"), TEXT("obj"));
				Items.Add(MakeShared<FJsonValueObject>(Item));
			}
			OutResult->SetArrayField(TEXT("subsystems"), Items);
			OutResult->SetNumberField(TEXT("total"), Items.Num());
			OutResult->SetBoolField(TEXT("hasMore"), false);
			return true;
		}

		FOwner Owner;
		if (!(*Resolve)(*InArgs, Context, Owner, OutError))
		{
			return false;
		}

		FCollect Collect{ Owner };
		InArgs->TryGetBoolField(TEXT("values"), Collect.bValues);
		InArgs->TryGetBoolField(TEXT("functions"), Collect.bFunctions);
		InArgs->TryGetStringField(TEXT("filter"), Collect.Filter);
		Collect.CollectReflected();
		Collect.CollectLua(Context.World);

		const TSharedPtr<FJsonObject> OwnerJson = MakeShared<FJsonObject>();
		if (Owner.Class != nullptr)
		{
			OwnerJson->SetStringField(TEXT("class"), Owner.Class->GetPathName());
			if (Cast<UBlueprintGeneratedClass>(Owner.Class) != nullptr)
			{
				FString Asset = Owner.Class->GetPathName();
				Asset.RemoveFromEnd(TEXT("_C"));
				OwnerJson->SetStringField(TEXT("asset"), Asset);
			}
		}
		const FString LuaModule = Owner.LuaModule.IsEmpty() ? VarsLuaModuleOf(Owner.Instance.Get(), Context) : Owner.LuaModule;
		if (!LuaModule.IsEmpty())
		{
			OwnerJson->SetStringField(TEXT("luaModule"), LuaModule);
		}
		OutResult->SetObjectField(TEXT("owner"), OwnerJson);

		// 分页跨组：先按 offset / limit 取条目，再把条目放回各自的组（空组不输出）
		int32 Offset = 0, Limit = VarsDefaultLimit;
		InArgs->TryGetNumberField(TEXT("offset"), Offset);
		InArgs->TryGetNumberField(TEXT("limit"), Limit);
		Offset = FMath::Max(0, Offset);
		Limit = FMath::Clamp(Limit, 1, VarsMaxLimit);
		TArray<TArray<TSharedPtr<FJsonValue>>> PerGroup;
		PerGroup.SetNum(Collect.Groups.Num());
		int32 Chars = 0, Taken = 0, Index = Offset;
		bool bTruncated = false;
		for (; Index < Collect.Entries.Num() && Taken < Limit; ++Index)
		{
			const FEntry& Entry = Collect.Entries[Index];
			Chars += GameTestJson::ToCompactString(Entry.Json).Len();
			if (Chars > VarsMaxChars && Taken > 0)
			{
				bTruncated = true;
				break;
			}
			PerGroup[Entry.Group].Add(MakeShared<FJsonValueObject>(Entry.Json));
			++Taken;
		}
		TArray<TSharedPtr<FJsonValue>> Groups;
		for (int32 GroupIndex = 0; GroupIndex < Collect.Groups.Num(); ++GroupIndex)
		{
			if (PerGroup[GroupIndex].Num() > 0)
			{
				Collect.Groups[GroupIndex].Head->SetArrayField(TEXT("items"), PerGroup[GroupIndex]);
				Groups.Add(MakeShared<FJsonValueObject>(Collect.Groups[GroupIndex].Head));
			}
		}
		OutResult->SetArrayField(TEXT("groups"), Groups);
		OutResult->SetNumberField(TEXT("total"), Collect.Entries.Num());
		OutResult->SetBoolField(TEXT("hasMore"), Index < Collect.Entries.Num());
		if (Index < Collect.Entries.Num())
		{
			OutResult->SetNumberField(TEXT("nextOffset"), Index);
		}
		if (bTruncated)
		{
			OutResult->SetBoolField(TEXT("truncated"), true);
		}
		return true;
	}
}
