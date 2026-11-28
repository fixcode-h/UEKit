// Copyright Epic Games, Inc. All Rights Reserved.

// 自带接口：等待（协议第 8.1 节 Wait.*）。都是跨帧接口（bMayWait）：只能当 api 步骤，条件和表达式里不能用。
// 步骤的 timeoutMs 管总超时；接口自己的 timeoutMs 参数到了按失败算（写清在等什么）。

#include "Api/Builtin/GameTestApiArgs.h"
#include "Core/GameTestClock.h"
#include "Api/Builtin/GameTestApiLoading.h"
#include "Api/GameTestApi.h"
#include "Core/GameTestWorld.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Checks/GameTestCond.h"

namespace GA = GameTestApiArgs;
using GameTestApiArgs::Opt;
using GameTestApiArgs::Req;

namespace
{
	using FCall = FGameTestApiCall;
	using FCtx = FGameTestFlowContext;
	using EState = EGameTestCallState;

	EState WaitSeconds(FCall& C, FCtx& X)
	{
		double Amount = 0.0;
		if (!C.Arg(0).TryNumber(Amount) || Amount < 0.0)
		{
			GA::ArgError(C, 0, TEXT("n"), TEXT("要不小于 0 的秒数"));
			return EState::Failed;
		}
		const double Until = GameTestClock::Now() + Amount;
		FCall* Call = &C;
		C.Poll = [Call, Until](double InNow)
		{
			return InNow >= Until ? Call->Done(FGameTestValue::Null()) : EState::Pending;
		};
		return EState::Pending;
	}

	EState WaitFrames(FCall& C, FCtx&)
	{
		double Amount = 0.0;
		if (!C.Arg(0).TryNumber(Amount) || Amount < 0.0)
		{
			GA::ArgError(C, 0, TEXT("n"), TEXT("要不小于 0 的帧数"));
			return EState::Failed;
		}
		// 按引擎帧号数，不按 Poll 次数：执行器某帧没轮到这一步时也不会少等
		const uint64 Until = GFrameCounter + static_cast<uint64>(Amount);
		FCall* Call = &C;
		C.Poll = [Call, Until](double)
		{
			return GFrameCounter >= Until ? Call->Done(FGameTestValue::Null()) : EState::Pending;
		};
		return EState::Pending;
	}

	EState WaitLoading(FCall& C, FCtx&)
	{
		double TimeoutMs = 0.0;
		if (!GA::Number(C.Arg(0), 0.0, TimeoutMs))
		{
			GA::ArgError(C, 0, TEXT("timeoutMs"), TEXT("要毫秒数"));
			return EState::Failed;
		}
		const double Deadline = TimeoutMs > 0.0 ? GameTestClock::Now() + TimeoutMs / 1000.0 : 0.0;
		FCall* Call = &C;
		auto Check = [Call, Deadline](double InNow)
		{
			FString Why;
			if (GameTestApiLoading::IsDone(GameTestWorld::FindGameWorld(), Why))
			{
				return Call->Done(FGameTestValue::Null());
			}
			if (Deadline > 0.0 && InNow >= Deadline)
			{
				return Call->Fail(TEXT("timeout"), FString::Printf(TEXT("等加载超时：%s"), *Why));
			}
			return EState::Pending;
		};
		const EState Now = Check(GameTestClock::Now());
		if (Now == EState::Pending)
		{
			C.Poll = Check;
		}
		return Now;
	}

	EState WaitUntil(FCall& C, FCtx& X)
	{
		// cond 参数由 api 步骤原样传进来（没先求值），这里每帧重新判：$变量、表达式要看此刻的值
		const TSharedPtr<FJsonObject> Cond = GameTestCond::AsObject(C.Arg(0).ToJson());
		if (!Cond.IsValid())
		{
			GA::ArgError(C, 0, TEXT("cond"), TEXT("要写统一条件（{target, visible: true}、{expr: \"=…\"}、{all: […]}）"));
			return EState::Failed;
		}
		double TimeoutMs = 0.0;
		if (!GA::Number(C.Arg(1), 0.0, TimeoutMs))
		{
			GA::ArgError(C, 1, TEXT("timeoutMs"), TEXT("要毫秒数"));
			return EState::Failed;
		}
		// 和 if / waitUntil 块同一套「稳住」规则：成立要持续 settleMs 才算，不被一闪而过的状态骗
		const TSharedRef<FGameTestCondWatch> Watch = MakeShared<FGameTestCondWatch>();
		Watch->Start(Cond, X, false);
		const double Deadline = TimeoutMs > 0.0 ? GameTestClock::Now() + TimeoutMs / 1000.0 : 0.0;
		FCall* Call = &C;
		FCtx* Context = &X;
		auto Check = [Call, Context, Watch, Deadline](double InNow)
		{
			if (Watch->Tick(*Context, InNow) == FGameTestCondWatch::EVerdict::Holds)
			{
				if (Context->StepPayload.IsValid())
				{
					Context->StepPayload->SetObjectField(TEXT("cond"), Watch->GetLast().ToJson());
				}
				return Call->Done(FGameTestValue::FromBool(true));
			}
			if (Deadline > 0.0 && InNow >= Deadline)
			{
				return Call->Fail(TEXT("timeout"), FString::Printf(TEXT("等条件超时：%s"), *Watch->GetLast().Summary()));
			}
			return EState::Pending;
		};
		const EState Now = Check(GameTestClock::Now());
		if (Now == EState::Pending)
		{
			C.Poll = Check;
		}
		return Now;
	}
}


namespace GameTestBuiltin
{
	void RegisterApiWait()
	{
		const TCHAR* Cat = TEXT("等待");
		GameTestApis::Add(TEXT("Wait.Seconds"), Cat, TEXT("等几秒（真实时间）"), { Req(TEXT("n"), TEXT("number")) }, TEXT(""), false, &WaitSeconds, true);
		GameTestApis::Add(TEXT("Wait.Frames"), Cat, TEXT("等几帧"), { Req(TEXT("n"), TEXT("int")) }, TEXT(""), false, &WaitFrames, true);
		GameTestApis::Add(TEXT("Wait.Loading"), Cat,
			TEXT("等地图、该加载的流送关卡加载完，且没有异步加载中的包（切图、传送后用）"),
			{ Opt(TEXT("timeoutMs"), TEXT("int"), TEXT("省略按步骤的 timeoutMs")) }, TEXT(""), false, &WaitLoading, true);
		GameTestApis::Add(TEXT("Wait.Until"), Cat, TEXT("等统一条件成立（稳住规则同 if 块）；超时失败并写出最后一次各项的实际值"),
			{ Req(TEXT("cond"), TEXT("cond")), Opt(TEXT("timeoutMs"), TEXT("int"), TEXT("省略按步骤的 timeoutMs")) }, TEXT("bool"), false, &WaitUntil, true);
	}
}
