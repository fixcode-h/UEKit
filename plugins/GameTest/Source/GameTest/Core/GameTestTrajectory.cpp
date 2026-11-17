// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestTrajectory.h"

#include "Dom/JsonValue.h"

namespace GameTestTrajectoryDetail
{
	/** 点到线段的距离（三维）：RDP 判断一个点偏离前后连线多远。 */
	double DistanceToSegment(const FVector& InPoint, const FVector& InA, const FVector& InB)
	{
		const FVector Segment = InB - InA;
		const double LengthSquared = Segment.SizeSquared();
		if (LengthSquared <= KINDA_SMALL_NUMBER)
		{
			return FVector::Dist(InPoint, InA);
		}
		const double T = FMath::Clamp(FVector::DotProduct(InPoint - InA, Segment) / LengthSquared, 0.0, 1.0);
		return FVector::Dist(InPoint, InA + Segment * T);
	}

	/** 不用递归：几千个采样点时递归深度不可控，用显式栈。 */
	void MarkKept(const TArray<FVector>& InPoints, double InTolerance, const TSet<int32>& InKeep, TArray<bool>& InOutKept)
	{
		TArray<TPair<int32, int32>> Stack;
		Stack.Emplace(0, InPoints.Num() - 1);
		while (Stack.Num() > 0)
		{
			const TPair<int32, int32> Range = Stack.Pop();
			const int32 First = Range.Key;
			const int32 Last = Range.Value;
			if (Last - First < 2)
			{
				continue;
			}

			// 必须保留的点直接当分割点：它两边各自再简化。
			int32 Split = INDEX_NONE;
			for (int32 Index = First + 1; Index < Last; ++Index)
			{
				if (InKeep.Contains(Index))
				{
					Split = Index;
					break;
				}
			}

			if (Split == INDEX_NONE)
			{
				double Farthest = -1.0;
				for (int32 Index = First + 1; Index < Last; ++Index)
				{
					const double Distance = DistanceToSegment(InPoints[Index], InPoints[First], InPoints[Last]);
					if (Distance > Farthest)
					{
						Farthest = Distance;
						Split = Index;
					}
				}
				if (Farthest <= InTolerance)
				{
					continue;
				}
			}

			InOutKept[Split] = true;
			Stack.Emplace(First, Split);
			Stack.Emplace(Split, Last);
		}
	}
}

TArray<int32> GameTestTrajectory::Simplify(const TArray<FVector>& InPoints, double InTolerance, const TSet<int32>& InKeep)
{
	TArray<int32> Out;
	if (InPoints.Num() <= 2)
	{
		for (int32 Index = 0; Index < InPoints.Num(); ++Index)
		{
			Out.Add(Index);
		}
		return Out;
	}

	TArray<bool> Kept;
	Kept.Init(false, InPoints.Num());
	Kept[0] = true;
	Kept.Last() = true;
	GameTestTrajectoryDetail::MarkKept(InPoints, FMath::Max(InTolerance, 0.0), InKeep, Kept);
	for (int32 Index = 0; Index < Kept.Num(); ++Index)
	{
		if (Kept[Index])
		{
			Out.Add(Index);
		}
	}
	return Out;
}

TArray<int32> GameTestTrajectory::SimplifyToLimit(const TArray<FVector>& InPoints, double InTolerance, int32 InMaxPoints, const TSet<int32>& InKeep)
{
	double Tolerance = FMath::Max(InTolerance, 1.0);
	TArray<int32> Kept = Simplify(InPoints, Tolerance, InKeep);
	// 必须保留的点本身就超上限时放大容差也没用：最多放大 20 次，返回最后一次的结果。
	for (int32 Round = 0; Round < 20 && Kept.Num() > FMath::Max(InMaxPoints, 2); ++Round)
	{
		Tolerance *= 1.5;
		Kept = Simplify(InPoints, Tolerance, InKeep);
	}
	return Kept;
}

int32 GameTestTrajectory::FindJoinIndex(const TArray<FVector>& InPath, const FVector& InPosition, double& OutDistance, FVector& OutProjected)
{
	OutDistance = TNumericLimits<double>::Max();
	OutProjected = InPosition;
	if (InPath.Num() < 2)
	{
		return INDEX_NONE;
	}

	int32 Best = INDEX_NONE;
	const FVector2D Position(InPosition);
	for (int32 Index = 0; Index + 1 < InPath.Num(); ++Index)
	{
		const FVector2D A(InPath[Index]);
		const FVector2D B(InPath[Index + 1]);
		const FVector2D Segment = B - A;
		const double LengthSquared = Segment.SizeSquared();
		const double T = LengthSquared <= KINDA_SMALL_NUMBER ? 0.0 : FMath::Clamp(FVector2D::DotProduct(Position - A, Segment) / LengthSquared, 0.0, 1.0);
		const FVector2D Projected = A + Segment * T;
		const double Distance = FVector2D::Distance(Position, Projected);
		// 距离一样时取靠后的一段：角色站在两段交界处，应该接着往前走而不是回到上一段。
		if (Distance <= OutDistance)
		{
			OutDistance = Distance;
			Best = Index + 1;
			OutProjected = FMath::Lerp(InPath[Index], InPath[Index + 1], T);
		}
	}
	return Best;
}

double GameTestTrajectory::Length(const TArray<FVector>& InPath)
{
	double Total = 0.0;
	for (int32 Index = 1; Index < InPath.Num(); ++Index)
	{
		Total += FVector::Dist(InPath[Index - 1], InPath[Index]);
	}
	return Total;
}

TSharedPtr<FJsonValue> GameTestTrajectory::PointToJson(const FVector& InPoint)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(InPoint.X * 10.0) / 10.0));
	Values.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(InPoint.Y * 10.0) / 10.0));
	Values.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(InPoint.Z * 10.0) / 10.0));
	return MakeShared<FJsonValueArray>(Values);
}

bool GameTestTrajectory::PointFromJson(const TSharedPtr<FJsonValue>& InValue, FVector& OutPoint)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!InValue.IsValid() || !InValue->TryGetArray(Values) || Values == nullptr || Values->Num() != 3)
	{
		return false;
	}
	double Components[3] = {0.0, 0.0, 0.0};
	for (int32 Index = 0; Index < 3; ++Index)
	{
		if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetNumber(Components[Index]))
		{
			return false;
		}
	}
	OutPoint = FVector(Components[0], Components[1], Components[2]);
	return true;
}

TArray<TSharedPtr<FJsonValue>> GameTestTrajectory::PathToJson(const TArray<FVector>& InPath)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FVector& Point : InPath)
	{
		Out.Add(PointToJson(Point));
	}
	return Out;
}

bool GameTestTrajectory::PathFromJson(const TArray<TSharedPtr<FJsonValue>>& InValues, TArray<FVector>& OutPath, FString& OutError)
{
	OutPath.Reset();
	for (int32 Index = 0; Index < InValues.Num(); ++Index)
	{
		FVector Point;
		if (!PointFromJson(InValues[Index], Point))
		{
			OutError = FString::Printf(TEXT("path 第 %d 个点不是 [x, y, z]"), Index + 1);
			return false;
		}
		OutPath.Add(Point);
	}
	return true;
}

TArray<TSharedPtr<FJsonValue>> GameTestTrajectory::IndicesToJson(const TArray<int32>& InIndices)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const int32 Index : InIndices)
	{
		Out.Add(MakeShared<FJsonValueNumber>(Index));
	}
	return Out;
}

TSet<int32> GameTestTrajectory::IndicesFromJson(const TArray<TSharedPtr<FJsonValue>>* InValues, int32 InCount)
{
	TSet<int32> Out;
	if (InValues == nullptr)
	{
		return Out;
	}
	for (const TSharedPtr<FJsonValue>& Value : *InValues)
	{
		double Number = 0.0;
		if (Value.IsValid() && Value->TryGetNumber(Number))
		{
			const int32 Index = FMath::RoundToInt(Number);
			if (Index >= 0 && Index < InCount)
			{
				Out.Add(Index);
			}
		}
	}
	return Out;
}
