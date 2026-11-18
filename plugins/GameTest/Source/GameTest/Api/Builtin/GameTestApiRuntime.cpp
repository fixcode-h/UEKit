// Copyright Epic Games, Inc. All Rights Reserved.

// 自带接口：性能、随机、截图（协议第 8.1 节 Perf.* / Random.Seed / Screen.Shot）。

#include "Api/Builtin/GameTestApiArgs.h"
#include "Api/GameTestApi.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformMemory.h"
#include "Misc/Paths.h"
#include "Observe/Lua/GameTestLuaBridge.h"
#include "Shot/GameTestShot.h"

// 引擎每帧更新的平滑帧率（stat fps 显示的就是它）：比 1 / DeltaTime 稳，冒烟检查不会被一帧卡顿带偏
extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;

namespace GA = GameTestApiArgs;
using GameTestApiArgs::Opt;
using GameTestApiArgs::Req;

namespace
{
	using FCall = FGameTestApiCall;
	using FCtx = FGameTestFlowContext;
	using EState = EGameTestCallState;

	EState Seed(FCall& C, FCtx& X)
	{
		double Number = 0.0;
		if (!C.Arg(0).TryNumber(Number))
		{
			GA::ArgError(C, 0, TEXT("n"), TEXT("要整数"));
			return EState::Failed;
		}
		const int32 Value = static_cast<int32>(Number);
		X.Random.Initialize(Value);
		FMath::RandInit(Value);
		FMath::SRandInit(Value);
		// 有 Lua 时 math.randomseed 也设上：玩法逻辑常在 Lua 里掷随机数。没有 Lua 不算失败。
		if (GameTestLuaBridge::IsAvailable())
		{
			TArray<FGameTestLuaValue> Ignore;
			FString Error;
			const TArray<TSharedPtr<FJsonValue>> Keys = { MakeShared<FJsonValueString>(TEXT("math")), MakeShared<FJsonValueString>(TEXT("randomseed")) };
			const TArray<TSharedPtr<FJsonValue>> Args = { MakeShared<FJsonValueNumber>(Value) };
			if (!GameTestLuaBridge::Call(X.World, nullptr, FString(), Keys, Args, Ignore, Error) && X.StepPayload.IsValid())
			{
				X.StepPayload->SetStringField(TEXT("note"), FString::Printf(TEXT("Lua 的 math.randomseed 没设上：%s"), *Error));
			}
		}
		return C.Done(FGameTestValue::Null());
	}

	EState Shot(FCall& C, FCtx& X)
	{
		UWorld* World = GA::World(X, C);
		if (World == nullptr)
		{
			return EState::Failed;
		}
		// 同一次运行里名字可能重复（循环里截图）：加时间戳免得互相覆盖
		const FString Name = GA::Text(C.Arg(0), TEXT("shot"));
		const FString FileName = FString::Printf(TEXT("api-%s-%s.png"), *FPaths::MakeValidFileName(Name), *FDateTime::Now().ToString(TEXT("%H%M%S%s")));
		const FString Dir = X.OutputDir.IsEmpty() ? GameTestShot::GetOutputRootDir() : X.OutputDir;
		const FString Path = FPaths::Combine(Dir, FileName);
		FString Message;
		TSharedPtr<FJsonObject> Payload = X.StepPayload.IsValid() ? X.StepPayload : MakeShared<FJsonObject>();
		if (!GameTestShot::CaptureGameWindow(World, Path, nullptr, Message, Payload))
		{
			return C.Fail(TEXT("unresolved"), Message);
		}
		return C.Done(FGameTestValue::FromString(GameTestShot::ToProjectRelative(Path)));
	}
}

namespace GameTestBuiltin
{
	void RegisterApiRuntime()
	{
		const TCHAR* PerfCat = TEXT("性能");
		GameTestApis::Add(TEXT("Perf.Fps"), PerfCat, TEXT("平滑后的帧率（同 stat fps）"), {}, TEXT("number"), true,
			[](FCall& C, FCtx&) { return C.Done(FGameTestValue::FromNumber(GAverageFPS)); });
		GameTestApis::Add(TEXT("Perf.FrameMs"), PerfCat, TEXT("平滑后的每帧毫秒"), {}, TEXT("number"), true,
			[](FCall& C, FCtx&) { return C.Done(FGameTestValue::FromNumber(GAverageMS)); });
		GameTestApis::Add(TEXT("Perf.MemoryMB"), PerfCat, TEXT("进程占用的物理内存（MB）"), {}, TEXT("number"), true,
			[](FCall& C, FCtx&)
			{
				const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
				return C.Done(FGameTestValue::FromNumber(static_cast<double>(Stats.UsedPhysical) / (1024.0 * 1024.0)));
			});
		GameTestApis::Add(TEXT("Random.Seed"), TEXT("随机"), TEXT("设随机种子（引擎 FMath、脚本、Lua math.randomseed），让结果可复现"),
			{ Req(TEXT("n"), TEXT("int")) }, TEXT(""), false, &Seed);
		GameTestApis::Add(TEXT("Screen.Shot"), TEXT("截图"), TEXT("截游戏窗口，返回相对项目根的路径（同 screenshot 动作）"),
			{ Opt(TEXT("name"), TEXT("string")) }, TEXT("string"), false, &Shot);
	}
}
