// Copyright Epic Games, Inc. All Rights Reserved.

// obj.wait（docs/game-test-ref.md 5.3）：`wait until=cond` 的简写，等到这个对象的判断成立。
// 判断和条件里的对象判断（GameTestCheckObj）是同一份：拼成 {obj, prop, op, value…} 交给 wait 的条件等待。

#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestRunner.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"

namespace GameTestHandlers
{
	EGameTestStepStatus HandleObjWait(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;
		const TSharedRef<FJsonObject> Cond = MakeShared<FJsonObject>();
		for (const TCHAR* Field : { TEXT("obj"), TEXT("prop"), TEXT("op"), TEXT("value"), TEXT("tol"), TEXT("ignoreCase"), TEXT("cmp"), TEXT("settleMs") })
		{
			if (const TSharedPtr<FJsonValue> Value = Params.TryGetField(Field))
			{
				Cond->SetField(Field, Value);
			}
		}
		// 等界面出现、消失和点击的自动等待同一个上限（默认不限，步骤 timeoutMs 优先）
		InContext.DefaultTimeoutMs = InContext.UiWaitDefault();
		return StartWaitFor(InContext, Cond);
	}
}
