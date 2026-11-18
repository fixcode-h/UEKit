// Copyright Epic Games, Inc. All Rights Reserved.

#include "Api/Builtin/GameTestApiArgs.h"

#include "Api/GameTestApi.h"
#include "Components/ActorComponent.h"
#include "Components/Widget.h"
#include "Core/GameTestWorld.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Observe/GameTestWidgetNode.h"
#include "Observe/Sources/GameTestObjectLocator.h"
#include "Observe/Sources/GameTestObjectRoots.h"
#include "Observe/Sources/GameTestPathText.h"

namespace
{
	const TCHAR* ObjectForms = TEXT("对象参数写 @来源（@pawn、@sub.类名）、{\"$obj\": 名字}、$变量 或 {\"target\": 选择器}");

	/** `{"target": sel}` 里的选择器；不是这种写法返回 false。 */
	bool TryTarget(const FGameTestValue& InArg, FString& OutSelector)
	{
		FGameTestValue Target;
		if (InArg.Type != EGameTestValueType::Map || !InArg.TryGetField(TEXT("target"), Target))
		{
			return false;
		}
		OutSelector = GameTestApiArgs::Text(Target);
		return true;
	}

	/** `@来源…` 字符串先解析成值（`"@pawn"` 作为参数时还是字符串：填值求值只认 `$` 和 `=`）。 */
	FGameTestValue Expand(const FGameTestValue& InArg, FGameTestFlowContext& InContext)
	{
		if (InArg.IsString() && InArg.GetString().StartsWith(TEXT("@")))
		{
			return GameTestPathText::ResolveSourceText(InArg.GetString(), InContext);
		}
		return InArg;
	}
}

namespace GameTestApiArgs
{
	UObject* Object(const FGameTestValue& InArg, FGameTestFlowContext& InContext, FString& OutError)
	{
		FString Selector;
		if (TryTarget(InArg, Selector))
		{
			const FGameTestWidgetLook Look = GameTestWidgetProbe::Look(Selector, InContext);
			UWidget* Widget = Look.bResolved && Look.Node.IsValid() ? Look.Node->Widget.Get() : nullptr;
			if (Widget == nullptr)
			{
				OutError = Look.Error.IsEmpty() ? FString::Printf(TEXT("控件 %s 不在"), *Selector) : Look.Error;
			}
			return Widget;
		}

		const FGameTestValue Value = Expand(InArg, InContext);
		switch (Value.Type)
		{
		case EGameTestValueType::Object:
			if (UObject* Found = Value.GetObject())
			{
				return Found;
			}
			OutError = TEXT("对象已经销毁了（切图、关界面之后要重新取）");
			return nullptr;
		case EGameTestValueType::Unresolved:
			OutError = Value.GetString();
			return nullptr;
		case EGameTestValueType::Null:
			OutError = TEXT("对象是空的（None）");
			return nullptr;
		case EGameTestValueType::String:
			if (Value.GetString().StartsWith(TEXT("/")))
			{
				// 只找已经加载的：参数里给路径通常是界面类、数据资产，没加载说明写错了或者该用 Asset.Load。
				if (UObject* Found = FindObject<UObject>(nullptr, *Value.GetString()))
				{
					return Found;
				}
				OutError = FString::Printf(TEXT("%s 没有加载（资产先用 Asset.Load 加载）"), *Value.GetString());
				return nullptr;
			}
			break;
		default:
			break;
		}
		OutError = FString::Printf(TEXT("%s，收到 %s %s"), ObjectForms, *Value.TypeName(), *Value.ToDisplay(60));
		return nullptr;
	}

	AActor* Actor(const FGameTestValue& InArg, FGameTestFlowContext& InContext, bool bDefaultPawn, FString& OutError)
	{
		if (InArg.IsNull() && bDefaultPawn)
		{
			if (InContext.World == nullptr)
			{
				OutError = TEXT("游戏没在跑");
				return nullptr;
			}
			return Cast<AActor>(GameTestObjectRoots::Player(*InContext.World, TEXT("pawn"), OutError));
		}
		UObject* Found = Object(InArg, InContext, OutError);
		if (Found == nullptr)
		{
			return nullptr;
		}
		if (AActor* AsActor = Cast<AActor>(Found))
		{
			return AsActor;
		}
		if (const UActorComponent* Component = Cast<UActorComponent>(Found))
		{
			return Component->GetOwner();
		}
		OutError = FString::Printf(TEXT("%s 不是 Actor（是 %s）"), *NameOf(Found), *Found->GetClass()->GetName());
		return nullptr;
	}

	UClass* Class(const FGameTestValue& InArg, FString& OutError)
	{
		if (UClass* AsClass = Cast<UClass>(InArg.GetObject()))
		{
			return AsClass;
		}
		const FString Name = Text(InArg);
		if (Name.IsEmpty())
		{
			OutError = TEXT("要写类名（BP_X_C、ANpc，前缀可省）或类路径");
			return nullptr;
		}
		UClass* Found = GameTestObjectRoots::FindClass(Name);
		if (Found == nullptr)
		{
			OutError = FString::Printf(TEXT("找不到类 %s（蓝图类要先加载，写 BP_X_C 或 /Game/…/BP_X.BP_X_C）"), *Name);
		}
		return Found;
	}

	bool Locator(const FGameTestValue& InArg, FGameTestObjectLocator& OutLocator, FString& OutError)
	{
		if (InArg.IsString())
		{
			OutLocator.Class = InArg.GetString();
			if (OutLocator.Class.IsEmpty())
			{
				OutError = TEXT("定位器的类名是空的");
				return false;
			}
			return true;
		}
		if (InArg.Type != EGameTestValueType::Map)
		{
			OutError = FString::Printf(TEXT("定位器写类名或 {class, in?, key?, component?, near?}，收到 %s"), *InArg.TypeName());
			return false;
		}
		const TSharedPtr<FJsonValue> Json = InArg.ToJson();
		const TSharedPtr<FJsonObject> Object = Json.IsValid() ? Json->AsObject() : nullptr;
		TArray<FString> Errors;
		if (!Object.IsValid() || !FGameTestObjectLocator::Parse(*Object, OutLocator, Errors))
		{
			OutError = Errors.Num() > 0 ? FString::Join(Errors, TEXT("；")) : TEXT("定位器写错了");
			return false;
		}
		return true;
	}

	bool Vector(const FGameTestValue& InArg, FGameTestFlowContext& InContext, FVector& OutVector, FString& OutError)
	{
		const FGameTestValue Value = Expand(InArg, InContext);
		switch (Value.Type)
		{
		case EGameTestValueType::Vector:
			OutVector = Value.GetVector();
			return true;
		case EGameTestValueType::Array:
		{
			const TArray<FGameTestValue>& Items = Value.GetArray();
			if (Items.Num() == 3 && Items[0].TryNumber(OutVector.X) && Items[1].TryNumber(OutVector.Y) && Items[2].TryNumber(OutVector.Z))
			{
				return true;
			}
			break;
		}
		case EGameTestValueType::Map:
		{
			FGameTestValue X, Y, Z;
			if (Value.TryGetField(TEXT("X"), X) && Value.TryGetField(TEXT("Y"), Y) && Value.TryGetField(TEXT("Z"), Z)
				&& X.TryNumber(OutVector.X) && Y.TryNumber(OutVector.Y) && Z.TryNumber(OutVector.Z))
			{
				return true;
			}
			break;
		}
		case EGameTestValueType::Object:
		{
			FString Error;
			if (const AActor* AsActor = Actor(Value, InContext, false, Error))
			{
				OutVector = AsActor->GetActorLocation();
				return true;
			}
			OutError = Error;
			return false;
		}
		case EGameTestValueType::Unresolved:
			OutError = Value.GetString();
			return false;
		default:
			break;
		}
		OutError = FString::Printf(TEXT("位置写 [x, y, z]、{X, Y, Z}、Actor 或 @pawn，收到 %s %s"), *Value.TypeName(), *Value.ToDisplay(60));
		return false;
	}

	FGameTestWidgetLook Widget(const FGameTestValue& InArg, FGameTestFlowContext& InContext)
	{
		FString Selector;
		if (!TryTarget(InArg, Selector))
		{
			Selector = Text(InArg);
		}
		if (Selector.IsEmpty())
		{
			FGameTestWidgetLook Empty;
			Empty.Error = TEXT("控件参数要写选择器（WBP_Shop:Root/BtnBuy）或 {\"target\": 选择器}");
			return Empty;
		}
		return GameTestWidgetProbe::Look(Selector, InContext);
	}

	FString Text(const FGameTestValue& InArg, const FString& InDefault)
	{
		if (InArg.IsNull())
		{
			return InDefault;
		}
		return InArg.IsString() ? InArg.GetString() : InArg.ToDisplay(100000);
	}

	bool Number(const FGameTestValue& InArg, double InDefault, double& OutNumber)
	{
		if (InArg.IsNull())
		{
			OutNumber = InDefault;
			return true;
		}
		return InArg.TryNumber(OutNumber);
	}

	UWorld* World(FGameTestFlowContext& InContext, FGameTestApiCall& InOutCall)
	{
		if (InContext.World == nullptr)
		{
			InOutCall.Fail(TEXT("engine_unavailable"), TEXT("游戏没在跑：先开 PIE 或启动游戏"));
		}
		return InContext.World;
	}

	UWorld* ClientWorld(const FGameTestValue& InClient, FGameTestFlowContext& InContext, FString& OutError)
	{
		if (InClient.IsNull())
		{
			if (InContext.World == nullptr)
			{
				OutError = TEXT("游戏没在跑：先开 PIE 或启动游戏");
			}
			return InContext.World;
		}
		double Number = 0.0;
		const FString ClientText = InClient.TryNumber(Number) ? FString::FromInt(static_cast<int32>(Number)) : Text(InClient);
		return GameTestWorld::ResolveWorld(ClientText, true, OutError);
	}

	void ArgError(FGameTestApiCall& InOutCall, int32 InIndex, const TCHAR* InName, const FString& InWhy)
	{
		InOutCall.Fail(TEXT("arg_type"), FString::Printf(TEXT("第 %d 个参数 %s：%s"), InIndex + 1, InName, *InWhy));
	}

	FString NameOf(const UObject* InObject)
	{
		return InObject == nullptr ? FString(TEXT("None")) : GameTestObjectRoots::DisplayName(InObject);
	}
}
