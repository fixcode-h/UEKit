// Copyright Epic Games, Inc. All Rights Reserved.

#include "EditorAct/GameTestEditorActions.h"

#include "EditorAct/GameTestEditorCommands.h"
#include "EditorAct/GameTestEditorRefs.h"
#include "EditorAct/GameTestEditorSlateFind.h"

#include "Core/GameTestAction.h"
#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestJson.h"
#include "Editor/GameTestEditorApi.h"
#include "Input/GameTestInput.h"

#include "AgentPanelBridge.h"
#include "AgentPanelBridgeUtil.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWidget.h"

namespace GameTestEditorActionsDetail
{
	// 不在文件作用域 using namespace GameTestHandlers：它也有 HandleSelect / HandleAssert / HandleClick，
	// 下面 Register 里取 &HandleSelect 会两边都找到、报重载有歧义（C2568）；unity build 时还会漏到同一块的其他文件。只引进要用的三个返回值。
	using GameTestHandlers::StepSucceeded;
	using GameTestHandlers::StepFailed;
	using GameTestHandlers::StepPending;

	/** 调 UELink 已有的编辑器操作（同一个进程里直接调，不走 HTTP）。 */
	bool CallUELink(const TCHAR* InOp, const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		FAgentPanelBridge& Bridge = FAgentPanelBridge::Get();
		if (!Bridge.HasOp(InOp))
		{
			OutError = FString::Printf(TEXT("UELink 没有 %s 操作：在 Agent 面板的「UE 插件」页把 UELink 升到最新版"), InOp);
			return false;
		}
		return Bridge.CallOp(InOp, InArgs, OutResult, OutError);
	}

	TArray<FString> StringArray(const FJsonObject& InParams, const TCHAR* InField)
	{
		TArray<FString> Out;
		if (const TArray<TSharedPtr<FJsonValue>>* Values = GameTestJson::TryGetArray(InParams, InField))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Text;
				if (Value.IsValid() && Value->TryGetString(Text) && !Text.IsEmpty())
				{
					Out.Add(Text);
				}
			}
		}
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& InValues)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& Value : InValues)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	EGameTestStepStatus Fail(FGameTestStepContext& InContext, const FString& InMessage)
	{
		InContext.Message = InMessage;
		return StepFailed;
	}

	EGameTestStepStatus HandleOpen(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		FString Map;
		FString Asset;
		GameTestJson::TryGetString(Params, TEXT("map"), Map);
		GameTestJson::TryGetString(Params, TEXT("asset"), Asset);
		if (!Map.IsEmpty())
		{
			// 换关卡和 level.load 同一条路：有没保存的改动时引擎会弹框问，回放前先保存或用干净的工作区
			if (!GameTestEditor::LoadLevel(Map, InContext.Message))
			{
				return StepFailed;
			}
			InContext.Message = FString::Printf(TEXT("打开了关卡 %s"), *Map);
			return StepSucceeded;
		}
		if (Asset.IsEmpty())
		{
			return Fail(InContext, TEXT("editor.open 要写 asset（资源路径）或 map（关卡路径）"));
		}
		const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("path"), Asset);
		TSharedPtr<FJsonObject> Result;
		FString Error;
		if (!CallUELink(TEXT("asset.open"), Args, Result, Error))
		{
			return Fail(InContext, Error);
		}
		InContext.Message = FString::Printf(TEXT("打开了 %s 的编辑器"), *Asset);
		return StepSucceeded;
	}

	EGameTestStepStatus HandleSelect(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		const TArray<FString> ActorRefs = StringArray(Params, TEXT("actors"));
		const TArray<FString> Assets = StringArray(Params, TEXT("assets"));
		if (ActorRefs.Num() == 0 && Assets.Num() == 0)
		{
			return Fail(InContext, TEXT("editor.select 要写 actors（actor:<大纲里的名字>）或 assets（资源路径）"));
		}
		TArray<FString> ActorPaths;
		for (const FString& Ref : ActorRefs)
		{
			FString Error;
			UObject* Found = GameTestEditorRefs::Resolve(Ref, Error);
			if (Found == nullptr)
			{
				return Fail(InContext, Error);
			}
			ActorPaths.Add(Found->GetPathName());
		}
		bool bFocus = false;
		GameTestJson::TryGetBool(Params, TEXT("focus"), bFocus);
		const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetArrayField(TEXT("actors"), ToJsonStrings(ActorPaths));
		Args->SetArrayField(TEXT("assets"), ToJsonStrings(Assets));
		Args->SetBoolField(TEXT("focus"), bFocus);
		TSharedPtr<FJsonObject> Result;
		FString Error;
		if (!CallUELink(TEXT("editor.select"), Args, Result, Error))
		{
			return Fail(InContext, Error);
		}
		const TArray<TSharedPtr<FJsonValue>>* Missing = nullptr;
		if (Result.IsValid() && Result->TryGetArrayField(TEXT("missing"), Missing) && Missing != nullptr && Missing->Num() > 0)
		{
			return Fail(InContext, FString::Printf(TEXT("有 %d 个没找到（%s）"), Missing->Num(), *GameTestJson::ToCompactString(*Missing)));
		}
		InContext.Message = FString::Printf(TEXT("选中了 %s"), *FString::Join(ActorRefs.Num() > 0 ? ActorRefs : Assets, TEXT("、")));
		return StepSucceeded;
	}

	EGameTestStepStatus HandleProp(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		FString Ref;
		FString Path;
		GameTestJson::TryGetString(Params, TEXT("object"), Ref);
		GameTestJson::TryGetString(Params, TEXT("path"), Path);
		const TSharedPtr<FJsonValue> Value = Params.TryGetField(TEXT("value"));
		if (Path.IsEmpty() || !Value.IsValid())
		{
			return Fail(InContext, TEXT("editor.prop 要写 object、path（属性路径，例如 ShopConfig.Discount）和 value"));
		}
		FString Error;
		UObject* Target = GameTestEditorRefs::Resolve(Ref, Error);
		if (Target == nullptr)
		{
			return Fail(InContext, Error);
		}
		AgentPanel::FResolved Resolved;
		if (!AgentPanel::ResolvePath(Target->GetClass(), Target, Target, Path, Resolved, Error))
		{
			return Fail(InContext, Error);
		}
		if (!Resolved.bEditable)
		{
			return Fail(InContext, FString::Printf(TEXT("不能改 %s：%s"), *Path, *Resolved.ReadOnlyWhy));
		}
		UObject* Owner = Resolved.Owner != nullptr ? Resolved.Owner : Target;

		// 包在事务里：回放改的属性在编辑器里 Ctrl+Z 能撤销，和人在细节面板里改的一样
		const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("GameTest：改 %s.%s"), *GameTestEditorRefs::Describe(Target), *Path)));
		Owner->Modify();
		Owner->PreEditChange(Resolved.Prop);
		const bool bOk = AgentPanel::FromJson(Resolved.Prop, Resolved.Value, Value, Error);
		FPropertyChangedEvent ChangedEvent(Resolved.Prop, EPropertyChangeType::ValueSet);
		Owner->PostEditChangeProperty(ChangedEvent);
		if (!bOk)
		{
			return Fail(InContext, FString::Printf(TEXT("%s 写不进去：%s"), *Path, *Error));
		}
		Owner->MarkPackageDirty();
		InContext.Payload->SetField(TEXT("value"), AgentPanel::ToJson(Resolved.Prop, Resolved.Value, 2000));
		InContext.Message = FString::Printf(TEXT("%s 的 %s 改好了"), *GameTestEditorRefs::Describe(Target), *Path);
		return StepSucceeded;
	}

	EGameTestStepStatus HandleSpawn(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		FString Class;
		FString Label;
		GameTestJson::TryGetString(Params, TEXT("class"), Class);
		GameTestJson::TryGetString(Params, TEXT("label"), Label);
		if (Class.IsEmpty())
		{
			return Fail(InContext, TEXT("editor.spawn 要写 class（原生类名或蓝图路径）"));
		}
		// 后面的步骤按标签找这个 Actor：关卡里已经有同名的（上次回放留下的）就会改到旧的那个
		if (!Label.IsEmpty() && GameTestEditorRefs::FindActorByLabel(Label) != nullptr)
		{
			return Fail(InContext, FString::Printf(TEXT("关卡里已经有「%s」了（上次回放留下的？）：先撤销或换一个干净的关卡再跑"), *Label));
		}
		const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("class"), Class);
		const TCHAR* const VectorFields[] = { TEXT("location"), TEXT("rotation"), TEXT("scale") };
		for (const TCHAR* Field : VectorFields)
		{
			if (const TArray<TSharedPtr<FJsonValue>>* Values = GameTestJson::TryGetArray(Params, Field))
			{
				Args->SetArrayField(Field, *Values);
			}
		}
		if (!Label.IsEmpty())
		{
			Args->SetStringField(TEXT("label"), Label);
		}
		TSharedPtr<FJsonObject> Result;
		FString Error;
		if (!CallUELink(TEXT("editor.spawn"), Args, Result, Error))
		{
			return Fail(InContext, Error);
		}
		const TSharedPtr<FJsonObject> Actor = Result.IsValid() ? GameTestJson::TryGetObject(*Result, TEXT("actor")) : nullptr;
		if (Actor.IsValid())
		{
			InContext.Payload->SetObjectField(TEXT("actor"), Actor);
		}
		InContext.Message = FString::Printf(TEXT("放了一个 %s%s"), *Class, Label.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("（%s）"), *Label));
		return StepSucceeded;
	}

	EGameTestStepStatus HandleCommand(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		FString Name;
		FString Asset;
		FString Console;
		GameTestJson::TryGetString(Params, TEXT("name"), Name);
		GameTestJson::TryGetString(Params, TEXT("asset"), Asset);
		GameTestJson::TryGetString(Params, TEXT("console"), Console);
		if (!Console.IsEmpty())
		{
			const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("command"), Console);
			TSharedPtr<FJsonObject> Result;
			FString Error;
			if (!CallUELink(TEXT("console"), Args, Result, Error))
			{
				return Fail(InContext, Error);
			}
			InContext.Message = FString::Printf(TEXT("执行了控制台命令 %s"), *Console);
			return StepSucceeded;
		}
		if (Name.IsEmpty())
		{
			return Fail(InContext, TEXT("editor.command 要写 name（上下文.命令，例如 LevelEditor.Save）或 console（控制台命令）"));
		}
		return GameTestEditorCommands::Execute(Name, Asset, InContext.Message) ? StepSucceeded : StepFailed;
	}

	EGameTestStepStatus HandleSave(FGameTestStepContext& InContext)
	{
		TArray<FString> Packages;
		for (const FString& Asset : StringArray(*InContext.Step->Params, TEXT("assets")))
		{
			Packages.AddUnique(GameTestEditorRefs::ToPackageName(Asset));
		}
		if (Packages.Num() == 0)
		{
			return Fail(InContext, TEXT("editor.save 要写 assets（要保存的资源或关卡路径）"));
		}
		const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetArrayField(TEXT("packages"), ToJsonStrings(Packages));
		TSharedPtr<FJsonObject> Result;
		FString Error;
		if (!CallUELink(TEXT("asset.save"), Args, Result, Error))
		{
			return Fail(InContext, Error);
		}
		InContext.Message = FString::Printf(TEXT("保存了 %s"), *FString::Join(Packages, TEXT("、")));
		return StepSucceeded;
	}

	EGameTestStepStatus HandleCompile(FGameTestStepContext& InContext)
	{
		FString Asset;
		if (!GameTestJson::TryGetString(*InContext.Step->Params, TEXT("asset"), Asset) || Asset.IsEmpty())
		{
			return Fail(InContext, TEXT("editor.compile 要写 asset（蓝图路径）"));
		}
		const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("path"), Asset);
		TSharedPtr<FJsonObject> Result;
		FString Error;
		if (!CallUELink(TEXT("bp.compile"), Args, Result, Error))
		{
			return Fail(InContext, Error);
		}
		const TSharedPtr<FJsonObject> Compile = Result.IsValid() ? GameTestJson::TryGetObject(*Result, TEXT("compile")) : nullptr;
		FString Status;
		if (Compile.IsValid())
		{
			GameTestJson::TryGetString(*Compile, TEXT("status"), Status);
			InContext.Payload->SetObjectField(TEXT("compile"), Compile);
		}
		if (Status == TEXT("error"))
		{
			return Fail(InContext, FString::Printf(TEXT("%s 编译有错（详情见这一步的 compile.messages）"), *Asset));
		}
		InContext.Message = FString::Printf(TEXT("%s 编译通过%s"), *Asset, Status == TEXT("warnings") ? TEXT("（有警告）") : TEXT(""));
		return StepSucceeded;
	}

	/** 界面点击和界面检查共用：每 0.2 秒找一次控件（编辑器的控件树很大，不每帧找），找到就交给 InOnFound。 */
	EGameTestStepStatus WaitForWidget(FGameTestStepContext& InContext, TFunction<bool(const TSharedRef<SWidget>&, FString&)> InOnFound)
	{
		FGameTestEditorWidgetSpec Spec;
		FString Error;
		if (!FGameTestEditorWidgetSpec::ReadFrom(*InContext.Step->Params, Spec, Error))
		{
			return Fail(InContext, Error);
		}
		InContext.DefaultTimeoutMs = InContext.UiWaitDefault();
		InContext.PendingProgress = MakeShared<FString>(FString::Printf(TEXT("还没找到 %s"), *Spec.Describe()));
		InContext.PendingFailure = MakeShared<FString>();
		const TSharedPtr<FString> Progress = InContext.PendingProgress;
		const TSharedPtr<FString> Failure = InContext.PendingFailure;
		double NextCheck = 0.0;
		InContext.PendingPredicate = [Spec, Progress, Failure, InOnFound, NextCheck](double InNow) mutable
		{
			if (InNow < NextCheck)
			{
				return false;
			}
			NextCheck = InNow + 0.2;
			TArray<TSharedRef<SWidget>> Found;
			GameTestEditorSlateFind::Find(Spec, Found);
			if (!Found.IsValidIndex(Spec.Nth))
			{
				*Progress = Found.Num() > 0
					? FString::Printf(TEXT("%s 只找到 %d 个，要的是第 %d 个"), *Spec.Describe(), Found.Num(), Spec.Nth + 1)
					: FString::Printf(TEXT("还没找到 %s（对应的编辑器、停靠页开着吗？）"), *Spec.Describe());
				return false;
			}
			FString Message;
			if (!InOnFound(Found[Spec.Nth], Message))
			{
				*Failure = Message;
			}
			return true;
		};
		InContext.Message = FString::Printf(TEXT("等 %s"), *Spec.Describe());
		return StepPending;
	}

	EGameTestStepStatus HandleClick(FGameTestStepContext& InContext)
	{
		return WaitForWidget(InContext, [](const TSharedRef<SWidget>& InWidget, FString& OutMessage)
		{
			FVector2D Center;
			if (!GameTestEditorSlateFind::GetCenter(*InWidget, Center))
			{
				OutMessage = TEXT("找到了控件，但它还没显示出来（大小是 0）");
				return false;
			}
			// 点出模态框时引擎会在这一下里进嵌套消息循环，等模态框关掉才回来（定稿 10.1：要在本地验证能不能跑）
			return GameTestInput::SendVirtualClick(Center, OutMessage);
		});
	}

	EGameTestStepStatus CheckAsset(FGameTestStepContext& InContext, const FString& InAsset, const FString& InCheck)
	{
		FString Error;
		UObject* Asset = AgentPanel::FindObjectByPath(InAsset, /*bLoad=*/true, Error, /*bKeepBlueprint=*/true);
		if (Asset == nullptr)
		{
			return Fail(InContext, Error.IsEmpty() ? FString::Printf(TEXT("找不到 %s"), *InAsset) : Error);
		}
		if (InCheck == TEXT("compiled"))
		{
			const UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
			if (Blueprint == nullptr)
			{
				return Fail(InContext, FString::Printf(TEXT("%s 不是蓝图，check=compiled 只能查蓝图"), *InAsset));
			}
			const bool bOk = Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
			InContext.Message = bOk ? FString::Printf(TEXT("%s 编译通过"), *InAsset) : FString::Printf(TEXT("%s 没编译或编译有错"), *InAsset);
			return bOk ? StepSucceeded : StepFailed;
		}
		if (InCheck == TEXT("saved"))
		{
			const UPackage* Package = Asset->GetOutermost();
			const bool bOk = Package != nullptr && !Package->IsDirty();
			InContext.Message = bOk ? FString::Printf(TEXT("%s 已保存"), *InAsset) : FString::Printf(TEXT("%s 有没保存的改动"), *InAsset);
			return bOk ? StepSucceeded : StepFailed;
		}
		if (InCheck == TEXT("open"))
		{
			UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
			const bool bOk = Editors != nullptr && Editors->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false) != nullptr;
			InContext.Message = bOk ? FString::Printf(TEXT("%s 的编辑器开着"), *InAsset) : FString::Printf(TEXT("%s 的编辑器没开"), *InAsset);
			return bOk ? StepSucceeded : StepFailed;
		}
		return Fail(InContext, FString::Printf(TEXT("check 只能是 compiled / saved / open，现在是 %s"), *InCheck));
	}

	EGameTestStepStatus HandleAssert(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		FString Asset;
		if (GameTestJson::TryGetString(Params, TEXT("asset"), Asset) && !Asset.IsEmpty())
		{
			FString Check = TEXT("compiled");
			GameTestJson::TryGetString(Params, TEXT("check"), Check);
			return CheckAsset(InContext, Asset, Check);
		}
		return WaitForWidget(InContext, [](const TSharedRef<SWidget>&, FString& OutMessage)
		{
			OutMessage = TEXT("控件在");
			return true;
		});
	}
}

void GameTestEditorActions::Register()
{
	const TCHAR* Module = TEXT("GameTestEditor");
	GameTestActions::Register({ TEXT("editor.open"), TEXT("asset,map"), false, true,
		TEXT("编辑器里打开资源（asset）或关卡（map）"), &GameTestEditorActionsDetail::HandleOpen, Module });
	GameTestActions::Register({ TEXT("editor.select"), TEXT("actors,assets,focus"), false, true,
		TEXT("在关卡里选中 Actor（actor:<大纲里的名字>）、在内容浏览器里定位资源"), &GameTestEditorActionsDetail::HandleSelect, Module });
	GameTestActions::Register({ TEXT("editor.prop"), TEXT("object,path,value"), false, true,
		TEXT("改 Actor、组件或资源的属性（在事务里，能撤销）"), &GameTestEditorActionsDetail::HandleProp, Module });
	GameTestActions::Register({ TEXT("editor.spawn"), TEXT("class,location,rotation,scale,label"), false, true,
		TEXT("在关卡里放一个 Actor，标签设成 label（后面的步骤按标签找它）"), &GameTestEditorActionsDetail::HandleSpawn, Module });
	GameTestActions::Register({ TEXT("editor.command"), TEXT("name,asset,console"), false, true,
		TEXT("执行编辑器命令（上下文.命令，资源编辑器的命令加 asset），或控制台命令"), &GameTestEditorActionsDetail::HandleCommand, Module });
	GameTestActions::Register({ TEXT("editor.save"), TEXT("assets"), false, true,
		TEXT("保存资源或关卡"), &GameTestEditorActionsDetail::HandleSave, Module });
	GameTestActions::Register({ TEXT("editor.compile"), TEXT("asset"), false, true,
		TEXT("编译蓝图，编译有错就失败"), &GameTestEditorActionsDetail::HandleCompile, Module });
	GameTestActions::Register({ TEXT("editor.click"), TEXT("tab,window,widget,text,nth,suggest"), false, true,
		TEXT("点编辑器界面上的控件（按停靠页、类型、文字认），等到它出现再点"), &GameTestEditorActionsDetail::HandleClick, Module });
	GameTestActions::Register({ TEXT("editor.assert"), TEXT("tab,window,widget,text,nth,asset,check"), false, true,
		TEXT("检查编辑器界面上有这个控件，或资源的状态（check=compiled / saved / open）"), &GameTestEditorActionsDetail::HandleAssert, Module });
}

void GameTestEditorActions::Unregister()
{
	const TCHAR* const Names[] = { TEXT("editor.open"), TEXT("editor.select"), TEXT("editor.prop"), TEXT("editor.spawn"), TEXT("editor.command"), TEXT("editor.save"), TEXT("editor.compile"), TEXT("editor.click"), TEXT("editor.assert") };
	for (const TCHAR* Name : Names)
	{
		GameTestActions::Unregister(FName(Name));
	}
}
