// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/Blocks/GameTestBlockUtil.h"
#include "Core/GameTestHandlerShared.h"
#include "Core/Script/GameTestDiag.h"
#include "Core/Expr/GameTestExpr.h"
#include "Observe/Ref/GameTestRef.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Components/ListView.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Observe/GameTestItemKey.h"
#include "Observe/GameTestWidgetNode.h"

/**
 * `loop`（协议第 2 节）：四选一——`until`（每轮做完看，成立就出）、`while`（每轮开始前看）、`times`、`each` / `in`。
 *
 * - `max` 除 `times` 外必填（防死循环），上限 1000；到了按 `onMax`（fail 默认 / continue）。
 * - 循环变量 `$index`（从 0）；`each` / `in` 的当前条目叫 `as`（默认 `item`）。名字放块局部，出了循环看不到。
 * - `each`：每轮重新收一遍容器的条目（领奖后条目可能消失、顺序会变），第 N 轮取第 N 个；条目没生成（虚拟化列表没滚到）先滚过去等它生成。
 *   `$item` 是条目控件（2.0.0 起，引用写法 `{"obj": "$item", "part": "BtnClaim"}`；数据对象用 `prop("$item", "@item.X")` 读）。
 */
namespace
{
	enum class ELoopMode : uint8
	{
		Until,
		While,
		Times,
		Each,
		In,
	};

	/** 写了正数时的上限，只防手误；要更多轮写 0（不限） */
	constexpr int32 LoopMaxLimit = 1000000;
	/** 条目没生成时最多等多久（秒）：滚过去后列表下一帧才生成控件，慢的界面要几帧。 */
	constexpr double EntryWaitSeconds = 3.0;

	struct FLoopData : FGameTestNodeData
	{
		ELoopMode Mode = ELoopMode::Until;
		TSharedPtr<FJsonObject> Cond;
		TSharedPtr<FJsonValue> Times;
		TSharedPtr<FJsonValue> In;
		/** each 的容器（部件写法 {obj, part}，v4）。 */
		TSharedPtr<FJsonValue> Each;
		FString As = TEXT("item");
		int32 Max = 0;
		int32 GapMs = 0;
		bool bOnMaxContinue = false;
		int32 List = INDEX_NONE;
	};

	enum class ELoopPhase : uint8
	{
		Init,
		BeginIter,
		CheckWhile,
		Prepare,
		AfterBody,
		CheckUntil,
		Gap,
	};

	struct FLoopState : FGameTestBlockState
	{
		ELoopPhase Phase = ELoopPhase::Init;
		int32 Iter = 0;
		/** times / in 的总轮数；until / while / each 为 -1（each 每轮重新数）。 */
		int32 Count = -1;
		TArray<FGameTestValue> Items;
		FString Container;
		FGameTestCondWatch Watch;
		bool bBreak = false;
		bool bLocals = false;
		double GapUntil = -1.0;
		double WaitSince = -1.0;
		double ContainerWaitSince = -1.0;
		double EntryWaitSince = -1.0;
	};

	class FBlockLoop : public IGameTestBlock
	{
	public:
		virtual FName GetKey() const override { return TEXT("loop"); }
		virtual FString GetSummary() const override { return TEXT("循环：until / while / times / each / in 四选一，max 防死循环"); }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = {
				TEXT("until"), TEXT("while"), TEXT("times"), TEXT("each"), TEXT("in"), TEXT("as"), TEXT("max"), TEXT("gapMs"), TEXT("onMax"), TEXT("do"),
			};
			return Fields;
		}

		virtual void Parse(FGameTestNode& InOutNode, FGameTestScriptParser& InParser) const override
		{
			TSharedRef<FLoopData> Data = MakeShared<FLoopData>();
			FGameTestDiagSink& Sink = InParser.Sink();
			const FJsonObject& Json = *InOutNode.Json;

			TArray<FString> Modes;
			for (const TCHAR* Key : { TEXT("until"), TEXT("while"), TEXT("times"), TEXT("each"), TEXT("in") })
			{
				if (Json.HasField(Key))
				{
					Modes.Add(Key);
				}
			}
			if (Modes.Num() != 1)
			{
				Sink.Error(TEXT("bad_type"), Modes.Num() == 0
					? TEXT("loop 要写 until、while、times、each、in 其中一个")
					: FString::Printf(TEXT("loop 只能写 until、while、times、each、in 其中一个，写了 %s"), *FString::Join(Modes, TEXT("、"))));
			}
			const FString Mode = Modes.Num() > 0 ? Modes[0] : TEXT("until");
			if (Mode == TEXT("until") || Mode == TEXT("while"))
			{
				Data->Mode = Mode == TEXT("until") ? ELoopMode::Until : ELoopMode::While;
				Data->Cond = GameTestBlockUtil::ParseCond(InParser, InOutNode, *Mode, true);
			}
			else if (Mode == TEXT("times"))
			{
				Data->Mode = ELoopMode::Times;
				Data->Times = Json.TryGetField(TEXT("times"));
				double Number = 0.0;
				FString Text;
				if (Data->Times->TryGetNumber(Number))
				{
					if (Number < 0 || Number > LoopMaxLimit)
					{
						Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("times 要在 0–%d 之间"), LoopMaxLimit));
					}
				}
				else if (!Data->Times->TryGetString(Text))
				{
					Sink.Error(TEXT("bad_type"), TEXT("times 要写整数或 $变量"));
				}
				InParser.CheckValue(Data->Times, TEXT("times"));
			}
			else if (Mode == TEXT("each"))
			{
				Data->Mode = ELoopMode::Each;
				Data->Each = Json.TryGetField(TEXT("each"));
				if (!Data->Each.IsValid() || Data->Each->IsNull())
				{
					Sink.Error(TEXT("bad_type"), TEXT("each 要写容器：{\"obj\": 引用, \"part\": \"列表部件名\"}（列表、滚动框、面板）"));
				}
				else
				{
					FString Unused;
					FString Error;
					if (!GameTestHandlers::TargetText(Json, TEXT("each"), Unused, Error))
					{
						Sink.Error(TEXT("selector_removed"), Error);
					}
					else
					{
						FGameTestDiagSink::FScope EachScope(Sink, TEXT("each"));
						GameTestRefCheck::Target(Data->Each, InParser.MakeExprScope(), Sink);
					}
				}
			}
			else
			{
				Data->Mode = ELoopMode::In;
				Data->In = Json.TryGetField(TEXT("in"));
				InParser.CheckValue(Data->In, TEXT("in"));
			}

			if (Json.TryGetStringField(TEXT("as"), Data->As))
			{
				if (Data->Mode != ELoopMode::Each && Data->Mode != ELoopMode::In)
				{
					Sink.Warning(TEXT("unknown_field"), TEXT("as 只对 each / in 有用"));
				}
				if (!GameTestBlockUtil::IsValidVarName(Data->As) || Data->As == TEXT("index"))
				{
					Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("as 写的「%s」不能当变量名（字母数字下划线，不以数字开头，不能叫 index）"), *Data->As));
				}
			}

			// max：0 或不写 = 不限轮数（用户 2026-11-19 定）。不限时靠整块的 timeoutMs 或面板的「停止」收住，
			// 每轮至少让出一帧，不会卡住编辑器。
			Json.TryGetNumberField(TEXT("max"), Data->Max);
			if (Data->Max < 0 || Data->Max > LoopMaxLimit)
			{
				Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("max 要在 0–%d 之间（0 = 不限），写的是 %d"), LoopMaxLimit, Data->Max));
			}
			Data->Max = FMath::Clamp(Data->Max, 0, LoopMaxLimit);
			Json.TryGetNumberField(TEXT("gapMs"), Data->GapMs);
			Data->GapMs = FMath::Clamp(Data->GapMs, 0, 600000);
			FString OnMax;
			if (Json.TryGetStringField(TEXT("onMax"), OnMax))
			{
				Data->bOnMaxContinue = OnMax == TEXT("continue");
				if (OnMax != TEXT("continue") && OnMax != TEXT("fail"))
				{
					Sink.Error(TEXT("bad_type"), FString::Printf(TEXT("onMax 只能是 fail 或 continue，写的是 %s"), *OnMax));
				}
			}

			// 循环变量只在循环体里能用；until 在循环体之后判断，也看得到它们。
			InParser.PushVarScope();
			InParser.DeclareVar(TEXT("index"));
			if (Data->Mode == ELoopMode::Each || Data->Mode == ELoopMode::In)
			{
				InParser.DeclareVar(Data->As);
			}
			Data->List = InParser.ParseList(InOutNode, TEXT("do"), Json.TryGetField(TEXT("do")), true);
			InParser.PopVarScope();
			InOutNode.Data = Data;
		}

		virtual TSharedRef<FGameTestBlockState> Enter(const FGameTestNode& InNode, FGameTestFlowContext& InContext) const override
		{
			return MakeShared<FLoopState>();
		}

		virtual EGameTestBlockStep Tick(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext, double InNowSeconds, FGameTestBlockTick& Out) const override
		{
			FLoopState& State = static_cast<FLoopState&>(InState);
			const FLoopData* Data = InNode.GetData<FLoopData>();
			if (!Data || Data->List == INDEX_NONE)
			{
				Out.Message = TEXT("loop 没解析好（场景有错，先 gt.validate）");
				return EGameTestBlockStep::Failed;
			}
			// 一次 Tick 里可能连着走几个阶段（做完 → 不用等 → 下一轮），上限防止写错的状态机卡死编辑器。
			for (int32 Guard = 0; Guard < 16; ++Guard)
			{
				switch (State.Phase)
				{
				case ELoopPhase::Init:
				{
					FString Error;
					if (!Init(*Data, State, InContext, Error))
					{
						Out.Message = Error;
						return EGameTestBlockStep::Failed;
					}
					InContext.PushLocals();
					State.bLocals = true;
					State.Phase = ELoopPhase::BeginIter;
					break;
				}
				case ELoopPhase::BeginIter:
					if (State.Count >= 0 && State.Iter >= State.Count)
					{
						return Finish(State, TEXT("count"), Out);
					}
					if (Data->Max > 0 && State.Iter >= Data->Max)
					{
						if (Data->bOnMaxContinue)
						{
							return Finish(State, TEXT("max"), Out);
						}
						Out.Message = FString::Printf(TEXT("循环跑满 max=%d 轮还没结束（%s）：条件可能永远不成立，或把 max 调大、onMax 改成 continue"),
							Data->Max, *State.Watch.GetLast().Summary());
						return EGameTestBlockStep::Failed;
					}
					if (Data->Mode == ELoopMode::While)
					{
						State.Watch.Start(Data->Cond, InContext, true);
						State.Phase = ELoopPhase::CheckWhile;
					}
					else
					{
						State.Phase = ELoopPhase::Prepare;
					}
					break;
				case ELoopPhase::CheckWhile:
				{
					const FGameTestCondWatch::EVerdict Verdict = TickWatch(State, InContext, InNowSeconds);
					if (Verdict == FGameTestCondWatch::EVerdict::Pending)
					{
						Out.Message = FString::Printf(TEXT("第 %d 轮前在等 while 的条件稳住：%s"), State.Iter + 1, *State.Watch.GetLast().Summary());
						return EGameTestBlockStep::Pending;
					}
					if (Verdict == FGameTestCondWatch::EVerdict::NotHolds)
					{
						return Finish(State, TEXT("while"), Out);
					}
					State.Phase = ELoopPhase::Prepare;
					break;
				}
				case ELoopPhase::Prepare:
				{
					FString Error;
					bool bWait = false;
					bool bEnd = false;
					if (!Prepare(*Data, State, InContext, InNowSeconds, bWait, bEnd, Error))
					{
						Out.Message = Error;
						return EGameTestBlockStep::Failed;
					}
					if (bEnd)
					{
						return Finish(State, TEXT("count"), Out);
					}
					if (bWait)
					{
						Out.Message = FString::Printf(TEXT("在等第 %d 个条目生成（已滚过去）"), State.Iter + 1);
						return EGameTestBlockStep::Pending;
					}
					State.Phase = ELoopPhase::AfterBody;
					Out.List = Data->List;
					Out.Iter = State.Iter;
					return EGameTestBlockStep::RunList;
				}
				case ELoopPhase::AfterBody:
					if (State.bBreak)
					{
						State.Iter++;
						return Finish(State, TEXT("break"), Out);
					}
					if (Data->Mode == ELoopMode::Until)
					{
						State.Watch.Start(Data->Cond, InContext, true);
						State.Phase = ELoopPhase::CheckUntil;
					}
					else
					{
						State.Iter++;
						State.Phase = ELoopPhase::Gap;
					}
					break;
				case ELoopPhase::CheckUntil:
				{
					const FGameTestCondWatch::EVerdict Verdict = TickWatch(State, InContext, InNowSeconds);
					if (Verdict == FGameTestCondWatch::EVerdict::Pending)
					{
						Out.Message = FString::Printf(TEXT("第 %d 轮做完，在等 until 的条件稳住：%s"), State.Iter + 1, *State.Watch.GetLast().Summary());
						return EGameTestBlockStep::Pending;
					}
					State.Iter++;
					if (Verdict == FGameTestCondWatch::EVerdict::Holds)
					{
						return Finish(State, TEXT("until"), Out);
					}
					State.Phase = ELoopPhase::Gap;
					break;
				}
				case ELoopPhase::Gap:
					if (Data->GapMs > 0)
					{
						if (State.GapUntil < 0.0)
						{
							State.GapUntil = InNowSeconds + Data->GapMs / 1000.0;
						}
						if (InNowSeconds < State.GapUntil)
						{
							Out.Message = FString::Printf(TEXT("两轮之间隔 %d 毫秒"), Data->GapMs);
							return EGameTestBlockStep::Pending;
						}
						State.GapUntil = -1.0;
					}
					State.Phase = ELoopPhase::BeginIter;
					break;
				}
			}
			Out.Message = TEXT("loop 内部状态出错（一帧里转了太多阶段）");
			return EGameTestBlockStep::Failed;
		}

		virtual bool CatchSignal(EGameTestFlowSignal InSignal, const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext) const override
		{
			if (InSignal != EGameTestFlowSignal::Break && InSignal != EGameTestFlowSignal::Continue)
			{
				return false;
			}
			FLoopState& State = static_cast<FLoopState&>(InState);
			State.bBreak = InSignal == EGameTestFlowSignal::Break;
			// continue 等于这一轮的循环体做完了：until 照常判断。
			State.Phase = ELoopPhase::AfterBody;
			return true;
		}

		virtual void Leave(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext) const override
		{
			FLoopState& State = static_cast<FLoopState&>(InState);
			if (State.bLocals)
			{
				InContext.PopLocals();
				State.bLocals = false;
			}
		}

	private:
		static FGameTestCondWatch::EVerdict TickWatch(FLoopState& InState, FGameTestFlowContext& InContext, double InNowSeconds)
		{
			FGameTestFlowContext::FReadOnlyScope ReadOnly(InContext);
			const FGameTestCondWatch::EVerdict Verdict = InState.Watch.Tick(InContext, InNowSeconds);
			// 判断不了（反面判断的屏不在）超过 elseSettle 就按不成立算：不然界面关着时会一直等下去。
			if (InState.Watch.GetLast().State == EGameTestCondState::Unknown && InState.Watch.GetCond().IsValid())
			{
				InState.WaitSince = InState.WaitSince < 0.0 ? InNowSeconds : InState.WaitSince;
				if ((InNowSeconds - InState.WaitSince) * 1000.0 >= GameTestCond::ElseSettleMs(*InState.Watch.GetCond(), InContext))
				{
					InState.WaitSince = -1.0;
					return FGameTestCondWatch::EVerdict::NotHolds;
				}
				return FGameTestCondWatch::EVerdict::Pending;
			}
			InState.WaitSince = -1.0;
			return Verdict;
		}

		static EGameTestBlockStep Finish(FLoopState& InState, const TCHAR* InExit, FGameTestBlockTick& Out)
		{
			Out.Message = FString::Printf(TEXT("跑了 %d 轮（%s）"), InState.Iter, InExit);
			Out.Payload = MakeShared<FJsonObject>();
			Out.Payload->SetNumberField(TEXT("iterations"), InState.Iter);
			Out.Payload->SetStringField(TEXT("exit"), InExit);
			return EGameTestBlockStep::Done;
		}

		static bool Init(const FLoopData& InData, FLoopState& InState, FGameTestFlowContext& InContext, FString& OutError)
		{
			if (InData.Mode == ELoopMode::Times)
			{
				double Number = 0.0;
				const FGameTestValue Value = GameTestRefs::EvalJson(InData.Times, InContext);
				if (Value.IsUnresolved() || !Value.TryNumber(Number))
				{
					OutError = FString::Printf(TEXT("times 取不到整数：%s"), Value.IsUnresolved() ? *Value.GetString() : *Value.ToDisplay());
					return false;
				}
				InState.Count = FMath::Clamp(FMath::FloorToInt(Number), 0, LoopMaxLimit);
			}
			else if (InData.Mode == ELoopMode::In)
			{
				const FGameTestValue Value = GameTestRefs::EvalJson(InData.In, InContext);
				if (Value.IsUnresolved())
				{
					OutError = FString::Printf(TEXT("in 取不到：%s"), *Value.GetString());
					return false;
				}
				if (Value.Type != EGameTestValueType::Array)
				{
					OutError = FString::Printf(TEXT("in 要是数组，实际是 %s（%s）"), *Value.TypeName(), *Value.ToDisplay());
					return false;
				}
				// 开始时取一份：循环里改了这个变量不影响这次循环（和大多数语言的 for-in 一样）。
				InState.Items = Value.GetArray();
				InState.Count = InState.Items.Num();
			}
			else if (InData.Mode == ELoopMode::Each)
			{
				// 部件写法转成内部 target 字符串：容器解析和 target 走同一条路（GameTestRefResolve）
				FString Error;
				const TSharedRef<FJsonObject> Holder = MakeShared<FJsonObject>();
				Holder->SetField(TEXT("each"), InData.Each);
				if (!GameTestHandlers::TargetText(*Holder, TEXT("each"), InState.Container, Error))
				{
					OutError = Error;
					return false;
				}
			}
			return true;
		}

		/** 设好这一轮的循环变量。each 的条目没生成时滚过去，`bWait` = true 让执行器下一帧再来。 */
		static bool Prepare(const FLoopData& InData, FLoopState& InState, FGameTestFlowContext& InContext, double InNowSeconds, bool& OutWait, bool& OutEnd, FString& OutError)
		{
			InContext.SetLocal(TEXT("index"), FGameTestValue::FromNumber(InState.Iter));
			if (InData.Mode == ELoopMode::In)
			{
				InContext.SetLocal(InData.As, InState.Items.IsValidIndex(InState.Iter) ? InState.Items[InState.Iter] : FGameTestValue::Null());
				return true;
			}
			if (InData.Mode != ELoopMode::Each)
			{
				return true;
			}

			const FGameTestWidgetLook Look = GameTestWidgetProbe::Look(InState.Container, InContext);
			UWidget* Container = Look.bResolved && Look.Node.IsValid() ? Look.Node->Widget.Get() : nullptr;
			if (!Container)
			{
				// 容器刚打开时可能还没出来：等一会儿再算失败。
				if (InState.ContainerWaitSince < 0.0)
				{
					InState.ContainerWaitSince = InNowSeconds;
				}
				if (InNowSeconds - InState.ContainerWaitSince < EntryWaitSeconds)
				{
					OutWait = true;
					return true;
				}
				OutError = FString::Printf(TEXT("each 的容器 %s 找不到：%s"), **GameTestHandlers::DescribeTarget(InState.Container), *Look.Error);
				return false;
			}
			InState.ContainerWaitSince = -1.0;
			TArray<FGameTestKeyEntry> Entries;
			if (!GameTestItemKey::CollectEntries(Container, Entries))
			{
				OutError = FString::Printf(TEXT("each 的 %s 不是容器（列表、滚动框、面板）"), *GameTestHandlers::DescribeTarget(InState.Container));
				return false;
			}
			if (InState.Iter >= Entries.Num())
			{
				OutEnd = true;
				return true;
			}
			const FGameTestKeyEntry& Entry = Entries[InState.Iter];
			UWidget* EntryWidget = Entry.Widget.Get();
			if (!EntryWidget)
			{
				// 虚拟化列表没滚到的条目没有控件：滚过去，等它生成。
				if (UListView* List = Cast<UListView>(Container))
				{
					if (InState.EntryWaitSince < 0.0)
					{
						InState.EntryWaitSince = InNowSeconds;
						List->ScrollIndexIntoView(InState.Iter);
					}
					if (InNowSeconds - InState.EntryWaitSince < EntryWaitSeconds)
					{
						OutWait = true;
						return true;
					}
				}
				OutError = FString::Printf(TEXT("%s 的第 %d 个条目滚过去 %.0f 秒还没生成控件"), *GameTestHandlers::DescribeTarget(InState.Container), InState.Iter + 1, EntryWaitSeconds);
				return false;
			}
			InState.EntryWaitSince = -1.0;
			// v4：$item 是条目控件（引用，`{"obj": "$item", "part": "BtnClaim"}`），数据对象用 prop("$item", "@item.X") 读
			InContext.SetLocal(InData.As, FGameTestValue::FromObject(EntryWidget));
			return true;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterBlockLoop()
	{
		GameTestBlocks::Get().Register(MakeShared<FBlockLoop>());
	}
}
