// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/Blocks/GameTestBlockUtil.h"
#include "Core/Script/GameTestFlowContext.h"

/**
 * `if`：`cond` / `then`、`elif: [{cond, then}]`、`else`。从上往下第一条成立的走，都不成立走 else（可省）。
 *
 * 判断带稳住（协议第 4.2 节）：所有分支的条件同时看，按顺序找第一条「稳住成立」的；
 * 排在它前面的必须都已「稳住不成立」（elseSettleMs），不然还要再等——界面刚弹出一半时不会误走 else。
 */
namespace
{
	struct FIfData : FGameTestNodeData
	{
		struct FBranch
		{
			TSharedPtr<FJsonObject> Cond;
			int32 List = INDEX_NONE;
			/** 报告里的分支名：`then`、`elif.0`。 */
			FString Name;
		};
		TArray<FBranch> Branches;
		int32 ElseList = INDEX_NONE;
	};

	struct FIfState : FGameTestBlockState
	{
		TArray<FGameTestCondWatch> Watches;
		/** 每条分支从什么时候开始「判断不了」（反面判断的屏不在）：久了按不成立算，不然会一直等。 */
		TArray<double> UnknownSince;
		bool bStarted = false;
		bool bChosen = false;
		FString Chosen;
	};

	class FBlockIf : public IGameTestBlock
	{
	public:
		virtual FName GetKey() const override { return TEXT("if"); }
		virtual FString GetSummary() const override { return TEXT("条件分支：第一条成立的走，都不成立走 else"); }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("cond"), TEXT("then"), TEXT("elif"), TEXT("else"), TEXT("branch") };
			return Fields;
		}

		virtual void Parse(FGameTestNode& InOutNode, FGameTestScriptParser& InParser) const override
		{
			TSharedRef<FIfData> Data = MakeShared<FIfData>();
			FGameTestDiagSink& Sink = InParser.Sink();
			const FJsonObject& Json = *InOutNode.Json;

			FIfData::FBranch& First = Data->Branches.AddDefaulted_GetRef();
			First.Name = TEXT("then");
			First.Cond = GameTestBlockUtil::ParseCond(InParser, InOutNode, TEXT("cond"), true);
			First.List = InParser.ParseList(InOutNode, TEXT("then"), Json.TryGetField(TEXT("then")), true);

			const TSharedPtr<FJsonValue> Elif = Json.TryGetField(TEXT("elif"));
			const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
			if (Elif.IsValid() && (!Elif->TryGetArray(Items) || !Items))
			{
				Sink.Error(TEXT("bad_type"), TEXT("elif 要写成数组：[{\"cond\": …, \"then\": […]}]"));
			}
			else if (Items)
			{
				for (int32 Index = 0; Index < Items->Num(); ++Index)
				{
					const FString Key = FString::Printf(TEXT("elif.%d"), Index);
					FGameTestDiagSink::FScope Scope(Sink, Key);
					const TSharedPtr<FJsonObject>* Object = nullptr;
					if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Object) || !Object || !Object->IsValid())
					{
						Sink.Error(TEXT("bad_type"), TEXT("elif 的每一项要写成 {cond, then}"));
						continue;
					}
					for (const auto& Pair : (*Object)->Values)
					{
						const FString Field = GAMETEST_JSON_KEY(Pair.Key);
						if (Field != TEXT("cond") && Field != TEXT("then"))
						{
							Sink.Error(TEXT("unknown_field"), FString::Printf(TEXT("elif 里不认识字段 %s（可用：cond, then）"), *Field));
						}
					}
					FIfData::FBranch& Branch = Data->Branches.AddDefaulted_GetRef();
					Branch.Name = Key;
					const TSharedPtr<FJsonValue> Cond = (*Object)->TryGetField(TEXT("cond"));
					if (!Cond.IsValid())
					{
						Sink.Error(TEXT("bad_type"), TEXT("elif 缺少 cond"));
					}
					else
					{
						InParser.CheckCond(Cond, TEXT("cond"));
						Branch.Cond = GameTestCond::AsObject(Cond);
					}
					Branch.List = InParser.ParseList(InOutNode, Key + TEXT(".then"), (*Object)->TryGetField(TEXT("then")), true);
				}
			}
			Data->ElseList = InParser.ParseList(InOutNode, TEXT("else"), Json.TryGetField(TEXT("else")), false);

			FString Branch;
			if (Json.TryGetStringField(TEXT("branch"), Branch) && Branch != TEXT("state"))
			{
				Sink.Warning(TEXT("bad_type"), FString::Printf(TEXT("branch 只有 \"state\" 一种（面板画成按状态分支），写的是 %s"), *Branch));
			}
			InOutNode.Data = Data;
		}

		virtual TSharedRef<FGameTestBlockState> Enter(const FGameTestNode& InNode, FGameTestFlowContext& InContext) const override
		{
			return MakeShared<FIfState>();
		}

		virtual EGameTestBlockStep Tick(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext,
			double InNowSeconds, FGameTestBlockTick& Out) const override
		{
			FIfState& State = static_cast<FIfState&>(InState);
			const FIfData* Data = InNode.GetData<FIfData>();
			if (!Data)
			{
				Out.Message = TEXT("if 没解析好（场景有错，先 gt.validate）");
				return EGameTestBlockStep::Failed;
			}
			if (State.bChosen)
			{
				// 分支跑完了。
				Out.Message = State.Chosen.IsEmpty() ? TEXT("条件都不成立，没有 else") : FString::Printf(TEXT("走了 %s"), *State.Chosen);
				Out.Payload = MakeShared<FJsonObject>();
				Out.Payload->SetStringField(TEXT("branch"), State.Chosen.IsEmpty() ? TEXT("none") : State.Chosen);
				return EGameTestBlockStep::Done;
			}
			if (!State.bStarted)
			{
				State.bStarted = true;
				State.Watches.SetNum(Data->Branches.Num());
				State.UnknownSince.Init(-1.0, Data->Branches.Num());
				for (int32 Index = 0; Index < Data->Branches.Num(); ++Index)
				{
					State.Watches[Index].Start(Data->Branches[Index].Cond, InContext, true);
				}
			}

			FString Waiting;
			for (int32 Index = 0; Index < Data->Branches.Num(); ++Index)
			{
				const FIfData::FBranch& Branch = Data->Branches[Index];
				FGameTestCondWatch& Watch = State.Watches[Index];
				FGameTestCondWatch::EVerdict Verdict = FGameTestCondWatch::EVerdict::NotHolds;
				if (Branch.Cond.IsValid())
				{
					FGameTestFlowContext::FReadOnlyScope ReadOnly(InContext);
					Verdict = Watch.Tick(InContext, InNowSeconds);
				}
				if (Watch.GetLast().State == EGameTestCondState::Unknown)
				{
					double& Since = State.UnknownSince[Index];
					Since = Since < 0.0 ? InNowSeconds : Since;
					const int32 ElseSettle = Branch.Cond.IsValid() ? GameTestCond::ElseSettleMs(*Branch.Cond, InContext) : 0;
					if ((InNowSeconds - Since) * 1000.0 >= ElseSettle)
					{
						Verdict = FGameTestCondWatch::EVerdict::NotHolds;
					}
				}
				else
				{
					State.UnknownSince[Index] = -1.0;
				}

				if (Verdict == FGameTestCondWatch::EVerdict::Holds)
				{
					return Choose(State, Branch.Name, Branch.List, Watch.GetLast(), Out);
				}
				if (Verdict == FGameTestCondWatch::EVerdict::Pending)
				{
					Out.Message = FString::Printf(TEXT("在等 %s 的条件稳住：%s"), *Branch.Name, *Watch.GetLast().Summary());
					return EGameTestBlockStep::Pending;
				}
			}
			if (Data->ElseList != INDEX_NONE)
			{
				return Choose(State, TEXT("else"), Data->ElseList, FGameTestCondResult(), Out);
			}
			State.bChosen = true;
			Out.Message = TEXT("条件都不成立，没有 else");
			Out.Payload = MakeShared<FJsonObject>();
			Out.Payload->SetStringField(TEXT("branch"), TEXT("none"));
			return EGameTestBlockStep::Done;
		}

	private:
		static EGameTestBlockStep Choose(FIfState& InState, const FString& InName, int32 InList, const FGameTestCondResult& InResult,
			FGameTestBlockTick& Out)
		{
			InState.bChosen = true;
			InState.Chosen = InName;
			// 走哪条分支、条件当时的值写进 payload：报告里能看到「为什么走了这边」。
			Out.Payload = MakeShared<FJsonObject>();
			Out.Payload->SetStringField(TEXT("branch"), InName);
			if (!InResult.Parts.IsEmpty())
			{
				Out.Payload->SetObjectField(TEXT("cond"), InResult.ToJson());
			}
			if (InList == INDEX_NONE)
			{
				return EGameTestBlockStep::Done;
			}
			Out.List = InList;
			Out.Message = FString::Printf(TEXT("走 %s"), *InName);
			return EGameTestBlockStep::RunList;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterBlockIf()
	{
		GameTestBlocks::Get().Register(MakeShared<FBlockIf>());
	}
}
