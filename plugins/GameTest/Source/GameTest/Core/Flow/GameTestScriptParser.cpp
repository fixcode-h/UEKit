// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/GameTestScriptParser.h"

#include "Core/Flow/Blocks/GameTestBlock.h"
#include "Core/Flow/GameTestGuards.h"
#include "Core/GameTestAction.h"
#include "Core/GameTestScenario.h"
#include "Observe/Ref/GameTestRef.h"
#include "Operate/Obj/GameTestObjActions.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Observe/Checks/GameTestCond.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	/** 块和叶子都认的公共字段（协议第 2 节）。 */
	const TArray<FString>& CommonFields()
	{
		static const TArray<FString> Fields = {
			TEXT("action"), TEXT("id"), TEXT("note"), TEXT("onFail"), TEXT("retries"), TEXT("timeoutMs"), TEXT("skipIf"),
			TEXT("guards"), TEXT("expectNoErrors"), TEXT("disabled"), TEXT("hint"),
		};
		return Fields;
	}

	/** 叶子里装条件、表达式、子步骤的字段：不按填值检查（它们有自己的检查）。 */
	const TArray<FString>& NonValueFields()
	{
		static const TArray<FString> Fields = {
			TEXT("action"), TEXT("id"), TEXT("note"), TEXT("hint"), TEXT("skipIf"), TEXT("guards"), TEXT("cond"), TEXT("expr"),
			TEXT("expect"), TEXT("until"), TEXT("onFail"),
			// 写引用、部件、判断项、关闭方式的字段由 GameTestRefCheck 检查（$var 是对象，不是填值）
			TEXT("target"), TEXT("toTarget"), TEXT("obj"), TEXT("prop"), TEXT("op"), TEXT("how"),
		};
		return Fields;
	}

	bool IsValidId(const FString& InId)
	{
		if (InId.IsEmpty() || InId.Len() > 64)
		{
			return false;
		}
		for (const TCHAR C : InId)
		{
			if (!(C < 0x80 && (FChar::IsAlnum(C) || C == TEXT('_'))))
			{
				return false;
			}
		}
		return true;
	}

	/** ParseStep 给的是中文句子，按内容归到错误码（面板按码给修复建议）。2.0.0 起新加的码写在句首（`selector_removed：…`）。 */
	FName CodeOfLeafError(const FString& InError)
	{
		FString Head;
		if (InError.Split(TEXT("："), &Head, nullptr) && !Head.IsEmpty() && Head.Len() <= 24
			&& !Head.Contains(TEXT(" ")) && Head.ToLower() == Head && FChar::IsAlpha(Head[0]))
		{
			return FName(*Head);
		}
		if (InError.Contains(TEXT("未知动作")) || InError.Contains(TEXT("没有加载")))
		{
			return TEXT("unknown_action");
		}
		if (InError.Contains(TEXT("不认识字段")))
		{
			return TEXT("unknown_field");
		}
		return TEXT("bad_type");
	}
}

namespace GameTestScriptFiles
{
	FString NormalizeCallPath(const FString& InScript)
	{
		FString Path = InScript.Replace(TEXT("\\"), TEXT("/")).TrimStartAndEnd();
		while (Path.StartsWith(TEXT("/")) || Path.StartsWith(TEXT("./")))
		{
			Path.RemoveFromStart(TEXT("./"));
			Path.RemoveFromStart(TEXT("/"));
		}
		if (!Path.EndsWith(TEXT(".json"), ESearchCase::IgnoreCase))
		{
			Path += TEXT(".json");
		}
		return Path;
	}

	TSharedPtr<FJsonObject> LoadJson(const FString& InAbsolutePath, FString& OutError)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *InAbsolutePath))
		{
			OutError = FString::Printf(TEXT("读不到 %s"), *InAbsolutePath);
			return nullptr;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			OutError = FString::Printf(TEXT("%s 不是合法的 JSON"), *InAbsolutePath);
			return nullptr;
		}
		return Root;
	}
}

FGameTestScriptParser::FGameTestScriptParser(const FString& InScenarioRoot, FGameTestDiagSink& InSink, const FGameTestScriptOptions& InOptions)
	: ScenarioRoot(InScenarioRoot)
	, DiagSink(InSink)
	, Options(InOptions)
{
}

TSharedRef<FGameTestScript> FGameTestScriptParser::ParseScript(const TSharedPtr<FJsonObject>& InJson, const FString& InPath)
{
	TSharedRef<FGameTestScript> Script = MakeShared<FGameTestScript>();
	Script->Path = InPath;
	FScriptState& Scope = States.AddDefaulted_GetRef();
	Scope.Path = InPath;
	Scope.Vars.AddDefaulted();
	if (!InJson.IsValid())
	{
		DiagSink.Error(TEXT("bad_type"), TEXT("场景不是 JSON 对象"));
	}
	else
	{
		ParseHeader(InJson, *Script);
		ParseNodes(InJson->TryGetField(TEXT("setup")), TEXT("setup"), Script->Setup);
		ParseNodes(InJson->TryGetField(TEXT("steps")), TEXT("steps"), Script->Steps);
	}
	States.Pop();
	return Script;
}

TSharedRef<FGameTestScript> FGameTestScriptParser::ParseSteps(const TArray<TSharedPtr<FJsonObject>>& InSteps, const FString& InName, const FString& InListKey)
{
	TSharedRef<FGameTestScript> Script = MakeShared<FGameTestScript>();
	Script->Name = InName;
	FScriptState& Scope = States.AddDefaulted_GetRef();
	Scope.Vars.AddDefaulted();
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const TSharedPtr<FJsonObject>& Step : InSteps)
	{
		Values.Add(MakeShared<FJsonValueObject>(Step));
	}
	ParseNodes(MakeShared<FJsonValueArray>(Values), InListKey, InListKey == TEXT("setup") ? Script->Setup : Script->Steps);
	States.Pop();
	return Script;
}

void FGameTestScriptParser::ParseHeader(const TSharedPtr<FJsonObject>& InJson, FGameTestScript& OutScript)
{
	OutScript.Name = GameTestScenario::GetName(InJson, FPaths::GetBaseFilename(OutScript.Path));
	int32 Version = GameTestScenario::SupportedSchemaVersion;
	InJson->TryGetNumberField(TEXT("schemaVersion"), Version);
	OutScript.SchemaVersion = Version;
	if (Version > GameTestScenario::SupportedSchemaVersion)
	{
		DiagSink.Error(TEXT("schema_too_new"), FString::Printf(TEXT("场景是 schemaVersion %d，这个 GameTest 只认到 %d：升级 UE 里的 GameTest 插件"), Version, GameTestScenario::SupportedSchemaVersion));
	}
	else if (Version < GameTestScenario::SupportedSchemaVersion)
	{
		// 2.1.0 起只认 v5（属性路径 v5）：不做迁移（用户定），旧脚本按新写法重写
		DiagSink.Error(TEXT("schema_old"), FString::Printf(TEXT("旧格式不再支持，请按新写法重写（脚本是 schemaVersion %d，这个 GameTest 只认 %d，写法见 docs/game-test-ref.md）"), Version, GameTestScenario::SupportedSchemaVersion));
	}
	const TSharedPtr<FJsonObject>* Params = nullptr;
	if (InJson->TryGetObjectField(TEXT("params"), Params) && Params && Params->IsValid())
	{
		for (const auto& Pair : (*Params)->Values)
		{
			FGameTestParamDef& Def = OutScript.Params.AddDefaulted_GetRef();
			Def.Name = GAMETEST_JSON_KEY(Pair.Key);
			const TSharedPtr<FJsonObject>* Spec = nullptr;
			if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Spec) && Spec && Spec->IsValid())
			{
				Def.Default = (*Spec)->TryGetField(TEXT("default"));
				(*Spec)->TryGetStringField(TEXT("type"), Def.Type);
				(*Spec)->TryGetStringField(TEXT("note"), Def.Note);
			}
			else
			{
				// 简写：`"params": {"account": "qa_01"}` 就是默认值。
				Def.Default = Pair.Value;
			}
			DeclareVar(Def.Name);
		}
	}
	const TSharedPtr<FJsonObject>* Vars = nullptr;
	if (InJson->TryGetObjectField(TEXT("vars"), Vars) && Vars && Vars->IsValid())
	{
		OutScript.Vars = *Vars;
		for (const auto& Pair : (*Vars)->Values)
		{
			DeclareVar(GAMETEST_JSON_KEY(Pair.Key));
		}
	}
	const TSharedPtr<FJsonObject>* Objects = nullptr;
	State().bHasObjects = true;
	if (InJson->TryGetObjectField(TEXT("objects"), Objects) && Objects && Objects->IsValid())
	{
		OutScript.Objects = *Objects;
		for (const auto& Pair : (*Objects)->Values)
		{
			State().Objects.Add(GAMETEST_JSON_KEY(Pair.Key));
		}
		GameTestRefCheck::Objects(**Objects, MakeExprScope(), DiagSink);
	}
	else if (InJson->HasField(TEXT("objects")))
	{
		FGameTestDiagSink::FScope Scope(DiagSink, TEXT("objects"));
		DiagSink.Error(TEXT("bad_type"), TEXT("objects 要写成对象 {名字: 引用}"));
	}
	if (InJson->HasField(TEXT("guards")))
	{
		GameTestGuards::ParseList(InJson->TryGetField(TEXT("guards")), TEXT("script"), TEXT("guards"), *this, OutScript.Guards);
	}
}

void FGameTestScriptParser::ParseNodes(const TSharedPtr<FJsonValue>& InValue, const FString& InPath, FGameTestNodeList& OutList)
{
	OutList.Key = InPath;
	if (!InValue.IsValid())
	{
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!InValue->TryGetArray(Items) || !Items)
	{
		FGameTestDiagSink::FScope Scope(DiagSink, InPath, FString(), FGameTestDiagSink::EScopeMode::FromRoot);
		DiagSink.Error(TEXT("bad_type"), TEXT("要写成步骤数组"));
		return;
	}
	const bool bTopLevel = !InPath.Contains(TEXT("."));
	for (int32 Index = 0; Index < Items->Num(); ++Index)
	{
		const TSharedRef<FGameTestNode> Node = ParseNode((*Items)[Index], FString::Printf(TEXT("%s.%d"), *InPath, Index));
		if (!Node->IsBlock())
		{
			Node->Step.bNested = !bTopLevel;
			Node->Step.bSetup = InPath == TEXT("setup");
			Node->Step.Index = Index;
		}
		OutList.Nodes.Add(Node);
	}
}

TSharedRef<FGameTestNode> FGameTestScriptParser::ParseNode(const TSharedPtr<FJsonValue>& InJson, const FString& InPath)
{
	if (States.Num() == 0)
	{
		// 不在某份脚本里解析的步骤（运行配置的全局守护）：给一份空的脚本状态，id 查重和变量表照常用。
		States.AddDefaulted_GetRef().Vars.AddDefaulted();
	}
	TSharedRef<FGameTestNode> Node = MakeShared<FGameTestNode>();
	Node->Path = InPath;
	const TSharedPtr<FJsonObject>* Object = nullptr;
	FGameTestDiagSink::FScope PathScope(DiagSink, InPath, FString(), FGameTestDiagSink::EScopeMode::FromRoot);
	if (!InJson.IsValid() || !InJson->TryGetObject(Object) || !Object || !Object->IsValid())
	{
		DiagSink.Error(TEXT("bad_type"), TEXT("步骤要写成对象"));
		return Node;
	}
	Node->Json = *Object;
	FString Action;
	(*Object)->TryGetStringField(TEXT("action"), Action);
	Node->Action = FName(*Action);
	(*Object)->TryGetStringField(TEXT("id"), Node->Id);
	// 路径压栈后再带上 id：这一步里的错误都标上它的 id。
	FGameTestDiagSink::FScope IdScope(DiagSink, FString(), Node->Id);
	ParseCommon(**Object, *Node);
	if (Action.IsEmpty())
	{
		DiagSink.Error(TEXT("bad_type"), TEXT("缺少 action"));
		return Node;
	}
	if (const IGameTestBlock* Block = GameTestBlocks::FindFor(**Object, Node->Action))
	{
		Node->Block = Block;
		ParseBlock(**Object, *Node);
	}
	else
	{
		ParseLeaf(*Object, *Node);
	}
	Node->Step.Id = Node->Id;
	Node->Step.Path = Node->Path;
	return Node;
}

void FGameTestScriptParser::ParseCommon(const FJsonObject& InJson, FGameTestNode& OutNode)
{
	if (!OutNode.Id.IsEmpty())
	{
		if (!IsValidId(OutNode.Id))
		{
			DiagSink.Error(TEXT("bad_type"), FString::Printf(TEXT("id「%s」只能用字母、数字、下划线（最长 64）"), *OutNode.Id));
		}
		else if (State().Ids.Contains(OutNode.Id))
		{
			DiagSink.Error(TEXT("dup_id"), FString::Printf(TEXT("id「%s」重复了：场景里每个步骤的 id 要不一样"), *OutNode.Id));
		}
		State().Ids.Add(OutNode.Id);
	}
	InJson.TryGetBoolField(TEXT("disabled"), OutNode.bDisabled);
	InJson.TryGetBoolField(TEXT("expectNoErrors"), OutNode.bExpectNoErrors);
	InJson.TryGetNumberField(TEXT("retries"), OutNode.Retries);
	OutNode.Retries = FMath::Clamp(OutNode.Retries, 0, 100);
	FString OnFail;
	if (InJson.TryGetStringField(TEXT("onFail"), OnFail) && OnFail.Equals(TEXT("retry"), ESearchCase::IgnoreCase) && OutNode.Retries == 0)
	{
		// 写了 retry 没写次数：重做一次，比报错更贴近作者意图。
		OutNode.Retries = 1;
	}
	if (InJson.HasField(TEXT("guards")))
	{
		GameTestGuards::ParseList(InJson.TryGetField(TEXT("guards")), TEXT("block"), OutNode.Path + TEXT(".guards"), *this, OutNode.Guards);
	}
}

void FGameTestScriptParser::ParseBlock(const FJsonObject& InJson, FGameTestNode& OutNode)
{
	TArray<FString> Allowed = CommonFields();
	Allowed.Append(OutNode.Block->GetFields());
	for (const auto& Pair : InJson.Values)
	{
		const FString Key = GAMETEST_JSON_KEY(Pair.Key);
		if (!Allowed.Contains(Key))
		{
			DiagSink.Error(TEXT("unknown_field"), FString::Printf(TEXT("%s 不认识字段 %s（可用：%s）"), *OutNode.Action.ToString(), *Key,
				*FString::Join(OutNode.Block->GetFields(), TEXT(", "))));
		}
	}
	// 公共字段（note、timeoutMs、onFail）照叶子的写法读进 Step，执行器统一处理。
	FGameTestStep& Step = OutNode.Step;
	Step.Action = OutNode.Action;
	InJson.TryGetStringField(TEXT("note"), Step.Note);
	if (InJson.TryGetNumberField(TEXT("timeoutMs"), Step.TimeoutMs) && Step.TimeoutMs <= 0)
	{
		// 写了 0 = 整块不限时（和叶子一样记成 -1）；块本来没写就不限，这里只为和叶子一致
		Step.TimeoutMs = -1;
	}
	FString OnFail;
	if (InJson.TryGetStringField(TEXT("onFail"), OnFail))
	{
		Step.FailMode = OnFail.Equals(TEXT("continue"), ESearchCase::IgnoreCase) ? EGameTestFailMode::Continue
			: OnFail.Equals(TEXT("retry"), ESearchCase::IgnoreCase) ? EGameTestFailMode::Retry
			: EGameTestFailMode::Stop;
		if (!OnFail.Equals(TEXT("stop"), ESearchCase::IgnoreCase) && Step.FailMode == EGameTestFailMode::Stop)
		{
			DiagSink.Error(TEXT("bad_type"), FString::Printf(TEXT("onFail 只能是 stop、continue 或 retry，写的是 %s"), *OnFail));
		}
	}
	Step.Params = OutNode.Json;
	Step.bNested = true;
	OutNode.Block->Parse(OutNode, *this);
}

void FGameTestScriptParser::ParseLeaf(const TSharedPtr<FJsonObject>& InJson, FGameTestNode& OutNode)
{
	TArray<FString> Errors;
	GameTestActions::ParseStep(InJson, 0, OutNode.Step, Errors);
	for (const FString& Error : Errors)
	{
		DiagSink.Error(CodeOfLeafError(Error), Error);
	}
	// 1.9.0 给 assert / wait 加的统一条件和表达式
	if (InJson->HasField(TEXT("cond")))
	{
		CheckCond(InJson->TryGetField(TEXT("cond")), TEXT("cond"));
	}
	FString Expr;
	if (InJson->TryGetStringField(TEXT("expr"), Expr))
	{
		CheckValue(MakeShared<FJsonValueString>(Expr.StartsWith(TEXT("=")) ? Expr : TEXT("=") + Expr), TEXT("expr"));
	}
	if (InJson->HasField(TEXT("expect")))
	{
		CheckCond(InJson->TryGetField(TEXT("expect")), TEXT("expect"));
	}
	CheckLeafRefs(*InJson);
	CheckLeafObjects(*InJson, OutNode.Step.Action);
	FString SaveAs;
	if (InJson->TryGetStringField(TEXT("saveAs"), SaveAs) && !SaveAs.IsEmpty())
	{
		DeclareVar(SaveAs);
	}
}

void FGameTestScriptParser::CheckLeafObjects(const FJsonObject& InJson, FName InAction)
{
	const FGameTestExprScope Scope = MakeExprScope();
	for (const TCHAR* Field : { TEXT("target"), TEXT("toTarget") })
	{
		if (const TSharedPtr<FJsonValue> Target = InJson.TryGetField(Field))
		{
			FString Text;
			// 字符串选择器 ParseStep 已经报过 selector_removed，这里只查对象写法和名字
			if (Target->TryGetString(Text) && !GameTestRefForm::IsName(Text) && !Text.StartsWith(TEXT("$")))
			{
				continue;
			}
			FGameTestDiagSink::FScope PathScope(DiagSink, Field);
			GameTestRefCheck::Target(Target, Scope, DiagSink);
		}
	}
	GameTestObjActions::Validate(InJson, InAction, Scope, DiagSink);
}

void FGameTestScriptParser::CheckLeafRefs(const FJsonObject& InJson)
{
	for (const auto& Pair : InJson.Values)
	{
		const FString Key = GAMETEST_JSON_KEY(Pair.Key);
		if (NonValueFields().Contains(Key))
		{
			continue;
		}
		CheckValue(Pair.Value, Key);
	}
}

int32 FGameTestScriptParser::ParseList(FGameTestNode& InOutOwner, const FString& InKey, const TSharedPtr<FJsonValue>& InValue, bool bRequired)
{
	if (!InValue.IsValid())
	{
		if (bRequired)
		{
			DiagSink.Error(TEXT("bad_type"), FString::Printf(TEXT("%s 缺少 %s（步骤数组）"), *InOutOwner.Action.ToString(), *InKey));
		}
		return INDEX_NONE;
	}
	FGameTestNodeList& List = InOutOwner.Lists.AddDefaulted_GetRef();
	const int32 Index = InOutOwner.Lists.Num() - 1;
	// ParseNodes 用完整路径做前缀；列表的 Key 记块内的相对名（then、elif.0.then）。
	ParseNodes(InValue, InOutOwner.Path + TEXT(".") + InKey, List);
	List.Key = InKey;
	return Index;
}

void FGameTestScriptParser::CheckCond(const TSharedPtr<FJsonValue>& InCond, const FString& InField)
{
	FGameTestDiagSink::FScope Scope(DiagSink, InField);
	GameTestCond::Validate(InCond, MakeExprScope(), DiagSink);
}

void FGameTestScriptParser::CheckValue(const TSharedPtr<FJsonValue>& InValue, const FString& InField)
{
	FGameTestDiagSink::FScope Scope(DiagSink, InField);
	FGameTestExprScope ExprScope = MakeExprScope();
	GameTestRefs::Validate(InValue, ExprScope, DiagSink);
}

void FGameTestScriptParser::DeclareVar(const FString& InName)
{
	if (States.Num() > 0 && State().Vars.Num() > 0)
	{
		State().Vars.Last().Add(InName);
	}
}

void FGameTestScriptParser::PushVarScope()
{
	if (States.Num() > 0)
	{
		// 新的一层继承外层的名字：循环里看得到循环外的变量。
		const TSet<FString> Outer = State().Vars.Num() > 0 ? State().Vars.Last() : TSet<FString>();
		State().Vars.Add(Outer);
	}
}

void FGameTestScriptParser::PopVarScope()
{
	if (States.Num() > 0 && State().Vars.Num() > 1)
	{
		// 循环里 set 的变量出了循环还在（set 写进脚本变量），所以内层登记的名字并回外层。
		// 循环自己的 index / as 也一起并回去了：只是少报一种 var_undefined，执行时取不到会照常报。
		TSet<FString> Inner = State().Vars.Pop();
		State().Vars.Last().Append(Inner);
	}
}

FGameTestExprScope FGameTestScriptParser::MakeExprScope() const
{
	FGameTestExprScope Scope;
	// 变量表只在解析一份脚本期间有效；表达式检查拿的是指针，所以只在这期间用（解析器的生命周期内）。
	Scope.KnownVars = States.Num() > 0 && States.Last().Vars.Num() > 0 ? &States.Last().Vars.Last() : nullptr;
	Scope.bAllowLuaInCond = Options.bAllowLuaInCond;
	// 只有在一份有头的脚本里才查 objects 的名字（全局守护、临时步骤没有 objects，不查）
	Scope.KnownObjects = States.Num() > 0 && States.Last().bHasObjects ? &States.Last().Objects : nullptr;
	return Scope;
}

TSharedPtr<FGameTestScript> FGameTestScriptParser::LoadCall(const FString& InScript)
{
	const FString Path = GameTestScriptFiles::NormalizeCallPath(InScript);
	if (ScenarioRoot.IsEmpty())
	{
		DiagSink.Error(TEXT("call_missing"), TEXT("没有场景根目录，call 找不到子脚本（临时步骤里不能用 call）"));
		return nullptr;
	}
	if (Loading.Contains(Path))
	{
		DiagSink.Error(TEXT("call_cycle"), FString::Printf(TEXT("call 成环：%s → %s"), *FString::Join(Loading, TEXT(" → ")), *Path));
		return nullptr;
	}
	if (Loading.Num() >= Options.MaxCallDepth)
	{
		DiagSink.Error(TEXT("call_depth"), FString::Printf(TEXT("call 嵌套超过 %d 层（运行设置 maxCallDepth）"), Options.MaxCallDepth));
		return nullptr;
	}
	if (const TSharedPtr<FGameTestScript>* Cached = CallCache.Find(Path))
	{
		return *Cached;
	}
	FString Error;
	const TSharedPtr<FJsonObject> Json = GameTestScriptFiles::LoadJson(FPaths::Combine(ScenarioRoot, Path), Error);
	if (!Json.IsValid())
	{
		DiagSink.Error(TEXT("call_missing"), FString::Printf(TEXT("call 的子脚本 %s：%s"), *Path, *Error));
		return nullptr;
	}
	Loading.Add(Path);
	// 子脚本的错误标上它自己的文件，路径从它的根算。
	TSharedPtr<FGameTestScript> Script;
	{
		FGameTestDiagSink::FScope Scope(DiagSink, FString::Printf(TEXT("<%s>"), *Path), FString(), FGameTestDiagSink::EScopeMode::NewRoot);
		Script = ParseScript(Json, Path);
	}
	Loading.Pop();
	CallCache.Add(Path, Script);
	return Script;
}
