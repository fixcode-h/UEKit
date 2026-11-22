// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/Blocks/GameTestBlockHooks.h"

#include "Core/Flow/Blocks/GameTestBlockUtil.h"
#include "Core/Script/GameTestFlowContext.h"

/**
 * 附带 GM 的执行（协议见 docs/game-test-script.md「附带 GM」）：
 * 执行前那组 → 主步骤 → 主步骤通过后「成功后」那组；主步骤（或 onError: fail 的附带项）失败时改跑「失败时」那组，再按失败往外传。
 *
 * - 主步骤自己的 onFail: retry 在里面那一层处理（重做时不再跑执行前那组，成功后那组只在最后通过那次后跑）；
 *   onFail: continue 挪到包的这一层，这样主步骤失败时照样先跑「失败时」那组再吞掉。
 * - 每一项 `onError`：note（默认，出错只记一笔、不算 run 失败）/ fail（这一步算失败）；`once: true` 循环里只第一次跑。
 * - 成功后、失败时那两组里 `$ret` 是主步骤 saveAs 存的值（调函数的返回值、找到的对象、call 的返回）。
 */
namespace
{
	struct FHookItem
	{
		int32 List = INDEX_NONE;
		bool bOnce = false;
	};

	struct FHooksData : FGameTestNodeData
	{
		TArray<FHookItem> Before;
		TArray<FHookItem> After;
		TArray<FHookItem> Fail;
		/** 主步骤的 saveAs：成功后那组的 $ret 从它取。 */
		FString SaveAs;
	};

	enum class EHookPhase : uint8
	{
		Before,
		Main,
		After,
		Fail,
		Done,
	};

	struct FHooksState : FGameTestBlockState
	{
		EHookPhase Phase = EHookPhase::Before;
		int32 Cursor = 0;
		bool bLocals = false;
		FString FailMessage;
	};

	class FBlockHooks : public IGameTestBlock
	{
	public:
		virtual FName GetKey() const override { return TEXT("hooks"); }
		virtual FString GetSummary() const override { return TEXT("附带 GM：步骤的执行前 / 成功后 / 失败时"); }
		/** 不是写在脚本里的动作：只由解析器包出来，不认 action: hooks。 */
		virtual bool Claims(const FJsonObject& InJson, FName InAction) const override { return false; }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields;
			return Fields;
		}

		virtual void Parse(FGameTestNode& InOutNode, FGameTestScriptParser& InParser) const override
		{
		}

		virtual TSharedRef<FGameTestBlockState> Enter(const FGameTestNode& InNode, FGameTestFlowContext& InContext) const override
		{
			// 压一层局部放 $ret：出了这一步就看不到
			TSharedRef<FHooksState> State = MakeShared<FHooksState>();
			InContext.PushLocals();
			State->bLocals = true;
			return State;
		}

		virtual EGameTestBlockStep Tick(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext, double InNowSeconds, FGameTestBlockTick& Out) const override
		{
			FHooksState& State = static_cast<FHooksState&>(InState);
			const FHooksData* Data = InNode.GetData<FHooksData>();
			if (!Data || InNode.Lists.Num() == 0)
			{
				Out.Message = TEXT("附带 GM 没解析成（场景有错，先 gt.validate）");
				return EGameTestBlockStep::Failed;
			}
			switch (State.Phase)
			{
			case EHookPhase::Before:
				if (NextItem(Data->Before, InNode, State, InContext, Out))
				{
					return EGameTestBlockStep::RunList;
				}
				State.Phase = EHookPhase::Main;
				Out.List = 0;
				return EGameTestBlockStep::RunList;
			case EHookPhase::Main:
			{
				// 主步骤通过了（失败会走 CatchFailure）：成功后那组能用 $ret
				FGameTestValue Ret;
				if (!Data->SaveAs.IsEmpty() && InContext.FindVar(Data->SaveAs, Ret))
				{
					InContext.SetLocal(TEXT("ret"), Ret);
				}
				State.Phase = EHookPhase::After;
				State.Cursor = 0;
				[[fallthrough]];
			}
			case EHookPhase::After:
				if (NextItem(Data->After, InNode, State, InContext, Out))
				{
					return EGameTestBlockStep::RunList;
				}
				State.Phase = EHookPhase::Done;
				Out.Message = TEXT("附带 GM 跑完");
				return EGameTestBlockStep::Done;
			case EHookPhase::Fail:
				if (NextItem(Data->Fail, InNode, State, InContext, Out))
				{
					return EGameTestBlockStep::RunList;
				}
				State.Phase = EHookPhase::Done;
				Out.Message = State.FailMessage;
				return EGameTestBlockStep::Failed;
			case EHookPhase::Done:
			default:
				Out.Message = State.FailMessage;
				return State.FailMessage.IsEmpty() ? EGameTestBlockStep::Done : EGameTestBlockStep::Failed;
			}
		}

		virtual bool CatchFailure(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext, const FString& InMessage) const override
		{
			FHooksState& State = static_cast<FHooksState&>(InState);
			if (State.Phase == EHookPhase::Fail || State.Phase == EHookPhase::Done)
			{
				// 「失败时」那组自己失败了、或者已经跑完要往外传：不再接
				if (State.FailMessage.IsEmpty())
				{
					State.FailMessage = InMessage;
				}
				return false;
			}
			State.FailMessage = State.Phase == EHookPhase::Main ? InMessage : FString::Printf(TEXT("附带 GM 出错（onError: fail）：%s"), *InMessage);
			State.Phase = EHookPhase::Fail;
			State.Cursor = 0;
			return true;
		}

		virtual void Leave(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext) const override
		{
			FHooksState& State = static_cast<FHooksState&>(InState);
			if (State.bLocals)
			{
				InContext.PopLocals();
				State.bLocals = false;
			}
		}

	private:
		/** 这一组里下一项（跳过循环里已经跑过的 once 项）：有就填 Out.List 返回 true。 */
		static bool NextItem(const TArray<FHookItem>& InItems, const FGameTestNode& InNode, FHooksState& InState, FGameTestFlowContext& InContext, FGameTestBlockTick& Out)
		{
			while (InItems.IsValidIndex(InState.Cursor))
			{
				const FHookItem& Item = InItems[InState.Cursor++];
				if (!InNode.Lists.IsValidIndex(Item.List) || InNode.Lists[Item.List].Nodes.Num() == 0)
				{
					continue;
				}
				if (Item.bOnce)
				{
					// 同一个子脚本被 call 多次时路径一样：带上脚本路径，各算各的
					const FString Key = InContext.GetScope().Script + TEXT("|") + InNode.Lists[Item.List].Nodes[0]->Path;
					if (InContext.HooksOnceDone.Contains(Key))
					{
						continue;
					}
					InContext.HooksOnceDone.Add(Key);
				}
				Out.List = Item.List;
				return true;
			}
			return false;
		}
	};

	const FBlockHooks& HooksBlock()
	{
		static FBlockHooks Block;
		return Block;
	}

	/** 附带项和它的子步骤都标上：断点、单步跳过；note 的失败只记一笔。 */
	void MarkInHook(FGameTestNode& InNode, bool bNote)
	{
		InNode.bInHook = true;
		InNode.bHookNote = bNote;
		for (FGameTestNodeList& List : InNode.Lists)
		{
			for (const TSharedRef<FGameTestNode>& Child : List.Nodes)
			{
				MarkInHook(*Child, bNote);
			}
		}
	}

	TSharedRef<FJsonObject> CopyWithout(const FJsonObject& InJson, std::initializer_list<const TCHAR*> InFields)
	{
		TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
		Copy->Values = InJson.Values;
		for (const TCHAR* Field : InFields)
		{
			Copy->RemoveField(Field);
		}
		return Copy;
	}

	/** 解析一组附带项（before / after / fail），每项单独一串，执行时能逐项跳过 once。 */
	void ParseGroup(FGameTestScriptParser& InParser, FGameTestNode& InOutWrap, const FJsonObject& InHooks, const TCHAR* InGroup, const FString& InPath, TArray<FHookItem>& OutItems)
	{
		FGameTestDiagSink& Sink = InParser.Sink();
		const TSharedPtr<FJsonValue> Value = InHooks.TryGetField(InGroup);
		if (!Value.IsValid())
		{
			return;
		}
		const FString GroupPath = FString::Printf(TEXT("%s.hooks.%s"), *InPath, InGroup);
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Value->TryGetArray(Items) || !Items)
		{
			FGameTestDiagSink::FScope Scope(Sink, GroupPath, FString(), FGameTestDiagSink::EScopeMode::FromRoot);
			Sink.Error(TEXT("bad_type"), TEXT("附带 GM 的每组要写成步骤数组"));
			return;
		}
		// 成功后、失败时能用 $ret（主步骤 saveAs 存的值）；执行前那时还没有
		InParser.PushVarScope();
		if (FCString::Strcmp(InGroup, TEXT("before")) != 0)
		{
			InParser.DeclareVar(TEXT("ret"));
		}
		for (int32 Index = 0; Index < Items->Num(); ++Index)
		{
			const FString ItemPath = FString::Printf(TEXT("%s.%d"), *GroupPath, Index);
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Object) || !Object || !Object->IsValid())
			{
				FGameTestDiagSink::FScope Scope(Sink, ItemPath, FString(), FGameTestDiagSink::EScopeMode::FromRoot);
				Sink.Error(TEXT("bad_type"), TEXT("步骤要写成对象"));
				continue;
			}
			FString OnError = TEXT("note");
			(*Object)->TryGetStringField(TEXT("onError"), OnError);
			bool bOnce = false;
			(*Object)->TryGetBoolField(TEXT("once"), bOnce);
			if (OnError != TEXT("note") && OnError != TEXT("fail"))
			{
				FGameTestDiagSink::FScope Scope(Sink, ItemPath, FString(), FGameTestDiagSink::EScopeMode::FromRoot);
				Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("onError 只能是 note（只记一笔）或 fail（这一步算失败），写的是 %s"), *OnError));
			}
			const TSharedRef<FGameTestNode> Item = InParser.ParseNode(MakeShared<FJsonValueObject>(CopyWithout(**Object, { TEXT("onError"), TEXT("once") })), ItemPath);
			// note：出错不往外传（记录标 warn）；fail：往外传到包的这一层，跑失败时那组
			const bool bNote = OnError != TEXT("fail");
			Item->Step.FailMode = bNote ? EGameTestFailMode::Continue : EGameTestFailMode::Stop;
			Item->Step.bNested = true;
			Item->Step.Index = Index;
			MarkInHook(*Item, bNote);
			FGameTestNodeList& List = InOutWrap.Lists.AddDefaulted_GetRef();
			List.Key = FString::Printf(TEXT("hooks.%s.%d"), InGroup, Index);
			List.Nodes.Add(Item);
			OutItems.Add({ InOutWrap.Lists.Num() - 1, bOnce });
		}
		InParser.PopVarScope();
	}
}

namespace GameTestHookWrap
{
	TSharedRef<FGameTestNode> Wrap(FGameTestScriptParser& InParser, const TSharedPtr<FJsonObject>& InJson, const FString& InPath)
	{
		TSharedRef<FJsonObject> MainJson = CopyWithout(*InJson, { TEXT("hooks") });
		bool bDisabled = false;
		InJson->TryGetBoolField(TEXT("disabled"), bDisabled);
		if (bDisabled)
		{
			// 整步不跑，附带的也不跑：不包，记录照旧是这一步的 Disabled
			return InParser.ParseNode(MakeShared<FJsonValueObject>(MainJson), InPath);
		}

		TSharedRef<FGameTestNode> Outer = MakeShared<FGameTestNode>();
		Outer->Path = InPath + TEXT("#hooks");
		Outer->Action = TEXT("hooks");
		Outer->Block = &HooksBlock();
		Outer->bHookWrap = true;
		Outer->Json = MakeShared<FJsonObject>();
		Outer->Json->SetStringField(TEXT("action"), TEXT("hooks"));
		Outer->Step.Action = Outer->Action;
		Outer->Step.Params = Outer->Json;
		Outer->Step.Note = TEXT("附带 GM");
		Outer->Step.bNested = true;
		Outer->Step.Path = Outer->Path;
		FString OnFail;
		if (MainJson->TryGetStringField(TEXT("onFail"), OnFail) && OnFail.Equals(TEXT("continue"), ESearchCase::IgnoreCase))
		{
			// 主步骤失败要先跑完失败时那组再吞掉：continue 挪到外面这层
			MainJson->RemoveField(TEXT("onFail"));
			Outer->Step.FailMode = EGameTestFailMode::Continue;
		}

		TSharedRef<FHooksData> Data = MakeShared<FHooksData>();
		MainJson->TryGetStringField(TEXT("saveAs"), Data->SaveAs);
		FGameTestNodeList& MainList = Outer->Lists.AddDefaulted_GetRef();
		MainList.Key = TEXT("step");
		MainList.Nodes.Add(InParser.ParseNode(MakeShared<FJsonValueObject>(MainJson), InPath));

		FGameTestDiagSink& Sink = InParser.Sink();
		const TSharedPtr<FJsonObject>* Hooks = nullptr;
		if (!InJson->TryGetObjectField(TEXT("hooks"), Hooks) || !Hooks || !Hooks->IsValid())
		{
			FGameTestDiagSink::FScope Scope(Sink, InPath + TEXT(".hooks"), FString(), FGameTestDiagSink::EScopeMode::FromRoot);
			Sink.Error(TEXT("bad_type"), TEXT("hooks 要写成 {before, after, fail}，每组是步骤数组"));
		}
		else
		{
			for (const auto& Pair : (*Hooks)->Values)
			{
				const FString Key = GAMETEST_JSON_KEY(Pair.Key);
				if (Key != TEXT("before") && Key != TEXT("after") && Key != TEXT("fail"))
				{
					FGameTestDiagSink::FScope Scope(Sink, InPath + TEXT(".hooks"), FString(), FGameTestDiagSink::EScopeMode::FromRoot);
					Sink.Error(TEXT("unknown_field"), FString::Printf(TEXT("hooks 不认识 %s（可用：before, after, fail）"), *Key));
				}
			}
			ParseGroup(InParser, *Outer, **Hooks, TEXT("before"), InPath, Data->Before);
			ParseGroup(InParser, *Outer, **Hooks, TEXT("after"), InPath, Data->After);
			ParseGroup(InParser, *Outer, **Hooks, TEXT("fail"), InPath, Data->Fail);
		}
		Outer->Data = Data;
		return Outer;
	}

	FGameTestNode& Main(FGameTestNode& InNode)
	{
		return InNode.bHookWrap && InNode.Lists.Num() > 0 && InNode.Lists[0].Nodes.Num() > 0 ? *InNode.Lists[0].Nodes[0] : InNode;
	}
}
