// Copyright Epic Games, Inc. All Rights Reserved.

// 成员访问：UObject 走反射（协议第 4.3 节）。读属性；没有这个属性但有同名的无参只读函数（GetXxx）时调它；
// `.Fn(参数)` 调 UFUNCTION（参数打包复用 UELink 的 ue.call，见 GameTestReflectCall）。

#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Sources/GameTestReflectCall.h"
#include "Observe/Sources/GameTestReflectValue.h"
#include "Observe/Sources/GameTestValueSource.h"

#include "UObject/UnrealType.h"

namespace
{
	class FReflectMemberAccess final : public IGameTestMemberAccess
	{
	public:
		virtual FName GetKey() const override { return TEXT("reflect"); }
		virtual int32 GetPriority() const override { return 0; }

		virtual bool Handles(const FGameTestValue& InBase) const override
		{
			return InBase.GetObject() != nullptr;
		}

		virtual bool ReadMember(const FGameTestValue& InBase, const FString& InName, FGameTestFlowContext& InContext, FGameTestValue& OutValue) const override
		{
			UObject* Object = InBase.GetObject();
			if (Object == nullptr)
			{
				return false;
			}
			if (FProperty* Prop = GameTestReflectValue::FindProperty(Object->GetClass(), InName))
			{
				OutValue = GameTestReflectValue::FromProperty(Prop, Prop->ContainerPtrToValuePtr<void>(Object));
				return true;
			}
			// 没有属性、但有无参的只读函数（蓝图里常把值做成 Pure 的 GetXxx）：写 @ps.GetGold 和 @ps.GetGold() 一样
			UFunction* Function = GameTestReflectValue::FindFunction(Object->GetClass(), InName);
			if (Function != nullptr && GameTestReflectValue::IsReadOnlyFunction(*Function)
				&& Function->NumParms == (Function->GetReturnProperty() != nullptr ? 1 : 0))
			{
				const TArray<FGameTestValue> NoArgs;
				GameTestReflectCall::FArgs Args;
				Args.Positional = &NoArgs;
				OutValue = GameTestReflectCall::Call(Object, Function, Args, InContext);
				return true;
			}
			return false;
		}

		virtual bool CallMethod(const FGameTestValue& InBase, const FString& InName, const TArray<FGameTestValue>& InArgs, FGameTestFlowContext& InContext, FGameTestValue& OutValue) const override
		{
			UObject* Object = InBase.GetObject();
			UFunction* Function = Object != nullptr ? GameTestReflectValue::FindFunction(Object->GetClass(), InName) : nullptr;
			if (Function == nullptr)
			{
				return false;
			}
			GameTestReflectCall::FArgs Args;
			Args.Positional = &InArgs;
			OutValue = GameTestReflectCall::Call(Object, Function, Args, InContext);
			return true;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterMemberReflect()
	{
		GameTestMemberAccess::Register(MakeShared<FReflectMemberAccess>());
	}
}
