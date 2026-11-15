// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Material/MaterialGraphCodec.h"

#include "Core/ForgeJsonText.h"
#include "Core/ForgePropertyCodec.h"

#include "MaterialGraph/MaterialGraphNode.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionComposite.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionPinBase.h"
#include "SceneTypes.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace MaterialGraphCodec
{

namespace
{
	const TCHAR* const FieldInputs = TEXT("inputs");
	const TCHAR* const FieldNodes = TEXT("nodes");
	const TCHAR* const FieldFrom = TEXT("from");
	const TCHAR* const FieldOutput = TEXT("output");

	/** 结构路径的分隔符：与字段路径一致（`BaseColor.A`）。 */
	const TCHAR PathSeparator = TEXT('.');

	/** 没有名字的引脚（引擎把 `Input` 映射成 NAME_None）改用序号引用。 */
	const TCHAR* const IndexPinPrefix = TEXT("#");

	/** 类的短名形如 `TextureSample`：按这个前缀补成引擎里的类名。 */
	const TCHAR* const ExpressionPrefix = TEXT("MaterialExpression");

	FString JoinPath(const FString& InPrefix, const FString& InSegment)
	{
		return InPrefix.IsEmpty() ? InSegment : FString::Printf(TEXT("%s%c%s"), *InPrefix, PathSeparator, *InSegment);
	}

	/** 类的短名：引擎类去掉 `MaterialExpression` 前缀（`Multiply`），其余写全路径。 */
	FString MakeClassKey(const UClass* InClass)
	{
		if (InClass == nullptr)
		{
			return FString();
		}

		const FString Name = InClass->GetName();
		if (InClass->GetOutermost() == nullptr || InClass->GetOutermost()->GetName() == TEXT("/Script/Engine"))
		{
			FString Short = Name;
			if (Short.RemoveFromStart(ExpressionPrefix) || Short.RemoveFromStart(TEXT("UMaterialExpression")))
			{
				return Short;
			}
		}

		return InClass->GetPathName();
	}

	/** 是否是这个契约放不下的节点：折叠图（Composite）及其内部的引脚基座。 */
	bool IsUnsupportedExpression(const UMaterialExpression* InExpression)
	{
		return InExpression != nullptr
			&& (InExpression->IsA<UMaterialExpressionComposite>()
				|| InExpression->IsA<UMaterialExpressionPinBase>()
				|| InExpression->SubgraphExpression != nullptr);
	}

	bool ParseLink(
		const TSharedPtr<FJsonValue>& InValue,
		FMaterialLink& OutLink,
		TArray<FForgeDiagnostic>& OutDiagnostics,
		const FString& InFieldPath)
	{
		if (!InValue.IsValid())
		{
			OutDiagnostics.Emplace(EForgeSeverity::Error, InFieldPath, TEXT("连线不能为空"));
			return false;
		}

		// 简写：只写源节点名，输出引脚取第一个。
		if (InValue->Type == EJson::String)
		{
			OutLink.NodeName = InValue->AsString();
			if (OutLink.NodeName.IsEmpty())
			{
				OutDiagnostics.Emplace(EForgeSeverity::Error, InFieldPath, TEXT("连线不能是空字符串"));
				return false;
			}
			return true;
		}

		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!InValue->TryGetObject(Object) || !Object->IsValid())
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				InFieldPath,
				TEXT("连线要么是节点名字符串，要么是 { \"from\": 节点名, \"output\": 引脚名 }"));
			return false;
		}

		if (!(*Object)->TryGetStringField(FieldFrom, OutLink.NodeName) || OutLink.NodeName.IsEmpty())
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				FString::Printf(TEXT("%s.%s"), *InFieldPath, FieldFrom),
				TEXT("连线的 `from` 必填，且是非空的源节点名"));
			return false;
		}

		(*Object)->TryGetStringField(FieldOutput, OutLink.OutputName);
		return true;
	}

	TSharedPtr<FJsonValue> LinkToJson(const FMaterialLink& InLink)
	{
		// 输出引脚为空 = 第一个输出，写成简写形式；这是导出与判等都认的规范形态。
		if (InLink.OutputName.IsEmpty())
		{
			return MakeShared<FJsonValueString>(InLink.NodeName);
		}

		const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(FieldFrom, InLink.NodeName);
		Object->SetStringField(FieldOutput, InLink.OutputName);
		return MakeShared<FJsonValueObject>(Object);
	}

	/** 一组连线 → JSON 对象（空的键不写）。 */
	TSharedPtr<FJsonObject> LinksToJson(const TArray<TPair<FString, FMaterialLink>>& InLinks)
	{
		if (InLinks.Num() == 0)
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		for (const TPair<FString, FMaterialLink>& Pair : InLinks)
		{
			Object->SetField(Pair.Key, LinkToJson(Pair.Value));
		}
		return Object;
	}

	/** 材质属性输入的排序键：按枚举值排，保证两边（配置与资产）的遍历顺序一致。 */
	void SortPropertyInputs(TArray<TPair<FString, FMaterialLink>>& InOutInputs)
	{
		InOutInputs.Sort([](const TPair<FString, FMaterialLink>& InLeft, const TPair<FString, FMaterialLink>& InRight)
		{
			const int32 Left = ResolveMaterialProperty(InLeft.Key);
			const int32 Right = ResolveMaterialProperty(InRight.Key);
			return Left == Right ? InLeft.Key < InRight.Key : Left < Right;
		});
	}

	/** 输入引脚的访问顺序：按引脚名排。**必须与资产侧的遍历顺序一致**，否则共享节点的结构路径会两边不同。 */
	void CollectSortedInputs(
		const FMaterialNodeSpec& InNode,
		TArray<TPair<FString, FMaterialLink>>& OutInputs)
	{
		OutInputs = InNode.Inputs;
		OutInputs.Sort([](const TPair<FString, FMaterialLink>& InLeft, const TPair<FString, FMaterialLink>& InRight)
		{
			return InLeft.Key < InRight.Key;
		});
	}

	/** 节点名 → 下标。重名在 ValidateGraph 里已经报过，这里只取第一个。 */
	void IndexNodes(const FMaterialGraph& InGraph, TMap<FString, int32>& OutIndices)
	{
		for (int32 Index = 0; Index < InGraph.Nodes.Num(); ++Index)
		{
			OutIndices.Add(InGraph.Nodes[Index].Name, Index);
		}
	}
}

const FMaterialLink* FMaterialNodeSpec::FindInput(const FString& InPinName) const
{
	for (const TPair<FString, FMaterialLink>& Pair : Inputs)
	{
		if (Pair.Key == InPinName)
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}

const FMaterialNodeSpec* FMaterialGraph::FindNode(const FString& InName) const
{
	for (const FMaterialNodeSpec& Node : Nodes)
	{
		if (Node.Name == InName)
		{
			return &Node;
		}
	}
	return nullptr;
}

UClass* ResolveExpressionClass(const FString& InClassName, FString& OutError)
{
	if (InClassName.IsEmpty())
	{
		OutError = TEXT("类名为空");
		return nullptr;
	}

	if (InClassName.Contains(TEXT("/")) || InClassName.Contains(TEXT(".")))
	{
		if (UClass* Loaded = LoadClass<UMaterialExpression>(nullptr, *InClassName))
		{
			return Loaded;
		}

		OutError = FString::Printf(TEXT("找不到材质表达式类 `%s`"), *InClassName);
		return nullptr;
	}

	// 短名：`TextureSample` → /Script/Engine.MaterialExpressionTextureSample。
	// 先试补前缀的写法，再试原样——后者覆盖少数不带前缀的名字。
	const FString Prefixed = FString::Printf(TEXT("/Script/Engine.%s%s"), ExpressionPrefix, *InClassName);
	const FString AsIs = FString::Printf(TEXT("/Script/Engine.%s"), *InClassName);
	for (const FString& Candidate : { Prefixed, AsIs })
	{
		if (UClass* Loaded = LoadClass<UMaterialExpression>(nullptr, *Candidate))
		{
			return Loaded;
		}
	}

	OutError = FString::Printf(
		TEXT("找不到材质表达式类 `%s`；短名只在 /Script/Engine 下有效，其它类请写完整路径"),
		*InClassName);
	return nullptr;
}

int32 ResolveMaterialProperty(const FString& InName)
{
	if (InName.IsEmpty())
	{
		return INDEX_NONE;
	}

	FString Bare = InName;
	if (!Bare.RemoveFromStart(TEXT("MP_"), ESearchCase::IgnoreCase))
	{
		Bare = InName;
	}

	// 用反射枚举查而不是手写一张表：引擎加了新属性（Substrate 就加过）这边自动跟上。
	const UEnum* PropertyEnum = StaticEnum<EMaterialProperty>();
	if (PropertyEnum == nullptr)
	{
		return INDEX_NONE;
	}

	const int32 NumEnums = PropertyEnum->NumEnums();
	for (int32 Index = 0; Index < NumEnums; ++Index)
	{
		const FString EntryName = PropertyEnum->GetNameStringByIndex(Index);
		if (!EntryName.StartsWith(TEXT("MP_")))
		{
			continue;
		}

		if (EntryName.RightChop(3).Equals(Bare, ESearchCase::IgnoreCase))
		{
			const int64 Value = PropertyEnum->GetValueByIndex(Index);
			return Value == MP_MAX ? INDEX_NONE : static_cast<int32>(Value);
		}
	}

	return INDEX_NONE;
}

FString MakeMaterialPropertyKey(int32 InProperty)
{
	const UEnum* PropertyEnum = StaticEnum<EMaterialProperty>();
	if (PropertyEnum == nullptr || InProperty < 0 || InProperty >= MP_MAX)
	{
		return FString();
	}

	FString EntryName = PropertyEnum->GetNameStringByValue(InProperty);
	EntryName.RemoveFromStart(TEXT("MP_"));
	return EntryName;
}

FString GetInputPinName(UMaterialExpression* InExpression, int32 InInputIndex)
{
	if (InExpression == nullptr || InInputIndex < 0 || InInputIndex >= InExpression->CountInputs())
	{
		return FString();
	}

	// 与 UMaterialEditingLibrary 里的解析保持一致（它也是这么分岔的），
	// 否则导出写出的名字会被连接 API 认成另一个引脚。
	if (const UMaterialExpressionMaterialFunctionCall* FunctionCall = Cast<UMaterialExpressionMaterialFunctionCall>(InExpression))
	{
		const FString TypeLess = FunctionCall->GetInputNameWithType(InInputIndex, /*bWithType=*/false).ToString();
		return TypeLess.IsEmpty() ? FString::Printf(TEXT("%s%d"), IndexPinPrefix, InInputIndex) : TypeLess;
	}

	const FString Shortened = UMaterialGraphNode::GetShortenPinName(InExpression->GetInputName(InInputIndex)).ToString();

	// 引擎把少数长名字缩成 NAME_None（`Input` → 无），那些引脚没有可写的名字，用序号引用。
	return Shortened.IsEmpty() ? FString::Printf(TEXT("%s%d"), IndexPinPrefix, InInputIndex) : Shortened;
}

FString GetOutputPinName(const UMaterialExpression* InExpression, int32 InOutputIndex)
{
	if (InExpression == nullptr || InOutputIndex < 0 || InOutputIndex >= InExpression->Outputs.Num())
	{
		return FString();
	}

	const FExpressionOutput& Output = InExpression->Outputs[InOutputIndex];
	if (!Output.OutputName.IsNone())
	{
		return Output.OutputName.ToString();
	}

	// 掩码输出没有名字，但连接 API 认 R/G/B/A（见 GetExpressionOutputIndexByName）。
	if (Output.MaskR && !Output.MaskG && !Output.MaskB && !Output.MaskA)
	{
		return TEXT("R");
	}
	if (Output.MaskG && !Output.MaskR && !Output.MaskB && !Output.MaskA)
	{
		return TEXT("G");
	}
	if (Output.MaskB && !Output.MaskR && !Output.MaskG && !Output.MaskA)
	{
		return TEXT("B");
	}
	if (Output.MaskA && !Output.MaskR && !Output.MaskG && !Output.MaskB)
	{
		return TEXT("A");
	}

	// 其余（含全通道输出）留空 = 第一个输出。
	return FString();
}

int32 ResolveInputIndex(UMaterialExpression* InExpression, const FString& InPinName)
{
	if (InExpression == nullptr)
	{
		return INDEX_NONE;
	}

	// `#序号`：引擎把少数长名字缩成 NAME_None，那些引脚没有可写的名字。
	if (InPinName.StartsWith(IndexPinPrefix))
	{
		const FString Digits = InPinName.RightChop(1);
		if (Digits.IsEmpty() || !Digits.IsNumeric())
		{
			return INDEX_NONE;
		}

		const int32 Index = FCString::Atoi(*Digits);
		return (Index >= 0 && Index < InExpression->CountInputs()) ? Index : INDEX_NONE;
	}

	const int32 InputCount = InExpression->CountInputs();
	for (int32 Index = 0; Index < InputCount; ++Index)
	{
		if (GetInputPinName(InExpression, Index) == InPinName)
		{
			return Index;
		}
	}

	// 长名字也认：`Coordinates` 与编辑器显示的 `UVs` 是同一个引脚。引擎自己的文档与头文件
	// 用的是长名字，让作者为此去记缩写没必要——导出的永远是最短的那个写法。
	for (int32 Index = 0; Index < InputCount; ++Index)
	{
		if (InExpression->GetInputName(Index) == *InPinName)
		{
			return Index;
		}
	}

	return INDEX_NONE;
}

int32 ResolveOutputIndex(UMaterialExpression* InExpression, const FString& InOutputName)
{
	if (InExpression == nullptr || InExpression->Outputs.Num() == 0)
	{
		return INDEX_NONE;
	}

	// 空 = 第一个输出（引擎的 GetExpressionOutputIndexByName 也是这么认的）。
	if (InOutputName.IsEmpty())
	{
		return 0;
	}

	for (int32 Index = 0; Index < InExpression->Outputs.Num(); ++Index)
	{
		if (InExpression->Outputs[Index].OutputName == *InOutputName)
		{
			return Index;
		}
	}

	// 掩码输出：引擎只认 R/G/B/A，编辑器里那个整块输出显示成 `RGB`——一并接受，
	// 因为作者看到的就是那个字。
	for (int32 Index = 0; Index < InExpression->Outputs.Num(); ++Index)
	{
		if (GetOutputPinName(InExpression, Index).Equals(InOutputName, ESearchCase::IgnoreCase))
		{
			return Index;
		}
	}

	if (InOutputName.Equals(TEXT("RGB"), ESearchCase::IgnoreCase))
	{
		for (int32 Index = 0; Index < InExpression->Outputs.Num(); ++Index)
		{
			const FExpressionOutput& Output = InExpression->Outputs[Index];
			if (Output.MaskR && Output.MaskG && Output.MaskB)
			{
				return Index;
			}
		}
	}

	return INDEX_NONE;
}

bool ParseGraph(
	const TSharedPtr<FJsonObject>& InExtra,
	FMaterialGraph& OutGraph,
	TArray<FForgeDiagnostic>& OutDiagnostics,
	const FString& InFieldPath)
{
	if (!InExtra.IsValid())
	{
		return true;
	}

	bool bOk = true;
	const FString InputsPath = JoinPath(InFieldPath, FieldInputs);

	if (const TSharedPtr<FJsonObject>* InputsObject = nullptr; InExtra->TryGetObjectField(FieldInputs, InputsObject))
	{
		for (const TPair<UE::FSharedString, TSharedPtr<FJsonValue>>& Pair : (*InputsObject)->Values)
		{
			const FString Key(Pair.Key.ToView());
			const FString LinkPath = JoinPath(InputsPath, Key);

			const int32 Property = ResolveMaterialProperty(Key);
			if (Property == INDEX_NONE)
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Error,
					LinkPath,
					FString::Printf(
						TEXT("`%s` 不是材质属性名；写 `BaseColor` / `Roughness` 这类（EMaterialProperty 去掉 MP_ 前缀）"),
						*Key));
				bOk = false;
				continue;
			}

			// 键归一化：`BaseColor`、`MP_BaseColor`、`basecolor` 是同一个属性的三种写法，
			// 不归一化就会在判等时报出「仅配置 / 仅资产」的假差异。
			const FString NormalizedKey = MakeMaterialPropertyKey(Property);
			const bool bAlreadyWritten = OutGraph.PropertyInputs.ContainsByPredicate(
				[&NormalizedKey](const TPair<FString, FMaterialLink>& InExisting) { return InExisting.Key == NormalizedKey; });

			if (bAlreadyWritten)
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Error,
					LinkPath,
					FString::Printf(TEXT("材质属性 `%s` 写了两遍（换成 `%s` 看：两次指的是同一个属性）"), *Key, *NormalizedKey));
				bOk = false;
				continue;
			}

			FMaterialLink Link;
			if (!ParseLink(Pair.Value, Link, OutDiagnostics, LinkPath))
			{
				bOk = false;
				continue;
			}

			OutGraph.PropertyInputs.Emplace(NormalizedKey, MoveTemp(Link));
		}
	}
	else if (InExtra->HasField(FieldInputs))
	{
		OutDiagnostics.Emplace(EForgeSeverity::Error, InputsPath, TEXT("`inputs` 必须是对象：键是材质属性名，值是连线"));
		bOk = false;
	}

	const FString NodesPath = JoinPath(InFieldPath, FieldNodes);

	if (const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr; InExtra->TryGetArrayField(FieldNodes, NodesArray))
	{
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& NodeValue : *NodesArray)
		{
			const FString NodePath = FString::Printf(TEXT("%s[%d]"), *NodesPath, Index);
			++Index;

			const TSharedPtr<FJsonObject>* NodeObject = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObject) || !NodeObject->IsValid())
			{
				OutDiagnostics.Emplace(EForgeSeverity::Error, NodePath, TEXT("节点必须是 JSON 对象"));
				bOk = false;
				continue;
			}

			FMaterialNodeSpec Node;

			// 节点名是匹配键，但这里跟别的 kind 一样先只检查「有没有写」；重名在 ValidateGraph 报。
			if (!(*NodeObject)->TryGetStringField(TEXT("name"), Node.Name) || Node.Name.IsEmpty())
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Error,
					JoinPath(NodePath, TEXT("name")),
					TEXT("节点缺少 `name`；材质表达式在资产里没有名字，节点名就是这个 kind 的匹配键"));
				bOk = false;
				continue;
			}

			if (!(*NodeObject)->TryGetStringField(TEXT("class"), Node.ClassName) || Node.ClassName.IsEmpty())
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Error,
					JoinPath(NodePath, TEXT("class")),
					TEXT("节点缺少 `class`"));
				bOk = false;
				continue;
			}

			if (const TSharedPtr<FJsonObject>* PropsObject = nullptr; (*NodeObject)->TryGetObjectField(TEXT("props"), PropsObject))
			{
				Node.Props = *PropsObject;
			}

			if (const TSharedPtr<FJsonObject>* NodeInputs = nullptr; (*NodeObject)->TryGetObjectField(FieldInputs, NodeInputs))
			{
				for (const TPair<UE::FSharedString, TSharedPtr<FJsonValue>>& Pair : (*NodeInputs)->Values)
				{
					const FString PinName(Pair.Key.ToView());
					const FString LinkPath = JoinPath(JoinPath(NodePath, FieldInputs), PinName);

					FMaterialLink Link;
					if (!ParseLink(Pair.Value, Link, OutDiagnostics, LinkPath))
					{
						bOk = false;
						continue;
					}

					Node.Inputs.Emplace(PinName, MoveTemp(Link));
				}
			}

			// 认不出的键只警告：通用层（ForgeNode::Extra）的立场是「保留不丢」，
			// 但在这个自己解析的层级上，多半是笔误，说出来比默默忽略好。
			for (const TPair<UE::FSharedString, TSharedPtr<FJsonValue>>& Pair : (*NodeObject)->Values)
			{
				const FString Key(Pair.Key.ToView());
				if (Key != TEXT("name") && Key != TEXT("class") && Key != TEXT("props") && Key != FieldInputs)
				{
					OutDiagnostics.Emplace(
						EForgeSeverity::Warning,
						JoinPath(NodePath, Key),
						FString::Printf(TEXT("材质节点上不认识字段 `%s`，已忽略（可用：name / class / inputs / props）"), *Key));
				}
			}

			OutGraph.Nodes.Add(MoveTemp(Node));
		}
	}
	else if (InExtra->HasField(FieldNodes))
	{
		OutDiagnostics.Emplace(EForgeSeverity::Error, NodesPath, TEXT("`nodes` 必须是数组"));
		bOk = false;
	}

	return bOk;
}

void WriteGraph(const FMaterialGraph& InGraph, TSharedPtr<FJsonObject>& OutExtra)
{
	if (const TSharedPtr<FJsonObject> InputsObject = LinksToJson(InGraph.PropertyInputs))
	{
		OutExtra->SetObjectField(FieldInputs, InputsObject);
	}

	if (InGraph.Nodes.Num() == 0)
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> NodesArray;
	NodesArray.Reserve(InGraph.Nodes.Num());

	for (const FMaterialNodeSpec& Node : InGraph.Nodes)
	{
		const TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
		NodeObject->SetStringField(TEXT("class"), Node.ClassName);
		NodeObject->SetStringField(TEXT("name"), Node.Name);

		if (const TSharedPtr<FJsonObject> NodeInputs = LinksToJson(Node.Inputs))
		{
			NodeObject->SetObjectField(FieldInputs, NodeInputs);
		}

		if (Node.Props.IsValid() && Node.Props->Values.Num() > 0)
		{
			NodeObject->SetObjectField(TEXT("props"), Node.Props);
		}

		NodesArray.Add(MakeShared<FJsonValueObject>(NodeObject));
	}

	OutExtra->SetArrayField(FieldNodes, NodesArray);
}

bool ReadGraph(UMaterial* InMaterial, FMaterialGraph& OutGraph, TArray<FForgeDiagnostic>& OutDiagnostics)
{
	if (InMaterial == nullptr)
	{
		OutDiagnostics.Emplace(EForgeSeverity::Error, TEXT("asset"), TEXT("材质为空"));
		return false;
	}

	const int32 FirstDiagnostic = OutDiagnostics.Num();

	// 第一遍：给每个表达式一个临时名字（`@e3`），连线先按临时名字记，
	// 结构路径随后由 CanonicalizeNames 统一算——导出与判等共用同一套命名，不各写一份。
	TMap<const UMaterialExpression*, FString> TemporaryNames;
	const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = InMaterial->GetExpressions();
	TemporaryNames.Reserve(Expressions.Num());

	for (int32 Index = 0; Index < Expressions.Num(); ++Index)
	{
		const UMaterialExpression* Expression = Expressions[Index];
		if (Expression == nullptr)
		{
			continue;
		}

		if (IsUnsupportedExpression(Expression))
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				TEXT("root.nodes"),
				FString::Printf(
					TEXT("资产里有 `%s`（折叠图 Composite 或它的引脚基座）；这个契约表达不了折叠结构，")
					TEXT("请先在材质编辑器里解组，或先给管道加对应能力"),
					*Expression->GetClass()->GetName()));
			return false;
		}

		TemporaryNames.Add(Expression, FString::Printf(TEXT("@e%d"), Index));
	}

	const auto MakeLink = [&TemporaryNames](const UMaterialExpression* InSource, int32 InOutputIndex)
	{
		FMaterialLink Link;
		if (const FString* Name = TemporaryNames.Find(InSource))
		{
			Link.NodeName = *Name;
			Link.OutputName = GetOutputPinName(InSource, InOutputIndex);
		}
		return Link;
	};

	for (int32 Index = 0; Index < Expressions.Num(); ++Index)
	{
		const UMaterialExpression* Expression = Expressions[Index];
		if (Expression == nullptr || !TemporaryNames.Contains(Expression))
		{
			continue;
		}

		FMaterialNodeSpec Node;
		Node.Name = TemporaryNames[Expression];
		Node.ClassName = MakeClassKey(Expression->GetClass());

		ForgePropertyCodec::ExportDeltaProperties(Expression, Node.Props, OutDiagnostics, Node.Name);

		const int32 InputCount = Expression->CountInputs();
		for (int32 InputIndex = 0; InputIndex < InputCount; ++InputIndex)
		{
			const FExpressionInput* Input = Expression->GetInput(InputIndex);
			if (Input == nullptr || Input->Expression == nullptr)
			{
				continue;
			}

			Node.Inputs.Emplace(GetInputPinName(const_cast<UMaterialExpression*>(Expression), InputIndex), MakeLink(Input->Expression, Input->OutputIndex));
		}

		OutGraph.Nodes.Add(MoveTemp(Node));
	}

	// 材质属性输入：顺序按枚举值，两边就会得到同一个遍历次序。
	for (int32 PropertyIndex = 0; PropertyIndex < MP_MAX; ++PropertyIndex)
	{
		FExpressionInput* Input = InMaterial->GetExpressionInputForProperty(static_cast<EMaterialProperty>(PropertyIndex));
		if (Input == nullptr || Input->Expression == nullptr)
		{
			continue;
		}

		OutGraph.PropertyInputs.Emplace(MakeMaterialPropertyKey(PropertyIndex), MakeLink(Input->Expression, Input->OutputIndex));
	}

	SortPropertyInputs(OutGraph.PropertyInputs);

	// 属性导出的诊断先按临时名字写，规范化之后要把路径换成结构路径——
	// 报告里的路径要能直接对回配置，给个 `@e3` 等于没说。
	TArray<FString> TemporaryOrder;
	TemporaryOrder.Reserve(OutGraph.Nodes.Num());
	for (const FMaterialNodeSpec& Node : OutGraph.Nodes)
	{
		TemporaryOrder.Add(Node.Name);
	}

	CanonicalizeNames(OutGraph);

	TMap<FString, FString> FinalNameByTemporary;
	FinalNameByTemporary.Reserve(TemporaryOrder.Num());
	for (int32 Index = 0; Index < OutGraph.Nodes.Num(); ++Index)
	{
		// CanonicalizeNames 不改节点顺序，下标仍然一一对应。
		FinalNameByTemporary.Add(TemporaryOrder[Index], OutGraph.Nodes[Index].Name);
	}

	for (int32 Index = FirstDiagnostic; Index < OutDiagnostics.Num(); ++Index)
	{
		for (const TPair<FString, FString>& Pair : FinalNameByTemporary)
		{
			if (OutDiagnostics[Index].FieldPath.StartsWith(Pair.Key))
			{
				OutDiagnostics[Index].FieldPath = Pair.Value + OutDiagnostics[Index].FieldPath.RightChop(Pair.Key.Len());
				break;
			}
		}
	}

	return true;
}

void CanonicalizeNames(FMaterialGraph& InOutGraph)
{
	TMap<FString, int32> IndexByName;
	IndexNodes(InOutGraph, IndexByName);

	TArray<FString> NewNames;
	NewNames.SetNum(InOutGraph.Nodes.Num());

	TArray<bool> bNamed;
	bNamed.Init(false, InOutGraph.Nodes.Num());

	// 从材质属性输入出发，先到先得：一个表达式只认第一次到达它的那条路径，
	// 其余引用都是对同一个名字的重复引用（共享节点就是这么表达的）。
	TArray<TPair<FString, FMaterialLink>> OrderedInputs = InOutGraph.PropertyInputs;
	SortPropertyInputs(OrderedInputs);

	TArray<TPair<FString, FString>> Pending;  // 路径 → 节点名
	for (const TPair<FString, FMaterialLink>& Pair : OrderedInputs)
	{
		Pending.Emplace(Pair.Key, Pair.Value.NodeName);
	}

	while (Pending.Num() > 0)
	{
		const TPair<FString, FString> Current = Pending.Pop(EAllowShrinking::No);
		const int32* NodeIndex = IndexByName.Find(Current.Value);
		if (NodeIndex == nullptr || bNamed[*NodeIndex])
		{
			// 悬空引用由 ValidateGraph 负责报错，这里只管命名。
			continue;
		}

		bNamed[*NodeIndex] = true;
		NewNames[*NodeIndex] = Current.Key;

		TArray<TPair<FString, FMaterialLink>> SortedInputs;
		CollectSortedInputs(InOutGraph.Nodes[*NodeIndex], SortedInputs);
		for (const TPair<FString, FMaterialLink>& Pair : SortedInputs)
		{
			Pending.Emplace(JoinPath(Current.Key, Pair.Key), Pair.Value.NodeName);
		}
	}

	// 走不到的表达式：孤儿节点，按节点列表里的相对次序命名。
	int32 OrphanIndex = 0;
	for (int32 Index = 0; Index < InOutGraph.Nodes.Num(); ++Index)
	{
		if (!bNamed[Index])
		{
			bNamed[Index] = true;
			NewNames[Index] = FString::Printf(TEXT("%s%d"), IndexPinPrefix, OrphanIndex);
			++OrphanIndex;
		}
	}

	// 引用改写成新名字之后再改节点自己的名字——否则索引会自相矛盾。
	const auto Rename = [&NewNames, &IndexByName](FString& InOutNodeName)
	{
		if (const int32* Index = IndexByName.Find(InOutNodeName))
		{
			InOutNodeName = NewNames[*Index];
		}
	};

	for (TPair<FString, FMaterialLink>& Pair : InOutGraph.PropertyInputs)
	{
		Rename(Pair.Value.NodeName);
	}

	for (FMaterialNodeSpec& Node : InOutGraph.Nodes)
	{
		for (TPair<FString, FMaterialLink>& Pair : Node.Inputs)
		{
			Rename(Pair.Value.NodeName);
		}
	}

	for (int32 Index = 0; Index < InOutGraph.Nodes.Num(); ++Index)
	{
		InOutGraph.Nodes[Index].Name = NewNames[Index];
	}
}

void ValidateGraph(
	FMaterialGraph& InOutGraph,
	TArray<FForgeDiagnostic>& OutDiagnostics,
	const FString& InFieldPath)
{
	const FString NodesPath = JoinPath(InFieldPath, FieldNodes);

	/** 类都解析出来之后，引用的输出引脚名要靠**源节点**的类来查，所以顺手记一张表。 */
	TMap<FString, UMaterialExpression*> DefaultByName;

	TSet<FString> SeenNames;
	for (int32 Index = 0; Index < InOutGraph.Nodes.Num(); ++Index)
	{
		FMaterialNodeSpec& Node = InOutGraph.Nodes[Index];
		const FString NodePath = FString::Printf(TEXT("%s[%d]"), *NodesPath, Index);

		bool bAlreadyPresent = false;
		SeenNames.Add(Node.Name, &bAlreadyPresent);
		if (bAlreadyPresent)
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				JoinPath(NodePath, TEXT("name")),
				FString::Printf(TEXT("节点名 `%s` 重复；name 是导入时的匹配键，必须唯一"), *Node.Name));
		}

		FString ClassError;
		UClass* ExpressionClass = ResolveExpressionClass(Node.ClassName, ClassError);
		if (ExpressionClass == nullptr)
		{
			OutDiagnostics.Emplace(EForgeSeverity::Error, JoinPath(NodePath, TEXT("class")), ClassError);
			continue;
		}

		if (ExpressionClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				JoinPath(NodePath, TEXT("class")),
				FString::Printf(TEXT("`%s` 是抽象类，建不出节点"), *ExpressionClass->GetName()));
			continue;
		}

		if (ExpressionClass->IsChildOf(UMaterialExpressionComposite::StaticClass())
			|| ExpressionClass->IsChildOf(UMaterialExpressionPinBase::StaticClass())
			|| ExpressionClass->IsChildOf(UMaterialExpressionComment::StaticClass()))
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				JoinPath(NodePath, TEXT("class")),
				FString::Printf(
					TEXT("`%s` 表达不了：折叠图（Composite）与注释节点不在这个契约里"),
					*ExpressionClass->GetName()));
			continue;
		}

		ForgePropertyCodec::ValidateProperties(
			ExpressionClass,
			Node.Props,
			OutDiagnostics,
			JoinPath(NodePath, TEXT("props")));

		// 归一化：把显式写出的默认值删掉，与导出方向的 delta 形态对齐。
		ForgePropertyCodec::PruneDefaults(ExpressionClass, Node.Props);

		// 引脚名在不在：拿类的默认对象问（GetInputName 是实例方法，但只读默认引脚就够了）。
		UMaterialExpression* ClassDefault = Cast<UMaterialExpression>(ExpressionClass->GetDefaultObject());
		const int32 InputCount = (ClassDefault != nullptr) ? ClassDefault->CountInputs() : 0;
		DefaultByName.Add(Node.Name, ClassDefault);
		if (Node.Inputs.Num() > 0 && InputCount == 0)
		{
			// 引脚取决于属性或引用的资产（Custom 的输入数组、函数调用的函数签名），计划阶段问不出来。
			OutDiagnostics.Emplace(
				EForgeSeverity::Warning,
				JoinPath(NodePath, FieldInputs),
				FString::Printf(
					TEXT("`%s` 的输入引脚由属性或引用的资产决定，计划阶段无法校验：连不上会在导入时报错"),
					*ExpressionClass->GetName()));
			continue;
		}

		TArray<FString> ValidPins;
		ValidPins.Reserve(InputCount);
		for (int32 PinIndex = 0; PinIndex < InputCount; ++PinIndex)
		{
			ValidPins.Add(GetInputPinName(ClassDefault, PinIndex));
		}

		for (const TPair<FString, FMaterialLink>& Pair : Node.Inputs)
		{
			if (ResolveInputIndex(ClassDefault, Pair.Key) != INDEX_NONE)
			{
				continue;
			}

			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				JoinPath(JoinPath(NodePath, FieldInputs), Pair.Key),
				FString::Printf(
					TEXT("`%s` 上没有叫 `%s` 的输入引脚；可用：%s"),
					*ExpressionClass->GetName(),
					*Pair.Key,
					*FString::Join(ValidPins, TEXT(", "))));
		}
	}

	// 引用：悬空、输出引脚名、成环都在这里拦下。成环的图编译不过，而且遍历会陷进去。
	for (int32 Index = 0; Index < InOutGraph.Nodes.Num(); ++Index)
	{
		const FMaterialNodeSpec& Node = InOutGraph.Nodes[Index];
		for (const TPair<FString, FMaterialLink>& Pair : Node.Inputs)
		{
			const FString LinkPath = JoinPath(JoinPath(FString::Printf(TEXT("%s[%d]"), *NodesPath, Index), FieldInputs), Pair.Key);

			if (!SeenNames.Contains(Pair.Value.NodeName))
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Error,
					LinkPath,
					FString::Printf(
						TEXT("引用了不存在的节点 `%s`（材质图没有顺序，节点必须先在 nodes 里定义）"),
						*Pair.Value.NodeName));
				continue;
			}

			// 输出引脚名由**源节点**的类决定，所以这里查它的默认对象。
			if (const UMaterialExpression* const* SourceDefault = DefaultByName.Find(Pair.Value.NodeName))
			{
				if (ResolveOutputIndex(const_cast<UMaterialExpression*>(*SourceDefault), Pair.Value.OutputName) == INDEX_NONE)
				{
					OutDiagnostics.Emplace(
						EForgeSeverity::Error,
						JoinPath(LinkPath, FieldOutput),
						FString::Printf(
							TEXT("节点 `%s` 上没有叫 `%s` 的输出引脚（留空表示第一个输出；多输出节点写 R/G/B/A 或它自己的引脚名）"),
							*Pair.Value.NodeName,
							*Pair.Value.OutputName));
				}
			}
		}
	}

	const TArray<FString> ReferencedNodes = [&InOutGraph]
	{
		TArray<FString> Names;
		Names.Reserve(InOutGraph.PropertyInputs.Num());
		for (const TPair<FString, FMaterialLink>& Pair : InOutGraph.PropertyInputs)
		{
			Names.Add(Pair.Value.NodeName);
		}
		return Names;
	}();

	for (const FString& NodeName : ReferencedNodes)
	{
		if (!SeenNames.Contains(NodeName))
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				JoinPath(InFieldPath, FieldInputs),
				FString::Printf(TEXT("材质属性连到了不存在的节点 `%s`"), *NodeName));
		}
	}

	// 环检测：深度优先，遇到「正在访问」的节点就是环。
	TMap<FString, int32> IndexByName;
	IndexNodes(InOutGraph, IndexByName);

	TArray<int32> States;
	States.Init(0, InOutGraph.Nodes.Num());

	TArray<FString> Stack;

	// 用 TFunction 而不是把泛型 lambda 绑成 TFunctionRef：后者的 lambda 临时量在表达式结束时就死了，
	// 递归调用的那一刻它已经悬空。
	TFunction<bool(int32)> VisitNode = [&](int32 InIndex) -> bool
	{
		if (States[InIndex] == 1)
		{
			Stack.Add(InOutGraph.Nodes[InIndex].Name);
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				JoinPath(InFieldPath, FieldNodes),
				FString::Printf(TEXT("连线成环：%s；材质图必须是有向无环图"), *FString::Join(Stack, TEXT(" → "))));
			Stack.Pop(EAllowShrinking::No);
			return false;
		}

		if (States[InIndex] == 2)
		{
			return true;
		}

		States[InIndex] = 1;
		Stack.Push(InOutGraph.Nodes[InIndex].Name);

		bool bOk = true;
		for (const TPair<FString, FMaterialLink>& Pair : InOutGraph.Nodes[InIndex].Inputs)
		{
			if (const int32* Target = IndexByName.Find(Pair.Value.NodeName))
			{
				bOk &= VisitNode(*Target);
			}
		}

		Stack.Pop(EAllowShrinking::No);
		States[InIndex] = 2;
		return bOk;
	};

	for (int32 Index = 0; Index < InOutGraph.Nodes.Num(); ++Index)
	{
		if (States[Index] == 0)
		{
			VisitNode(Index);
		}
	}
}

} // namespace MaterialGraphCodec
