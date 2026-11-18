// Copyright Epic Games, Inc. All Rights Reserved.

// 取值来源：@obj.<名字>（协议第 4.3、7 节）：场景 objects 里的对象，按定位器找（同一次运行里找到过就用缓存）。
// {"$obj": "名字"} 也走这里（GameTestRefs 调 GameTestObjectLocate::ResolveNamed，同一份逻辑）。

#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Sources/GameTestObjectLocator.h"
#include "Observe/Sources/GameTestValueSource.h"

namespace
{
	class FObjectSource final : public IGameTestValueSource
	{
	public:
		virtual FName GetKey() const override { return TEXT("obj"); }
		virtual FString GetSummary() const override { return TEXT("场景 objects 里起了名字的对象（按定位器找）"); }
		virtual FString GetSyntax() const override { return TEXT("@obj.<名字>.<属性>"); }

		virtual FGameTestValue Resolve(const FString& InSelector, TArrayView<const FGameTestPathSegment> InRest, int32& OutConsumed,
			FGameTestFlowContext& InContext) const override
		{
			OutConsumed = 0;
			if (InRest.Num() == 0 || InRest[0].Kind != FGameTestPathSegment::EKind::Member)
			{
				return FGameTestValue::Unresolved(TEXT("@obj 后面要写 objects 里的名字：@obj.shopNpc.Stock"), TEXT("expr_syntax"));
			}
			OutConsumed = 1;
			return GameTestObjectLocate::ResolveNamed(InRest[0].Name, InContext);
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterSourceObject()
	{
		GameTestValueSources::Get().Register(MakeShared<FObjectSource>());
	}
}
