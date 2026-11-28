// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Flow/Blocks/GameTestBlockUtil.h"

/**
 * `obj.do`（2.9.0）：对象调用节点。一个对象上依次调几个函数、读写几个字段（面板里点「+」一项一项加）。
 *
 * 和 gm.run 一样，GameTest 不解释 `ops`：面板存盘、运行前把 `ops` 展开成 obj.call / obj.set / set 步骤写进 `run`，这里只跑 `run`。
 * `obj`、`part`、`each`、`ops` 只给面板和报告看（面板按它们重新展开 run），所以这里只核对 run 在不在。
 */
namespace
{
	class FBlockObjDo : public IGameTestBlock
	{
	public:
		virtual FName GetKey() const override { return TEXT("obj.do"); }
		virtual FString GetSummary() const override { return TEXT("对象调用：在一个对象上依次调函数、读写字段；run 是面板按 ops 展开的步骤"); }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("obj"), TEXT("part"), TEXT("each"), TEXT("ops"), TEXT("run") };
			return Fields;
		}

		virtual void Parse(FGameTestNode& InOutNode, FGameTestScriptParser& InParser) const override
		{
			const TSharedPtr<FJsonValue> Run = InOutNode.Json->TryGetField(TEXT("run"));
			if (!Run.IsValid())
			{
				InParser.Sink().Error(TEXT("bad_type"), TEXT("obj.do 缺少 run：面板存盘时按 ops 展开，脚本是手写的就先在面板里打开存一次（或运行前让面板校验一次）"));
				return;
			}
			InParser.ParseList(InOutNode, TEXT("run"), Run, true);
		}

		virtual TSharedRef<FGameTestBlockState> Enter(const FGameTestNode& InNode, FGameTestFlowContext& InContext) const override
		{
			return MakeShared<GameTestBlockUtil::FOnceState>();
		}

		virtual EGameTestBlockStep Tick(const FGameTestNode& InNode, FGameTestBlockState& InState, FGameTestFlowContext& InContext, double InNowSeconds, FGameTestBlockTick& Out) const override
		{
			GameTestBlockUtil::FOnceState& State = static_cast<GameTestBlockUtil::FOnceState&>(InState);
			if (State.bStarted || InNode.Lists.Num() == 0)
			{
				return EGameTestBlockStep::Done;
			}
			State.bStarted = true;
			Out.List = 0;
			return EGameTestBlockStep::RunList;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterBlockObjDo()
	{
		GameTestBlocks::Get().Register(MakeShared<FBlockObjDo>());
	}
}
