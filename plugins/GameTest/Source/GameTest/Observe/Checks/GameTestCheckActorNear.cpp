// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Script/GameTestDiag.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Core/Script/GameTestRefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "Observe/Checks/GameTestCheck.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Observe/GameTestActor.h"
#include "Engine/World.h"

/**
 * 判断：Actor 到了某个点附近 `{"actorNear": {"actor": "BP_Hero_C", "point": [100, 0, 0], "radius": 200}}`。
 * 和 `wait until=actorNear` 同一个找 Actor 的办法（GameTestActor::ResolveOne）；`point` 还可以写 `$var`、`@actor[…]`、`=表达式`。
 */
namespace
{
	bool ToPoint(const FGameTestValue& InValue, FVector& OutPoint)
	{
		if (InValue.Type == EGameTestValueType::Vector)
		{
			OutPoint = InValue.GetVector();
			return true;
		}
		if (const AActor* Actor = Cast<AActor>(InValue.GetObject()))
		{
			OutPoint = Actor->GetActorLocation();
			return true;
		}
		const TArray<FGameTestValue>& Items = InValue.GetArray();
		double C[3] = { 0.0, 0.0, 0.0 };
		if (Items.Num() == 3 && Items[0].TryNumber(C[0]) && Items[1].TryNumber(C[1]) && Items[2].TryNumber(C[2]))
		{
			OutPoint = FVector(C[0], C[1], C[2]);
			return true;
		}
		FGameTestValue X;
		FGameTestValue Y;
		FGameTestValue Z;
		if (InValue.TryGetField(TEXT("X"), X) && InValue.TryGetField(TEXT("Y"), Y) && InValue.TryGetField(TEXT("Z"), Z)
			&& X.TryNumber(C[0]) && Y.TryNumber(C[1]) && Z.TryNumber(C[2]))
		{
			OutPoint = FVector(C[0], C[1], C[2]);
			return true;
		}
		return false;
	}

	/** `point` 写 "@pawn" 这种来源时要当表达式求。 */
	FGameTestValue ReadPoint(const TSharedPtr<FJsonValue>& InRaw, FGameTestFlowContext& InContext)
	{
		FString Text;
		if (InRaw.IsValid() && InRaw->TryGetString(Text) && Text.StartsWith(TEXT("@")))
		{
			return GameTestRefs::EvalJson(MakeShared<FJsonValueString>(TEXT("=") + Text), InContext);
		}
		return GameTestRefs::EvalJson(InRaw, InContext);
	}

	class FCheckActorNear : public IGameTestCheck
	{
	public:
		virtual FName GetKey() const override { return TEXT("actorNear"); }
		virtual FString GetSummary() const override { return TEXT("Actor 在某个点的半径内"); }
		virtual bool Matches(const FJsonObject& InJson) const override { return InJson.HasField(TEXT("actorNear")); }

		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("actorNear") };
			return Fields;
		}

		virtual void Validate(const FJsonObject& InJson, FGameTestDiagSink& InSink) const override
		{
			const TSharedPtr<FJsonObject>* Spec = nullptr;
			if (!InJson.TryGetObjectField(TEXT("actorNear"), Spec) || !Spec)
			{
				InSink.Error(TEXT("bad_type"), TEXT("actorNear 要写成对象 {actor 或 tag, point, radius}"));
				return;
			}
			if (!(*Spec)->HasField(TEXT("actor")) && !(*Spec)->HasField(TEXT("tag")))
			{
				InSink.Error(TEXT("bad_type"), TEXT("actorNear 要写 actor（类名）或 tag"));
			}
			if (!(*Spec)->HasField(TEXT("point")) || !(*Spec)->HasField(TEXT("radius")))
			{
				InSink.Error(TEXT("bad_type"), TEXT("actorNear 要写 point（[x,y,z]、$var 或 @来源）和 radius"));
			}
		}

		virtual void Evaluate(const FJsonObject& InJson, FGameTestFlowContext& InContext, FGameTestCheckResult& OutResult) const override
		{
			const TSharedPtr<FJsonObject>* Spec = nullptr;
			UWorld* World = GameTestWidgetProbe::WorldOf(InContext);
			if (!InJson.TryGetObjectField(TEXT("actorNear"), Spec) || !Spec || !World)
			{
				OutResult.Unresolved = World ? TEXT("actorNear 写法不对") : TEXT("没有游戏世界");
				return;
			}
			FString ClassName;
			FString Tag;
			double Radius = 0.0;
			(*Spec)->TryGetStringField(TEXT("actor"), ClassName);
			(*Spec)->TryGetStringField(TEXT("tag"), Tag);
			const FGameTestValue RadiusValue = GameTestRefs::EvalJson((*Spec)->TryGetField(TEXT("radius")), InContext);
			FVector Point;
			if (!ToPoint(ReadPoint((*Spec)->TryGetField(TEXT("point")), InContext), Point) || !RadiusValue.TryNumber(Radius))
			{
				OutResult.Unresolved = TEXT("point 或 radius 读不到");
				return;
			}
			FString Error;
			AActor* Actor = GameTestActor::ResolveOne(*World, ClassName, Tag, TOptional<bool>(), Error);
			if (!Actor)
			{
				OutResult.Unresolved = Error;
				return;
			}
			const double Distance = FVector::Dist(Actor->GetActorLocation(), Point);
			OutResult.Actual = FGameTestValue::FromNumber(Distance);
			OutResult.bHolds = Distance <= Radius;
			OutResult.Say = FString::Printf(TEXT("%s 距目标点 %.1f（半径 %.1f）"), *Actor->GetName(), Distance, Radius);
		}

		virtual FString Describe(const FJsonObject& InJson) const override
		{
			const TSharedPtr<FJsonObject>* Spec = nullptr;
			FString Who;
			if (InJson.TryGetObjectField(TEXT("actorNear"), Spec) && Spec && !(*Spec)->TryGetStringField(TEXT("actor"), Who))
			{
				(*Spec)->TryGetStringField(TEXT("tag"), Who);
			}
			return FString::Printf(TEXT("%s 到达目标点附近"), *Who);
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCheckActorNear()
	{
		GameTestChecks::Get().Register(MakeShared<FCheckActorNear>());
	}
}
