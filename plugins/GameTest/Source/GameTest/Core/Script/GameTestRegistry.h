// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 扩展点注册表（协议第 14 节）：块、判断、取值来源、表达式函数、接口都是「一种一个实现 + 按名字登记」。
 *
 * 为什么统一成一个模板：每张表要的东西一样——按名字找、按登记顺序列出（`gt.actions`、`gt.api`、
 * 面板的节点面板都从这里生成，顺序就是文档顺序）、同名替换（可选模块热重载时重新登记）、模块关闭时摘掉。
 * 各写一份迟早有一张表忘了「同名替换」。
 *
 * `T` 要提供 `FName GetKey() const`。FName 比较忽略大小写，`widget.IsShown` 和 `Widget.IsShown` 是同一个。
 * 只在游戏线程上用（登记在模块启动、查找在执行时，都在游戏线程）。
 */
template <typename T>
class TGameTestRegistry
{
public:
	/** 登记；同名的替换掉原来那个，位置不变。 */
	void Register(const TSharedRef<T>& InItem)
	{
		const FName Key = InItem->GetKey();
		if (const int32* Existing = IndexByKey.Find(Key))
		{
			Items[*Existing] = InItem;
			return;
		}
		IndexByKey.Add(Key, Items.Num());
		Items.Add(InItem);
	}

	void Unregister(FName InKey)
	{
		if (!IndexByKey.Contains(InKey))
		{
			return;
		}
		Items.RemoveAll([InKey](const TSharedRef<T>& Item) { return Item->GetKey() == InKey; });
		Reindex();
	}

	/** 按名字找；没有返回 nullptr。 */
	const T* Find(FName InKey) const
	{
		const int32* Index = IndexByKey.Find(InKey);
		return Index ? &Items[*Index].Get() : nullptr;
	}

	TSharedPtr<T> FindShared(FName InKey) const
	{
		const int32* Index = IndexByKey.Find(InKey);
		return Index ? TSharedPtr<T>(Items[*Index]) : TSharedPtr<T>();
	}

	/** 按登记顺序的全部实现。 */
	const TArray<TSharedRef<T>>& GetAll() const
	{
		return Items;
	}

	int32 Num() const
	{
		return Items.Num();
	}

	/** 写错名字时给候选（「你是想写 Widget.IsShown 吗」）：编辑距离最近的几个。 */
	void Suggest(const FString& InWritten, TArray<FString>& OutSuggestions, int32 InMax = 3) const
	{
		TArray<TPair<int32, FString>> Scored;
		const FString Lower = InWritten.ToLower();
		for (const TSharedRef<T>& Item : Items)
		{
			const FString Name = Item->GetKey().ToString();
			const FString NameLower = Name.ToLower();
			const int32 Distance = NameLower.Contains(Lower) || Lower.Contains(NameLower) ? 0 : LevenshteinCapped(Lower, NameLower, 4);
			if (Distance <= 3)
			{
				Scored.Add(TPair<int32, FString>(Distance, Name));
			}
		}
		Scored.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B) { return A.Key < B.Key; });
		for (int32 Index = 0; Index < Scored.Num() && OutSuggestions.Num() < InMax; ++Index)
		{
			OutSuggestions.AddUnique(Scored[Index].Value);
		}
	}

private:
	void Reindex()
	{
		IndexByKey.Reset();
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			IndexByKey.Add(Items[Index]->GetKey(), Index);
		}
	}

	/** 编辑距离（超过 InCap 就提前返回 InCap），名字都很短，不用更快的算法。 */
	static int32 LevenshteinCapped(const FString& A, const FString& B, int32 InCap)
	{
		if (FMath::Abs(A.Len() - B.Len()) >= InCap)
		{
			return InCap;
		}
		TArray<int32> Prev;
		TArray<int32> Cur;
		Prev.SetNumUninitialized(B.Len() + 1);
		Cur.SetNumUninitialized(B.Len() + 1);
		for (int32 J = 0; J <= B.Len(); ++J)
		{
			Prev[J] = J;
		}
		for (int32 I = 1; I <= A.Len(); ++I)
		{
			Cur[0] = I;
			int32 RowMin = Cur[0];
			for (int32 J = 1; J <= B.Len(); ++J)
			{
				const int32 Cost = A[I - 1] == B[J - 1] ? 0 : 1;
				Cur[J] = FMath::Min3(Prev[J] + 1, Cur[J - 1] + 1, Prev[J - 1] + Cost);
				RowMin = FMath::Min(RowMin, Cur[J]);
			}
			if (RowMin >= InCap)
			{
				return InCap;
			}
			Swap(Prev, Cur);
		}
		return FMath::Min(Prev[B.Len()], InCap);
	}

	TArray<TSharedRef<T>> Items;
	TMap<FName, int32> IndexByKey;
};
