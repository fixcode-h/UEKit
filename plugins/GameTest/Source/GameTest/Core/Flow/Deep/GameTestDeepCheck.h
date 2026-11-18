// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Script/GameTestRegistry.h"

class FGameTestDiagSink;
class FGameTestFlowContext;
class FJsonObject;
struct FGameTestScript;

/** 深度检查查了多少东西（`gt.validate` 的 `checked`）。 */
struct GAMETEST_API FGameTestDeepStats
{
	int32 Functions = 0;
	int32 Selectors = 0;
	int32 Paths = 0;
	int32 Apis = 0;
	int32 Objects = 0;
};

/** 一次深度检查的环境：检查器往 Sink 里写错误（路径、id 由遍历设好），往 Stats 里记数。 */
struct GAMETEST_API FGameTestDeepEnv
{
	FGameTestFlowContext& Context;
	FGameTestDiagSink& Sink;
	FGameTestDeepStats& Stats;
	const FGameTestScript& Script;
	/** 游戏开着（能看运行时的控件、对象），没开时只查资产和类。 */
	bool bGameRunning = false;
};

/**
 * 深度检查（`gt.validate` 的 `deep: true`，协议第 10 节）：结构对了以后，再查写的东西在项目里有没有——
 * 函数、接口、选择器指的界面和控件、取值来源里的类名、对象定位器的类。
 *
 * 一种东西一个检查器（接口 + 注册表）：遍历只做一件事——把场景里每个 JSON 对象（不含子步骤列表）交给每个检查器，
 * 检查器自己认字段。编辑器专属的（读 WBP 资产的控件树）由 GameTestEditor 登记，游戏进程里没有它也照常查其余的。
 */
class GAMETEST_API IGameTestDeepCheck
{
public:
	virtual ~IGameTestDeepCheck() = default;

	virtual FName GetKey() const = 0;

	/** 场景里的一个 JSON 对象（步骤、条件、参数对象……）。认不出的字段不管。 */
	virtual void CheckObject(const FJsonObject& InJson, FGameTestDeepEnv& InEnv) const = 0;

	/** 整个场景查一次（objects 定义、全局守护之类不在步骤里的）。 */
	virtual void CheckScript(const FGameTestScript& InScript, FGameTestDeepEnv& InEnv) const {}
};

namespace GameTestDeepChecks
{
	GAMETEST_API TGameTestRegistry<IGameTestDeepCheck>& Get();

	/** 遍历场景（setup、steps、各层块的子步骤、节点上的守护），每个 JSON 对象交给每个检查器。 */
	GAMETEST_API void Run(const FGameTestScript& InScript, FGameTestFlowContext& InContext, FGameTestDiagSink& InSink, FGameTestDeepStats& OutStats);

	GAMETEST_API TSharedRef<FJsonObject> StatsToJson(const FGameTestDeepStats& InStats);
}
