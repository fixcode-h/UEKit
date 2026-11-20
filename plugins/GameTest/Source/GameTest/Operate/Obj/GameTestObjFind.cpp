// Copyright Epic Games, Inc. All Rights Reserved.

// obj.find（docs/game-test-ref.md 5.3）：按引用找到对象，存进变量（这次运行里的弱引用）。取代 1.x 的 find（捕获句柄）。

#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestRunner.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Observe/Ref/GameTestRefResolve.h"
#include "Operate/Obj/GameTestObjActions.h"

namespace GameTestHandlers
{
	EGameTestStepStatus HandleObjFind(FGameTestStepContext& InContext)
	{
		FString SaveAs;
		InContext.Step->Params->TryGetStringField(TEXT("saveAs"), SaveAs);
		if (SaveAs.IsEmpty())
		{
			InContext.Message = TEXT("obj.find 要写 saveAs（存到哪个变量，之后写 \"$变量\" 引用它）");
			return StepFailed;
		}
		return GameTestObjActions::RunWhenFound(InContext, TEXT("存进变量"), [SaveAs](FGameTestStepContext& InStep, UObject* InObject)
		{
			InStep.Flow->SetVar(SaveAs, FGameTestValue::FromObject(InObject));
			InStep.Payload->SetStringField(TEXT("saveAs"), SaveAs);
			InStep.Message = FString::Printf(TEXT("找到 %s，存进 $%s"), *GameTestRefResolve::DisplayName(InObject), *SaveAs);
			return StepSucceeded;
		});
	}
}
