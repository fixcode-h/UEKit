// Copyright Epic Games, Inc. All Rights Reserved.

// 能力系统接口（Ability.*）：读属性、标签、技能状态和冷却，准备状态时激活技能、上效果、改属性。
// 放在可选模块里：主模块不依赖 GameplayAbilities，项目没开 GAS 时只是没有这些接口。

#include "GameTestGASApi.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Abilities/GameplayAbility.h"
#include "Api/Builtin/GameTestApiArgs.h"
#include "Api/GameTestApi.h"
#include "AttributeSet.h"
#include "Core/Script/GameTestFlowContext.h"
#include "GameplayEffect.h"
#include "GameplayTagContainer.h"
#include "Observe/Sources/GameTestObjectRoots.h"

namespace GA = GameTestApiArgs;
using GameTestApiArgs::Opt;
using GameTestApiArgs::Req;

namespace
{
	using FCall = FGameTestApiCall;
	using FCtx = FGameTestFlowContext;
	using EState = EGameTestCallState;

	const TCHAR* ModuleName = TEXT("GameTestGAS");

	UAbilitySystemComponent* NeedAsc(FCall& C, FCtx& X)
	{
		FString Error;
		AActor* Actor = GA::Actor(C.Arg(0), X, false, Error);
		if (Actor == nullptr)
		{
			C.Fail(TEXT("arg_object"), FString::Printf(TEXT("第 1 个参数 obj：%s"), *Error));
			return nullptr;
		}
		// 认 IAbilitySystemInterface 和直接挂的组件（PlayerState 上的 ASC 由 Pawn 的接口转过去）
		UAbilitySystemComponent* Asc = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Actor);
		if (Asc == nullptr)
		{
			C.Fail(TEXT("arg_object"), FString::Printf(TEXT("%s 没有能力系统组件（ASC）"), *GA::NameOf(Actor)));
		}
		return Asc;
	}

	/** `"Set.Attr"`（属性集类名，前缀和 AttributeSet 后缀可省）或只写 `"Attr"`。 */
	bool FindAttribute(const UAbilitySystemComponent& InAsc, const FString& InName, FGameplayAttribute& OutAttribute)
	{
		FString SetName, AttrName = InName;
		InName.Split(TEXT("."), &SetName, &AttrName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		for (const UAttributeSet* Set : InAsc.GetSpawnedAttributes())
		{
			if (Set == nullptr)
			{
				continue;
			}
			if (!SetName.IsEmpty() && !GameTestObjectRoots::ClassMatches(Set->GetClass(), SetName)
				&& !GameTestObjectRoots::ClassMatches(Set->GetClass(), SetName + TEXT("AttributeSet")))
			{
				continue;
			}
			for (TFieldIterator<FProperty> It(Set->GetClass()); It; ++It)
			{
				if (It->GetName().Equals(AttrName, ESearchCase::IgnoreCase) && FGameplayAttribute::IsGameplayAttributeDataProperty(*It))
				{
					OutAttribute = FGameplayAttribute(*It);
					return true;
				}
			}
		}
		return false;
	}

	bool NeedAttribute(FCall& C, const UAbilitySystemComponent& InAsc, FGameplayAttribute& OutAttribute)
	{
		const FString Name = GA::Text(C.Arg(1));
		if (FindAttribute(InAsc, Name, OutAttribute))
		{
			return true;
		}
		C.Fail(TEXT("path_missing"), FString::Printf(TEXT("%s 的属性集里没有 %s（写 属性集.属性，如 Health.Current）"),
			*GA::NameOf(InAsc.GetOwner()), *Name));
		return false;
	}

	/** 标签参数：不认识的标签报错（拼错了比静默 false 好查）。 */
	bool NeedTag(FCall& C, int32 InIndex, FGameplayTag& OutTag)
	{
		const FString Name = GA::Text(C.Arg(InIndex));
		OutTag = FGameplayTag::RequestGameplayTag(FName(*Name), false);
		if (!OutTag.IsValid())
		{
			GA::ArgError(C, InIndex, TEXT("tag"), FString::Printf(TEXT("没有标签 %s（看项目的 GameplayTag 列表）"), *Name));
			return false;
		}
		return true;
	}

	/**
	 * 技能自己的标签。为什么用反射读：5.5 起 AbilityTags 改成了 GetAssetTags()，两版写法不同；
	 * 属性名两版都是 AbilityTags，反射读一份代码两版都能编。
	 */
	const FGameplayTagContainer* AbilityTags(const UGameplayAbility& InAbility)
	{
		static const FName Name(TEXT("AbilityTags"));
		const FStructProperty* Prop = CastField<FStructProperty>(InAbility.GetClass()->FindPropertyByName(Name));
		return Prop && Prop->Struct == FGameplayTagContainer::StaticStruct() ? Prop->ContainerPtrToValuePtr<FGameplayTagContainer>(&InAbility) : nullptr;
	}

	/** 按技能类名或标签挑技能；第 2 个参数是 `abilityCls 或 tag`。 */
	TArray<FGameplayAbilitySpec*> PickAbilities(UAbilitySystemComponent& InAsc, const FString& InWhich)
	{
		const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*InWhich), false);
		TArray<FGameplayAbilitySpec*> Out;
		for (FGameplayAbilitySpec& Spec : InAsc.GetActivatableAbilities())
		{
			const UGameplayAbility* Ability = Spec.Ability;
			if (Ability == nullptr)
			{
				continue;
			}
			const FGameplayTagContainer* Tags = AbilityTags(*Ability);
			if ((Tag.IsValid() && Tags && Tags->HasTag(Tag)) || GameTestObjectRoots::ClassMatches(Ability->GetClass(), InWhich))
			{
				Out.Add(&Spec);
			}
		}
		return Out;
	}

	void Add(const TCHAR* InName, const TCHAR* InSummary, TArray<FGameTestApiArg>&& InArgs, const TCHAR* InReturns, bool bReadOnly,
		FGameTestLambdaApi::FCallFn&& InFn)
	{
		FGameTestApiDesc Desc;
		Desc.Name = InName;
		Desc.Category = TEXT("能力系统");
		Desc.Summary = InSummary;
		Desc.Args = MoveTemp(InArgs);
		Desc.Returns = InReturns;
		Desc.bReadOnly = bReadOnly;
		Desc.Module = ModuleName;
		GameTestApis::Get().Register(MakeShared<FGameTestLambdaApi>(MoveTemp(Desc), MoveTemp(InFn)));
	}
}

namespace GameTestGASApi
{
	void Register()
	{
		Add(TEXT("Ability.Attribute"), TEXT("属性当前值（含效果加成）"),
			{ Req(TEXT("obj"), TEXT("obj"), TEXT("@pawn、Actor 或 PlayerState")), Req(TEXT("attr"), TEXT("string"), TEXT("Health.Current 或 Current")) }, TEXT("number"), true,
			[](FCall& C, FCtx& X)
			{
				UAbilitySystemComponent* Asc = NeedAsc(C, X);
				FGameplayAttribute Attribute;
				if (Asc == nullptr || !NeedAttribute(C, *Asc, Attribute))
				{
					return EState::Failed;
				}
				return C.Done(FGameTestValue::FromNumber(Asc->GetNumericAttribute(Attribute)));
			});
		Add(TEXT("Ability.HasTag"), TEXT("身上有没有这个 GameplayTag（含父标签匹配：有 State.Stun.Hard 时问 State.Stun 也算）"),
			{ Req(TEXT("obj"), TEXT("obj")), Req(TEXT("tag"), TEXT("string")) }, TEXT("bool"), true,
			[](FCall& C, FCtx& X)
			{
				UAbilitySystemComponent* Asc = NeedAsc(C, X);
				FGameplayTag Tag;
				if (Asc == nullptr || !NeedTag(C, 1, Tag))
				{
					return EState::Failed;
				}
				return C.Done(FGameTestValue::FromBool(Asc->HasMatchingGameplayTag(Tag)));
			});
		Add(TEXT("Ability.IsActive"), TEXT("技能在不在释放中（按技能类名或技能标签）"),
			{ Req(TEXT("obj"), TEXT("obj")), Req(TEXT("ability"), TEXT("string"), TEXT("GA_Dash 或 Ability.Dash")) }, TEXT("bool"), true,
			[](FCall& C, FCtx& X)
			{
				UAbilitySystemComponent* Asc = NeedAsc(C, X);
				if (Asc == nullptr)
				{
					return EState::Failed;
				}
				const TArray<FGameplayAbilitySpec*> Specs = PickAbilities(*Asc, GA::Text(C.Arg(1)));
				if (Specs.Num() == 0)
				{
					return C.Fail(TEXT("object_not_found"), FString::Printf(TEXT("%s 没有授予技能 %s"), *GA::NameOf(Asc->GetOwner()), *GA::Text(C.Arg(1))));
				}
				bool bActive = false;
				for (const FGameplayAbilitySpec* Spec : Specs)
				{
					bActive |= Spec->IsActive();
				}
				return C.Done(FGameTestValue::FromBool(bActive));
			});
		Add(TEXT("Ability.Cooldown"), TEXT("冷却剩余秒数（按冷却标签；没在冷却给 0）"),
			{ Req(TEXT("obj"), TEXT("obj")), Req(TEXT("tag"), TEXT("string"), TEXT("Cooldown.Dash")) }, TEXT("number"), true,
			[](FCall& C, FCtx& X)
			{
				UAbilitySystemComponent* Asc = NeedAsc(C, X);
				FGameplayTag Tag;
				if (Asc == nullptr || !NeedTag(C, 1, Tag))
				{
					return EState::Failed;
				}
				const FGameplayEffectQuery Query = FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(Tag));
				float Remaining = 0.f;
				for (const float Time : Asc->GetActiveEffectsTimeRemaining(Query))
				{
					Remaining = FMath::Max(Remaining, Time);
				}
				return C.Done(FGameTestValue::FromNumber(Remaining));
			});
		Add(TEXT("Ability.Activate"), TEXT("尝试激活技能（按类名或标签），返回激活成功没有（准备状态用；测按键用 ability.input）"),
			{ Req(TEXT("obj"), TEXT("obj")), Req(TEXT("ability"), TEXT("string")) }, TEXT("bool"), false,
			[](FCall& C, FCtx& X)
			{
				UAbilitySystemComponent* Asc = NeedAsc(C, X);
				if (Asc == nullptr)
				{
					return EState::Failed;
				}
				const TArray<FGameplayAbilitySpec*> Specs = PickAbilities(*Asc, GA::Text(C.Arg(1)));
				if (Specs.Num() == 0)
				{
					return C.Fail(TEXT("object_not_found"), FString::Printf(TEXT("%s 没有授予技能 %s"), *GA::NameOf(Asc->GetOwner()), *GA::Text(C.Arg(1))));
				}
				bool bOk = false;
				for (const FGameplayAbilitySpec* Spec : Specs)
				{
					bOk |= Asc->TryActivateAbility(Spec->Handle);
				}
				return C.Done(FGameTestValue::FromBool(bOk));
			});
		Add(TEXT("Ability.ApplyEffect"), TEXT("给自己上一个 GameplayEffect（准备状态用：加血、加 Buff）"),
			{ Req(TEXT("obj"), TEXT("obj")), Req(TEXT("effect"), TEXT("cls"), TEXT("GE_Heal")), Opt(TEXT("level"), TEXT("number"), TEXT("默认 1")) },
			TEXT(""), false,
			[](FCall& C, FCtx& X)
			{
				UAbilitySystemComponent* Asc = NeedAsc(C, X);
				if (Asc == nullptr)
				{
					return EState::Failed;
				}
				FString Error;
				UClass* Class = GA::Class(C.Arg(1), Error);
				if (Class == nullptr || !Class->IsChildOf(UGameplayEffect::StaticClass()))
				{
					GA::ArgError(C, 1, TEXT("effect"), Class ? FString::Printf(TEXT("%s 不是 GameplayEffect"), *Class->GetName()) : Error);
					return EState::Failed;
				}
				double Level = 1.0;
				GA::Number(C.Arg(2), 1.0, Level);
				const FActiveGameplayEffectHandle Handle = Asc->BP_ApplyGameplayEffectToSelf(Class, static_cast<float>(Level), Asc->MakeEffectContext());
				// 瞬时效果（Instant）执行完没有句柄，也算成功；只有被免疫、条件不满足时才是真没上
				return C.Done(FGameTestValue::FromBool(Handle.WasSuccessfullyApplied()));
			});
		Add(TEXT("Ability.SetAttribute"), TEXT("改属性的基础值（准备状态用：把血量、体力设好）"),
			{ Req(TEXT("obj"), TEXT("obj")), Req(TEXT("attr"), TEXT("string")), Req(TEXT("value"), TEXT("number")) }, TEXT(""), false,
			[](FCall& C, FCtx& X)
			{
				UAbilitySystemComponent* Asc = NeedAsc(C, X);
				FGameplayAttribute Attribute;
				double Value = 0.0;
				if (Asc == nullptr || !NeedAttribute(C, *Asc, Attribute))
				{
					return EState::Failed;
				}
				if (!C.Arg(2).TryNumber(Value))
				{
					GA::ArgError(C, 2, TEXT("value"), TEXT("要数字"));
					return EState::Failed;
				}
				Asc->SetNumericAttributeBase(Attribute, static_cast<float>(Value));
				return C.Done(FGameTestValue::Null());
			});
	}

	void Unregister()
	{
		GameTestApis::RemoveWhere([](const FGameTestApiDesc& InDesc) { return InDesc.Module == ModuleName; });
	}
}
