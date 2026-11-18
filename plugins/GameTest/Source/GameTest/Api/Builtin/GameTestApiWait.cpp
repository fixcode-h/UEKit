// Copyright Epic Games, Inc. All Rights Reserved.

// 自带接口：等待（协议第 8.1 节 Wait.*）。都是跨帧接口（bMayWait）：只能当 api 步骤，条件和表达式里不能用。
// 步骤的 timeoutMs 管总超时；接口自己的 timeoutMs 参数到了按失败算（写清在等什么）。

#include "Api/Builtin/GameTestApiArgs.h"
#include "Api/Builtin/GameTestApiLoading.h"
#include "Api/GameTestApi.h"
#include "Core/GameTestWorld.h"
#include "Core/Script/GameTestFlowContext.h"

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
		const double Until = FPlatformTime::Seconds() + Amount;
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
		const double Deadline = TimeoutMs > 0.0 ? FPlatformTime::Seconds() + TimeoutMs / 1000.0 : 0.0;
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
		const EState Now = Check(FPlatformTime::Seconds());
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
	}
}
