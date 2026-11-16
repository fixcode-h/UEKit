// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Anim/AnimBlueprintForge.h"
#include "Core/ForgeClassResolver.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"

#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "AnimationGraph.h"
#include "AnimationStateGraph.h"
#include "AnimationStateMachineGraph.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace AnimBlueprintForge
{

namespace
{
	/** 引擎给转移的默认混合时长（`UAnimStateTransitionNode` 构造值），也是配置的规范缺省。 */
	constexpr float DefaultCrossfade = 0.2f;

	// ---- 中间表示 ----

	struct FStateSpec
	{
		FString Name;
		FString Sequence;
		bool bLoop = true;
		float PlayRate = 1.0f;
	};

	struct FTransitionSpec
	{
		FString From;
		FString To;
		float Crossfade = DefaultCrossfade;
		bool bAutomaticRule = true;
	};

	struct FStateMachineSpec
	{
		FString Name;
		TArray<FStateSpec> States;
		TArray<FTransitionSpec> Transitions;
		FString Entry;
	};

	// ---- 小工具 ----

	/** 找指定方向上第一个还没连线的引脚（引脚名因节点类型而异，按方向找最稳）。 */
	UEdGraphPin* FindFreePin(const UEdGraphNode* InNode, EEdGraphPinDirection InDirection)
	{
		if (InNode == nullptr)
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : InNode->Pins)
		{
			if (Pin != nullptr && Pin->Direction == InDirection && Pin->LinkedTo.Num() == 0)
			{
				return Pin;
			}
		}

		return nullptr;
	}

	UEdGraphPin* FindAnyPin(const UEdGraphNode* InNode, EEdGraphPinDirection InDirection)
	{
		if (InNode == nullptr)
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : InNode->Pins)
		{
			if (Pin != nullptr && Pin->Direction == InDirection)
			{
				return Pin;
			}
		}

		return nullptr;
	}

	/** 状态的名字就是它内部图的名字——这是引擎自己的约定（`OnRenameNode` 直接改名内部图）。 */
	FString GetStateName(const UAnimStateNodeBase* InNode)
	{
		if (const UAnimStateNode* StateNode = Cast<UAnimStateNode>(InNode))
		{
			if (StateNode->BoundGraph != nullptr)
			{
				return StateNode->BoundGraph->GetName();
			}
		}

		return InNode != nullptr ? InNode->GetStateName() : FString();
	}

	UEdGraph* FindAnimGraph(const UAnimBlueprint* InBlueprint)
	{
		if (InBlueprint == nullptr)
		{
			return nullptr;
		}

		for (UEdGraph* Graph : InBlueprint->FunctionGraphs)
		{
			if (Cast<UAnimationGraph>(Graph) != nullptr)
			{
				return Graph;
			}
		}

		return nullptr;
	}

	UAnimGraphNode_Root* FindAnimGraphRoot(const UEdGraph* InAnimGraph)
	{
		if (InAnimGraph == nullptr)
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : InAnimGraph->Nodes)
		{
			if (UAnimGraphNode_Root* RootNode = Cast<UAnimGraphNode_Root>(Node))
			{
				return RootNode;
			}
		}

		return nullptr;
	}

	bool SaveAssetPackage(UObject* InAsset, FString& OutError)
	{
		UPackage* Package = InAsset->GetOutermost();
		Package->MarkPackageDirty();

		const FString PackageFileName = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()));

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, InAsset, *PackageFileName, SaveArgs))
		{
			OutError = FString::Printf(TEXT("保存资产失败：%s"), *PackageFileName);
			return false;
		}

		return true;
	}

	// ---- 解析：配置文档 → 中间表示 ----

	bool ReadStateMachineSpec(
		const FForgeNode& InNode,
		FStateMachineSpec& OutSpec,
		FForgeReport& OutReport,
		const FString& InPath)
	{
		if (InNode.Name.IsEmpty())
		{
			OutReport.AddError(FString::Printf(TEXT("%s.name"), *InPath), TEXT("状态机缺少 name"));
			return false;
		}

		OutSpec.Name = InNode.Name;

		for (int32 Index = 0; Index < InNode.Children.Num(); ++Index)
		{
			const FForgeNode& Child = InNode.Children[Index];
			const FString ChildPath = FString::Printf(TEXT("%s.children[%d]"), *InPath, Index);

			if (Child.ClassName != StateClass)
			{
				OutReport.AddError(
					FString::Printf(TEXT("%s.class"), *ChildPath),
					FString::Printf(TEXT("状态机下只认 class = %s，收到 `%s`"), StateClass, *Child.ClassName));
				return false;
			}

			if (Child.Name.IsEmpty())
			{
				OutReport.AddError(FString::Printf(TEXT("%s.name"), *ChildPath), TEXT("状态缺少 name"));
				return false;
			}

			for (const FStateSpec& Existing : OutSpec.States)
			{
				if (Existing.Name == Child.Name)
				{
					OutReport.AddError(
						FString::Printf(TEXT("%s.name"), *ChildPath),
						FString::Printf(TEXT("状态名 `%s` 在同一个状态机里重复"), *Child.Name));
					return false;
				}
			}

			FStateSpec StateSpec;
			StateSpec.Name = Child.Name;

			if (Child.Props.IsValid())
			{
				Child.Props->TryGetStringField(TEXT("sequence"), StateSpec.Sequence);
				Child.Props->TryGetBoolField(TEXT("loop"), StateSpec.bLoop);
				Child.Props->TryGetNumberField(TEXT("playRate"), StateSpec.PlayRate);
			}

			OutSpec.States.Add(MoveTemp(StateSpec));
		}

		if (OutSpec.States.Num() == 0)
		{
			OutReport.AddError(FString::Printf(TEXT("%s.children"), *InPath), TEXT("状态机至少要有一个状态"));
			return false;
		}

		// 入口：显式写就用写的，否则取第一个状态（导出时总会显式写出来，所以往返是稳定的）。
		OutSpec.Entry = InNode.Children[0].Name;
		if (InNode.Extra.IsValid())
		{
			FString EntryField;
			if (InNode.Extra->TryGetStringField(TEXT("entry"), EntryField) && !EntryField.IsEmpty())
			{
				OutSpec.Entry = EntryField;
			}
		}

		bool bHasEntry = false;
		for (const FStateSpec& StateSpec : OutSpec.States)
		{
			bHasEntry |= (StateSpec.Name == OutSpec.Entry);
		}
		if (!bHasEntry)
		{
			OutReport.AddError(
				FString::Printf(TEXT("%s.extra.entry"), *InPath),
				FString::Printf(TEXT("入口状态 `%s` 不在状态列表里"), *OutSpec.Entry));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* TransitionArray = nullptr;
		if (InNode.Extra.IsValid() && InNode.Extra->TryGetArrayField(TEXT("transitions"), TransitionArray))
		{
			for (int32 Index = 0; Index < TransitionArray->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject>* TransitionObject = nullptr;
				const FString TransitionPath = FString::Printf(TEXT("%s.extra.transitions[%d]"), *InPath, Index);

				if (!(*TransitionArray)[Index].IsValid() || !(*TransitionArray)[Index]->TryGetObject(TransitionObject))
				{
					OutReport.AddError(TransitionPath, TEXT("转移必须是一个对象"));
					return false;
				}

				FTransitionSpec TransitionSpec;
				if (!(*TransitionObject)->TryGetStringField(TEXT("from"), TransitionSpec.From)
					|| !(*TransitionObject)->TryGetStringField(TEXT("to"), TransitionSpec.To))
				{
					OutReport.AddError(TransitionPath, TEXT("转移缺少 from / to"));
					return false;
				}

				(*TransitionObject)->TryGetNumberField(TEXT("crossfade"), TransitionSpec.Crossfade);
				(*TransitionObject)->TryGetBoolField(TEXT("autoRule"), TransitionSpec.bAutomaticRule);

				bool bFromKnown = false;
				bool bToKnown = false;
				for (const FStateSpec& StateSpec : OutSpec.States)
				{
					bFromKnown |= (StateSpec.Name == TransitionSpec.From);
					bToKnown |= (StateSpec.Name == TransitionSpec.To);
				}

				if (!bFromKnown || !bToKnown)
				{
					OutReport.AddError(
						TransitionPath,
						FString::Printf(
							TEXT("转移端点 `%s` → `%s` 有状态不在列表里"),
							*TransitionSpec.From,
							*TransitionSpec.To));
					return false;
				}

				OutSpec.Transitions.Add(MoveTemp(TransitionSpec));
			}
		}

		return true;
	}

	bool ReadDocumentSpec(
		const FForgeDocument& InDocument,
		TArray<FStateMachineSpec>& OutSpecs,
		FString& OutSkeletonPath,
		FForgeReport& OutReport)
	{
		if (InDocument.Root.ClassName != AnimGraphRootClass)
		{
			OutReport.AddError(
				TEXT("root.class"),
				FString::Printf(
					TEXT("动画蓝图的根占位节点 class 必须是 `%s`，收到 `%s`"),
					AnimGraphRootClass,
					*InDocument.Root.ClassName));
			return false;
		}

		if (InDocument.Root.Children.Num() == 0)
		{
			OutReport.AddError(TEXT("root.children"), TEXT("AnimGraph 下至少要有一个状态机"));
			return false;
		}

		for (int32 Index = 0; Index < InDocument.Root.Children.Num(); ++Index)
		{
			const FForgeNode& Child = InDocument.Root.Children[Index];
			const FString ChildPath = FString::Printf(TEXT("root.children[%d]"), Index);

			if (Child.ClassName != StateMachineClass)
			{
				OutReport.AddError(
					FString::Printf(TEXT("%s.class"), *ChildPath),
					FString::Printf(TEXT("AnimGraph 下目前只认 class = %s，收到 `%s`"), StateMachineClass, *Child.ClassName));
				return false;
			}

			for (const FStateMachineSpec& Existing : OutSpecs)
			{
				if (Existing.Name == Child.Name)
				{
					OutReport.AddError(
						FString::Printf(TEXT("%s.name"), *ChildPath),
						FString::Printf(TEXT("状态机名 `%s` 重复"), *Child.Name));
					return false;
				}
			}

			FStateMachineSpec Spec;
			if (!ReadStateMachineSpec(Child, Spec, OutReport, ChildPath))
			{
				return false;
			}

			OutSpecs.Add(MoveTemp(Spec));
		}

		if (InDocument.Extra.IsValid())
		{
			InDocument.Extra->TryGetStringField(TEXT("skeleton"), OutSkeletonPath);
		}

		return true;
	}

	// ---- 归一化：中间表示 → 规范的配置节点树 ----

	FForgeNode BuildStateNode(const FStateSpec& InSpec)
	{
		FForgeNode Node;
		Node.ClassName = StateClass;
		Node.Name = InSpec.Name;

		// 只写与缺省不同的部分，导出与导入因此收敛到同一个形态。
		if (!InSpec.Sequence.IsEmpty() || !InSpec.bLoop || !FMath::IsNearlyEqual(InSpec.PlayRate, 1.0f))
		{
			Node.Props = MakeShared<FJsonObject>();
			if (!InSpec.Sequence.IsEmpty())
			{
				Node.Props->SetStringField(TEXT("sequence"), InSpec.Sequence);
			}
			if (!InSpec.bLoop)
			{
				Node.Props->SetBoolField(TEXT("loop"), false);
			}
			if (!FMath::IsNearlyEqual(InSpec.PlayRate, 1.0f))
			{
				Node.Props->SetNumberField(TEXT("playRate"), InSpec.PlayRate);
			}
		}

		return Node;
	}

	FForgeNode BuildRootNode(const TArray<FStateMachineSpec>& InSpecs)
	{
		FForgeNode Root;
		Root.ClassName = AnimGraphRootClass;
		Root.Name = AnimGraphRootClass;

		for (const FStateMachineSpec& Spec : InSpecs)
		{
			FForgeNode StateMachineNode;
			StateMachineNode.ClassName = StateMachineClass;
			StateMachineNode.Name = Spec.Name;

			for (const FStateSpec& StateSpec : Spec.States)
			{
				StateMachineNode.Children.Add(BuildStateNode(StateSpec));
			}

			StateMachineNode.Extra = MakeShared<FJsonObject>();
			StateMachineNode.Extra->SetStringField(TEXT("entry"), Spec.Entry);

			if (Spec.Transitions.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> TransitionArray;
				for (const FTransitionSpec& TransitionSpec : Spec.Transitions)
				{
					TSharedPtr<FJsonObject> TransitionObject = MakeShared<FJsonObject>();
					TransitionObject->SetStringField(TEXT("from"), TransitionSpec.From);
					TransitionObject->SetStringField(TEXT("to"), TransitionSpec.To);
					if (!FMath::IsNearlyEqual(TransitionSpec.Crossfade, DefaultCrossfade))
					{
						TransitionObject->SetNumberField(TEXT("crossfade"), TransitionSpec.Crossfade);
					}
					if (!TransitionSpec.bAutomaticRule)
					{
						TransitionObject->SetBoolField(TEXT("autoRule"), false);
					}
					TransitionArray.Add(MakeShared<FJsonValueObject>(MoveTemp(TransitionObject)));
				}
				StateMachineNode.Extra->SetArrayField(TEXT("transitions"), TransitionArray);
			}

			Root.Children.Add(MoveTemp(StateMachineNode));
		}

		return Root;
	}

	// ---- 读图：状态机 → 中间表示 ----

	void ReadStateFromNode(const UAnimStateNode* InStateNode, FStateSpec& OutSpec)
	{
		OutSpec.Name = GetStateName(InStateNode);

		const UEdGraph* StateGraph = InStateNode != nullptr ? InStateNode->BoundGraph : nullptr;
		if (StateGraph == nullptr)
		{
			return;
		}

		for (UEdGraphNode* Node : StateGraph->Nodes)
		{
			const UAnimGraphNode_SequencePlayer* Player = Cast<UAnimGraphNode_SequencePlayer>(Node);
			if (Player == nullptr)
			{
				continue;
			}

			OutSpec.Sequence = Player->Node.GetSequence() != nullptr
				? Player->Node.GetSequence()->GetPathName()
				: FString();
			OutSpec.bLoop = Player->Node.IsLooping();
			OutSpec.PlayRate = Player->Node.GetPlayRate();
			break;
		}
	}

	void ReadStateMachineFromNode(const UAnimGraphNode_StateMachineBase* InStateMachineNode, FStateMachineSpec& OutSpec)
	{
		const UAnimationStateMachineGraph* StateMachineGraph = InStateMachineNode != nullptr
			? InStateMachineNode->EditorStateMachineGraph
			: nullptr;
		if (StateMachineGraph == nullptr)
		{
			return;
		}

		OutSpec.Name = StateMachineGraph->GetName();

		for (UEdGraphNode* Node : StateMachineGraph->Nodes)
		{
			if (const UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
			{
				FStateSpec StateSpec;
				ReadStateFromNode(StateNode, StateSpec);
				OutSpec.States.Add(MoveTemp(StateSpec));
			}
			else if (const UAnimStateTransitionNode* TransitionNode = Cast<UAnimStateTransitionNode>(Node))
			{
				FTransitionSpec TransitionSpec;
				TransitionSpec.From = GetStateName(TransitionNode->GetPreviousState());
				TransitionSpec.To = GetStateName(TransitionNode->GetNextState());
				TransitionSpec.Crossfade = TransitionNode->CrossfadeDuration;
				TransitionSpec.bAutomaticRule = TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState;

				if (!TransitionSpec.From.IsEmpty() && !TransitionSpec.To.IsEmpty())
				{
					OutSpec.Transitions.Add(MoveTemp(TransitionSpec));
				}
			}
		}

		// 入口 = 入口节点连出去的那个状态。
		if (StateMachineGraph->EntryNode != nullptr)
		{
			if (const UEdGraphPin* EntryPin = FindAnyPin(StateMachineGraph->EntryNode, EGPD_Output))
			{
				if (EntryPin->LinkedTo.Num() > 0)
				{
					OutSpec.Entry = GetStateName(Cast<UAnimStateNodeBase>(EntryPin->LinkedTo[0]->GetOwningNode()));
				}
			}
		}

		// 图里节点的顺序由编辑历史决定，不保证与作者写入的顺序一致；排序后往返才是稳定的。
		OutSpec.States.Sort([](const FStateSpec& A, const FStateSpec& B) { return A.Name < B.Name; });
		OutSpec.Transitions.Sort([](const FTransitionSpec& A, const FTransitionSpec& B)
		{
			return A.From == B.From ? A.To < B.To : A.From < B.From;
		});
	}

	void ReadSpecsFromBlueprint(const UAnimBlueprint* InBlueprint, TArray<FStateMachineSpec>& OutSpecs)
	{
		const UEdGraph* AnimGraph = FindAnimGraph(InBlueprint);
		if (AnimGraph == nullptr)
		{
			return;
		}

		for (UEdGraphNode* Node : AnimGraph->Nodes)
		{
			const UAnimGraphNode_StateMachineBase* StateMachineNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
			if (StateMachineNode == nullptr)
			{
				continue;
			}

			FStateMachineSpec Spec;
			ReadStateMachineFromNode(StateMachineNode, Spec);
			if (!Spec.Name.IsEmpty())
			{
				OutSpecs.Add(MoveTemp(Spec));
			}
		}

		OutSpecs.Sort([](const FStateMachineSpec& A, const FStateMachineSpec& B) { return A.Name < B.Name; });
	}

	// ---- 写图 ----

	/**
	 * 建一个状态节点并填好它内部图里的动画序列。
	 *
	 * 状态内部图（`UAnimationStateGraph`）由 `PostPlacedNewNode` 建好并自带一个结果节点，
	 * 这里只需要再放一个序列播放器并连到结果上。
	 */
	UAnimStateNode* CreateStateNode(
		UAnimationStateMachineGraph* InStateMachineGraph,
		const FStateSpec& InSpec,
		UAnimSequenceBase* InSequence,
		int32 InIndex,
		FForgeReport& OutReport,
		const FString& InPath)
	{
		UAnimStateNode* StateNode = NewObject<UAnimStateNode>(InStateMachineGraph);
		StateNode->CreateNewGuid();
		StateNode->PostPlacedNewNode();
		StateNode->AllocateDefaultPins();
		InStateMachineGraph->AddNode(StateNode, /*bSelectNewNode=*/false, /*bIsUserAction=*/false);

		StateNode->NodePosX = InIndex * 360;
		StateNode->NodePosY = 0;

		if (StateNode->BoundGraph == nullptr)
		{
			OutReport.AddError(InPath, FString::Printf(TEXT("状态 `%s` 的内部图没有建出来"), *InSpec.Name));
			return nullptr;
		}

		FBlueprintEditorUtils::RenameGraph(StateNode->BoundGraph, InSpec.Name);
		if (StateNode->BoundGraph->GetName() != InSpec.Name)
		{
			OutReport.AddWarning(
				FString::Printf(TEXT("%s.name"), *InPath),
				FString::Printf(
					TEXT("状态名 `%s` 已被占用，实际写成 `%s`"),
					*InSpec.Name,
					*StateNode->BoundGraph->GetName()));
		}

		if (InSequence == nullptr)
		{
			return StateNode;
		}

		UEdGraph* StateGraph = StateNode->BoundGraph;
		UAnimationStateGraph* TypedGraph = Cast<UAnimationStateGraph>(StateGraph);
		UEdGraphNode* ResultNode = TypedGraph != nullptr ? TypedGraph->GetResultNode() : nullptr;

		UAnimGraphNode_SequencePlayer* PlayerNode = NewObject<UAnimGraphNode_SequencePlayer>(StateGraph);
		PlayerNode->CreateNewGuid();
		PlayerNode->PostPlacedNewNode();
		PlayerNode->AllocateDefaultPins();
		// 序列、循环、播放速率在 FAnimNode_SequencePlayer 上是 protected，只能走它公开的 setter。
		PlayerNode->Node.SetSequence(InSequence);
		PlayerNode->Node.SetLoopAnimation(InSpec.bLoop);
		PlayerNode->Node.SetPlayRate(InSpec.PlayRate);
		PlayerNode->NodePosX = -320;
		PlayerNode->NodePosY = 0;
		StateGraph->AddNode(PlayerNode, /*bSelectNewNode=*/false, /*bIsUserAction=*/false);

		UEdGraphPin* FromPin = FindFreePin(PlayerNode, EGPD_Output);
		UEdGraphPin* ToPin = FindFreePin(ResultNode, EGPD_Input);
		if (FromPin != nullptr && ToPin != nullptr)
		{
			StateGraph->GetSchema()->TryCreateConnection(FromPin, ToPin);
		}
		else
		{
			OutReport.AddWarning(
				InPath,
				FString::Printf(TEXT("状态 `%s` 的序列播放器没能连到状态输出"), *InSpec.Name));
		}

		return StateNode;
	}

	void CreateTransitionNode(
		UAnimationStateMachineGraph* InStateMachineGraph,
		UAnimStateNode* InFromState,
		UAnimStateNode* InToState,
		const FTransitionSpec& InSpec,
		int32 InIndex)
	{
		UAnimStateTransitionNode* TransitionNode = NewObject<UAnimStateTransitionNode>(InStateMachineGraph);
		TransitionNode->CreateNewGuid();
		TransitionNode->PostPlacedNewNode();
		TransitionNode->AllocateDefaultPins();
		InStateMachineGraph->AddNode(TransitionNode, /*bSelectNewNode=*/false, /*bIsUserAction=*/false);

		TransitionNode->NodePosX = InIndex * 360 + 120;
		TransitionNode->NodePosY = 180;

		// 转移的端点关系完全由引脚连接表达（引擎没有独立的转移数组）。
		TransitionNode->CreateConnections(InFromState, InToState);
		TransitionNode->CrossfadeDuration = InSpec.Crossfade;
		TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState = InSpec.bAutomaticRule;
	}

	/** 清掉状态机里现有的状态与转移（保留入口节点），为重建让路。 */
	void ClearStateMachine(UBlueprint* InBlueprint, UAnimationStateMachineGraph* InStateMachineGraph)
	{
		TArray<UEdGraphNode*> ToRemove;
		for (UEdGraphNode* Node : InStateMachineGraph->Nodes)
		{
			if (Node != nullptr && Node->IsA(UAnimStateNodeBase::StaticClass()))
			{
				ToRemove.Add(Node);
			}
		}

		for (UEdGraphNode* Node : ToRemove)
		{
			// 删状态会连带删掉它的内部图，内部会重编译；这里不再叠加一次。
			FBlueprintEditorUtils::RemoveNode(InBlueprint, Node, /*bDontRecompile=*/true);
		}
	}

	bool SyncStateMachine(
		UBlueprint* InBlueprint,
		UAnimGraphNode_StateMachineBase* InStateMachineNode,
		const FStateMachineSpec& InSpec,
		FForgeReport& OutReport,
		const FString& InPath)
	{
		UAnimationStateMachineGraph* StateMachineGraph = InStateMachineNode->EditorStateMachineGraph;
		if (StateMachineGraph == nullptr)
		{
			OutReport.AddError(InPath, TEXT("状态机节点没有内部图"));
			return false;
		}

		// 状态机整体重建而不是增量改：转移是图连接，增删状态会牵动一串引脚的接续，
		// 增量算法比"重画一遍"更容易出错，而配置本来就是唯一真源。
		ClearStateMachine(InBlueprint, StateMachineGraph);

		TMap<FString, UAnimStateNode*> StateNodes;

		for (int32 Index = 0; Index < InSpec.States.Num(); ++Index)
		{
			const FStateSpec& StateSpec = InSpec.States[Index];
			const FString StatePath = FString::Printf(TEXT("%s/%s"), *InPath, *StateSpec.Name);

			UAnimSequenceBase* Sequence = nullptr;
			if (!StateSpec.Sequence.IsEmpty())
			{
				Sequence = LoadObject<UAnimSequenceBase>(nullptr, *StateSpec.Sequence);
				if (Sequence == nullptr)
				{
					OutReport.AddError(
						FString::Printf(TEXT("%s.props.sequence"), *StatePath),
						FString::Printf(TEXT("找不到动画序列 `%s`"), *StateSpec.Sequence));
					return false;
				}
			}

			UAnimStateNode* StateNode = CreateStateNode(
				StateMachineGraph,
				StateSpec,
				Sequence,
				Index,
				OutReport,
				StatePath);
			if (StateNode == nullptr)
			{
				return false;
			}

			StateNodes.Add(StateSpec.Name, StateNode);
		}

		for (int32 Index = 0; Index < InSpec.Transitions.Num(); ++Index)
		{
			const FTransitionSpec& TransitionSpec = InSpec.Transitions[Index];
			UAnimStateNode** FromState = StateNodes.Find(TransitionSpec.From);
			UAnimStateNode** ToState = StateNodes.Find(TransitionSpec.To);

			if (FromState == nullptr || ToState == nullptr)
			{
				OutReport.AddError(
					FString::Printf(TEXT("%s.extra.transitions[%d]"), *InPath, Index),
					FString::Printf(
						TEXT("转移端点 `%s` → `%s` 建不出来"),
						*TransitionSpec.From,
						*TransitionSpec.To));
				return false;
			}

			CreateTransitionNode(StateMachineGraph, *FromState, *ToState, TransitionSpec, Index);
		}

		// 入口连到入口状态：状态机编译时它决定默认状态。
		UAnimStateNode** EntryState = StateNodes.Find(InSpec.Entry);
		if (EntryState == nullptr)
		{
			OutReport.AddError(
				FString::Printf(TEXT("%s.extra.entry"), *InPath),
				FString::Printf(TEXT("入口状态 `%s` 建不出来"), *InSpec.Entry));
			return false;
		}

		if (StateMachineGraph->EntryNode == nullptr)
		{
			OutReport.AddError(InPath, TEXT("状态机图缺少入口节点"));
			return false;
		}

		if (UEdGraphPin* EntryPin = FindAnyPin(StateMachineGraph->EntryNode, EGPD_Output))
		{
			EntryPin->BreakAllPinLinks();
			if (UEdGraphPin* StateInPin = FindAnyPin(*EntryState, EGPD_Input))
			{
				StateMachineGraph->GetSchema()->TryCreateConnection(EntryPin, StateInPin);
			}
		}

		return true;
	}
}

bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(LoadObject<UObject>(nullptr, *InAssetPath));
	if (Blueprint == nullptr)
	{
		OutReport.AddError(
			TEXT("asset"),
			FString::Printf(TEXT("找不到动画蓝图：%s（非动画蓝图请换对应的 kind）"), *InAssetPath));
		return false;
	}

	TArray<FStateMachineSpec> Specs;
	ReadSpecsFromBlueprint(Blueprint, Specs);

	if (Specs.Num() == 0)
	{
		OutReport.AddWarning(TEXT("root.children"), TEXT("这个动画蓝图里没有状态机，导出结果为空"));
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;
	OutDocument.Parent = Blueprint->ParentClass != nullptr ? Blueprint->ParentClass->GetPathName() : FString();
	OutDocument.Root = BuildRootNode(Specs);

	if (Blueprint->TargetSkeleton != nullptr)
	{
		if (!OutDocument.Extra.IsValid())
		{
			OutDocument.Extra = MakeShared<FJsonObject>();
		}
		OutDocument.Extra->SetStringField(TEXT("skeleton"), Blueprint->TargetSkeleton->GetPathName());
	}

	OutReport.bSucceeded = true;
	return true;
}

bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport)
{
	OutReport.bDryRun = bDryRun;
	OutReport.SourcePath = InDocument.SourcePath;
	OutReport.AssetPath = InDocument.Asset;

	TArray<FStateMachineSpec> Specs;
	FString SkeletonPath;
	if (!ReadDocumentSpec(InDocument, Specs, SkeletonPath, OutReport))
	{
		return false;
	}

	const FString ParentPath = InDocument.Parent.IsEmpty()
		? FString(TEXT("/Script/Engine.AnimInstance"))
		: InDocument.Parent;

	FString ParentError;
	UClass* ParentClass = FForgeClassResolver::Resolve(ParentPath, UAnimInstance::StaticClass(), ParentError);
	if (ParentClass == nullptr)
	{
		OutReport.AddError(TEXT("parent"), ParentError);
		return false;
	}

	USkeleton* Skeleton = nullptr;
	if (!SkeletonPath.IsEmpty())
	{
		Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
		if (Skeleton == nullptr)
		{
			OutReport.AddError(TEXT("skeleton"), FString::Printf(TEXT("找不到骨架 `%s`"), *SkeletonPath));
			return false;
		}
	}

	UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(LoadObject<UObject>(nullptr, *InDocument.Asset));

	// 计划：把目标形态与现状都归一到同一个中间表示后整体比对。
	const FForgeNode TargetRoot = BuildRootNode(Specs);
	bool bSameAsCurrent = false;

	if (Blueprint != nullptr)
	{
		TArray<FStateMachineSpec> CurrentSpecs;
		ReadSpecsFromBlueprint(Blueprint, CurrentSpecs);

		const FForgeNode CurrentRoot = BuildRootNode(CurrentSpecs);
		bSameAsCurrent = ForgeJsonText::ToCanonicalString(ForgeDoc::NodeToJson(TargetRoot))
			== ForgeJsonText::ToCanonicalString(ForgeDoc::NodeToJson(CurrentRoot));

		if (Skeleton != nullptr && Blueprint->TargetSkeleton != Skeleton)
		{
			bSameAsCurrent = false;
		}
	}

	FForgePlanEntry PlanEntry;
	PlanEntry.NodePath = TEXT("root");
	PlanEntry.NodeName = AnimGraphRootClass;
	PlanEntry.Action = (Blueprint == nullptr)
		? EForgeAction::Create
		: (bSameAsCurrent ? EForgeAction::Unchanged : EForgeAction::Update);
	PlanEntry.Detail = FString::Printf(TEXT("状态机 %d 个"), Specs.Num());
	OutReport.Actions.Add(MoveTemp(PlanEntry));
	OutReport.CountActions();

	if (bDryRun)
	{
		OutReport.bSucceeded = !OutReport.HasErrors();
		return OutReport.bSucceeded;
	}

	if (OutReport.HasErrors())
	{
		return false;
	}

	if (Skeleton == nullptr)
	{
		OutReport.AddError(
			TEXT("skeleton"),
			TEXT("动画蓝图必须有 targetSkeleton，否则编译不出可用的动画实例"));
		return false;
	}

	// 状态机与现状一致：不动资产，也不写盘。计划里那条 Unchanged 就是这个意思——
	// 无意义的保存会改文件时间戳、把「哪里变了」这件事弄失真。
	if (bSameAsCurrent)
	{
		OutReport.bSucceeded = true;
		return true;
	}

	if (Blueprint == nullptr)
	{
		UPackage* Package = CreatePackage(*InDocument.Asset);
		if (Package == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("无法创建包：%s"), *InDocument.Asset));
			return false;
		}

		FString PackagePath;
		FString AssetName;
		InDocument.Asset.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (AssetName.IsEmpty())
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(
				TEXT("资产路径 `%s` 不合法，应形如 /Game/Animation/ABP_Xxx"), *InDocument.Asset));
			return false;
		}

		Blueprint = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UAnimBlueprint::StaticClass(),
			UAnimBlueprintGeneratedClass::StaticClass()));

		if (Blueprint == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("创建动画蓝图失败：%s"), *InDocument.Asset));
			return false;
		}

		FAssetRegistryModule::AssetCreated(Blueprint);
		Package->MarkPackageDirty();
	}
	else if (Blueprint->ParentClass != ParentClass)
	{
		OutReport.AddError(
			TEXT("parent"),
			FString::Printf(
				TEXT("现有动画蓝图的父类是 `%s`，与配置里的 `%s` 不一致；换父类请人工处理"),
				*Blueprint->ParentClass->GetPathName(),
				*ParentPath));
		return false;
	}

	if (Blueprint->TargetSkeleton != Skeleton)
	{
		if (Blueprint->TargetSkeleton != nullptr)
		{
			OutReport.AddError(
				TEXT("skeleton"),
				FString::Printf(
					TEXT("现有动画蓝图绑定的骨架是 `%s`，与配置里的 `%s` 不一致；换骨架请人工处理"),
					*Blueprint->TargetSkeleton->GetPathName(),
					*SkeletonPath));
			return false;
		}

		Blueprint->TargetSkeleton = Skeleton;
	}

	UEdGraph* AnimGraph = FindAnimGraph(Blueprint);
	if (AnimGraph == nullptr)
	{
		OutReport.AddError(TEXT("asset"), TEXT("动画蓝图里没有 AnimGraph 图"));
		return false;
	}

	// 现有状态机节点按图名匹配：配置里有的改，配置里没有的删。
	TMap<FString, UAnimGraphNode_StateMachineBase*> ExistingNodes;
	TArray<UAnimGraphNode_StateMachineBase*> ExistingOrder;
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		UAnimGraphNode_StateMachineBase* StateMachineNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
		if (StateMachineNode == nullptr || StateMachineNode->EditorStateMachineGraph == nullptr)
		{
			continue;
		}

		ExistingNodes.Add(StateMachineNode->EditorStateMachineGraph->GetName(), StateMachineNode);
		ExistingOrder.Add(StateMachineNode);
	}

	TMap<FString, UAnimGraphNode_StateMachineBase*> KeptNodes;

	for (int32 Index = 0; Index < Specs.Num(); ++Index)
	{
		const FStateMachineSpec& Spec = Specs[Index];
		const FString StateMachinePath = FString::Printf(TEXT("root/%s"), *Spec.Name);

		UAnimGraphNode_StateMachineBase** Existing = ExistingNodes.Find(Spec.Name);
		UAnimGraphNode_StateMachineBase* StateMachineNode = Existing != nullptr ? *Existing : nullptr;

		if (StateMachineNode == nullptr)
		{
			UAnimGraphNode_StateMachine* NewNode = NewObject<UAnimGraphNode_StateMachine>(AnimGraph);
			NewNode->CreateNewGuid();
			NewNode->PostPlacedNewNode();
			NewNode->AllocateDefaultPins();
			AnimGraph->AddNode(NewNode, /*bSelectNewNode=*/false, /*bIsUserAction=*/false);

			NewNode->NodePosX = 0;
			NewNode->NodePosY = Index * 480;

			FBlueprintEditorUtils::RenameGraph(NewNode->EditorStateMachineGraph, Spec.Name);

			// 状态机要接到 AnimGraph 的根节点上，否则编译时接不到最终 Pose。
			UAnimGraphNode_Root* RootNode = FindAnimGraphRoot(AnimGraph);
			UEdGraphPin* FromPin = FindFreePin(NewNode, EGPD_Output);
			UEdGraphPin* ToPin = FindFreePin(RootNode, EGPD_Input);
			if (FromPin != nullptr && ToPin != nullptr)
			{
				AnimGraph->GetSchema()->TryCreateConnection(FromPin, ToPin);
			}
			else
			{
				OutReport.AddWarning(StateMachinePath, TEXT("状态机没能连到 AnimGraph 的根节点，需要在编辑器里手动连上"));
			}

			StateMachineNode = NewNode;
		}

		KeptNodes.Add(Spec.Name, StateMachineNode);

		if (!SyncStateMachine(Blueprint, StateMachineNode, Spec, OutReport, StateMachinePath))
		{
			return false;
		}
	}

	for (UAnimGraphNode_StateMachineBase* StateMachineNode : ExistingOrder)
	{
		const FString NodeName = StateMachineNode->EditorStateMachineGraph != nullptr
			? StateMachineNode->EditorStateMachineGraph->GetName()
			: FString();
		if (KeptNodes.Contains(NodeName))
		{
			continue;
		}

		OutReport.Actions.Add(FForgePlanEntry{
			EForgeAction::Delete,
			FString::Printf(TEXT("root/%s"), *NodeName),
			NodeName,
			TEXT("配置里没有这个状态机") });

		FBlueprintEditorUtils::RemoveNode(Blueprint, StateMachineNode, /*bDontRecompile=*/true);
	}

	OutReport.CountActions();

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	if (Blueprint->Status == BS_Error)
	{
		OutReport.AddError(
			TEXT("asset"),
			TEXT("动画蓝图编译失败，细节见日志（常见原因是序列与骨架不匹配）"));
		return false;
	}

	FString SaveError;
	if (!SaveAssetPackage(Blueprint, SaveError))
	{
		OutReport.AddError(TEXT("asset"), SaveError);
		return false;
	}

	// 归一化：让配置文件的形态与导出形态一致（显式写出的默认值在这里被摘掉）。
	InDocument.Root = TargetRoot;

	OutReport.bSucceeded = true;
	return true;
}

} // namespace AnimBlueprintForge
