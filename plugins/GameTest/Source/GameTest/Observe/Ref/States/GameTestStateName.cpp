// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

namespace GameTestStateNameImpl
{
	/**
	 * `:name`：对象名（GetName，UMG_Main_C_0 这种）。面板「抓取值」把某一个实例当值填进对象属性时，
	 * 存成「类 + :name 等于它」现找（2.2.2）；名字只在这次运行里稳定，不建议写进长期脚本当标识。
	 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("name");
		}

		virtual EGameTestStateScope GetScope() const override
		{
			return EGameTestStateScope::Any;
		}

		virtual EGameTestKind GetKind() const override
		{
			return EGameTestKind::String;
		}

		virtual FString GetSummary() const override
		{
			return TEXT("对象名（GetName，只在这次运行里稳定）");
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			if (InObject == nullptr)
			{
				return GameTestStateUtil::NotApplicable(InObject, TEXT("name"));
			}
			return GameTestStateUtil::Of(FGameTestValue::FromString(InObject->GetName()));
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateName()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateNameImpl::FState>());
	}
}
