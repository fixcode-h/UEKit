// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Sources/GameTestValueSource.h"

#include "Core/Script/GameTestBuiltins.h"
#include "Core/Script/GameTestFlowContext.h"

FString FGameTestPathSegment::ToDisplay() const
{
	switch (Kind)
	{
	case EKind::Index:
		return Index.IsString() ? FString::Printf(TEXT("[\"%s\"]"), *Index.GetString()) : FString::Printf(TEXT("[%s]"), *Index.ToDisplay());
	case EKind::Call:
	{
		TArray<FString> Shown;
		for (const FGameTestValue& Arg : Args)
		{
			Shown.Add(Arg.ToDisplay(40));
		}
		return FString::Printf(TEXT(".%s(%s)"), *Name, *FString::Join(Shown, TEXT(", ")));
	}
	default:
		return TEXT(".") + Name;
	}
}

FGameTestPathSegment FGameTestPathSegment::Member(const FString& InName)
{
	FGameTestPathSegment Segment;
	Segment.Kind = EKind::Member;
	Segment.Name = InName;
	return Segment;
}

FGameTestPathSegment FGameTestPathSegment::At(const FGameTestValue& InIndex)
{
	FGameTestPathSegment Segment;
	Segment.Kind = EKind::Index;
	Segment.Index = InIndex;
	return Segment;
}

FGameTestPathSegment FGameTestPathSegment::Call(const FString& InName, TArray<FGameTestValue>&& InArgs)
{
	FGameTestPathSegment Segment;
	Segment.Kind = EKind::Call;
	Segment.Name = InName;
	Segment.Args = MoveTemp(InArgs);
	return Segment;
}

TGameTestRegistry<IGameTestValueSource>& GameTestValueSources::Get()
{
	static TGameTestRegistry<IGameTestValueSource> Registry;
	GameTestBuiltins::EnsureRegistered();
	return Registry;
}

namespace
{
	TArray<TSharedRef<IGameTestMemberAccess>>& MemberAccessList()
	{
		static TArray<TSharedRef<IGameTestMemberAccess>> List;
		return List;
	}
}

void GameTestMemberAccess::Register(const TSharedRef<IGameTestMemberAccess>& InAccess)
{
	TArray<TSharedRef<IGameTestMemberAccess>>& List = MemberAccessList();
	List.RemoveAll([&InAccess](const TSharedRef<IGameTestMemberAccess>& Item) { return Item->GetKey() == InAccess->GetKey(); });
	List.Add(InAccess);
	List.StableSort([](const TSharedRef<IGameTestMemberAccess>& A, const TSharedRef<IGameTestMemberAccess>& B)
	{
		return A->GetPriority() > B->GetPriority();
	});
}

void GameTestMemberAccess::Unregister(FName InKey)
{
	MemberAccessList().RemoveAll([InKey](const TSharedRef<IGameTestMemberAccess>& Item) { return Item->GetKey() == InKey; });
}

const TArray<TSharedRef<IGameTestMemberAccess>>& GameTestMemberAccess::GetAll()
{
	GameTestBuiltins::EnsureRegistered();
	return MemberAccessList();
}

namespace GameTestPath
{
	FGameTestValue Index(const FGameTestValue& InBase, const FGameTestValue& InIndex)
	{
		if (InIndex.IsUnresolved())
		{
			return InIndex;
		}
		if (InBase.Type == EGameTestValueType::Array)
		{
			double Number = 0.0;
			if (!InIndex.TryNumber(Number))
			{
				return FGameTestValue::Unresolved(FString::Printf(TEXT("数组下标要是数字，写的是 %s"), *InIndex.ToDisplay()), TEXT("expr_type"));
			}
			const TArray<FGameTestValue>& Items = InBase.GetArray();
			int32 At = FMath::FloorToInt32(Number);
			// 负数从尾数（$list[-1] 是最后一个），作者常要「最新的那条」。
			if (At < 0)
			{
				At += Items.Num();
			}
			if (!Items.IsValidIndex(At))
			{
				return FGameTestValue::Unresolved(FString::Printf(TEXT("下标 %d 越界（一共 %d 个）"), FMath::FloorToInt32(Number), Items.Num()));
			}
			return Items[At];
		}
		if (InBase.Type == EGameTestValueType::Map || InBase.Type == EGameTestValueType::Vector)
		{
			FGameTestValue Out;
			const FString Key = InIndex.IsString() ? InIndex.GetString() : InIndex.ToDisplay();
			if (InBase.TryGetField(Key, Out))
			{
				return Out;
			}
			return FGameTestValue::Unresolved(FString::Printf(TEXT("没有键 \"%s\""), *Key));
		}
		if (InBase.IsString())
		{
			double Number = 0.0;
			if (InIndex.TryNumber(Number))
			{
				int32 At = FMath::FloorToInt32(Number);
				const FString& Text = InBase.GetString();
				At = At < 0 ? At + Text.Len() : At;
				if (At >= 0 && At < Text.Len())
				{
					return FGameTestValue::FromString(Text.Mid(At, 1));
				}
			}
		}
		return FGameTestValue::Unresolved(FString::Printf(TEXT("%s 不能按下标取"), *InBase.TypeName()), TEXT("expr_type"));
	}

	FGameTestValue Member(const FGameTestValue& InBase, const FString& InName, FGameTestFlowContext& InContext)
	{
		if (InBase.IsUnresolved())
		{
			return InBase;
		}
		FGameTestValue Out;
		if (InBase.TryGetField(InName, Out))
		{
			return Out;
		}
		// 数组、字符串的长度：脚本里常写 $list.Num / $text.Length，和蓝图的叫法一致。
		if ((InBase.Type == EGameTestValueType::Array || InBase.IsString())
			&& (InName.Equals(TEXT("Num"), ESearchCase::IgnoreCase) || InName.Equals(TEXT("Length"), ESearchCase::IgnoreCase)))
		{
			return FGameTestValue::FromNumber(InBase.Len());
		}
		for (const TSharedRef<IGameTestMemberAccess>& Access : GameTestMemberAccess::GetAll())
		{
			if (Access->Handles(InBase) && Access->ReadMember(InBase, InName, InContext, Out))
			{
				return Out;
			}
		}
		if (InBase.IsNull())
		{
			return FGameTestValue::Unresolved(FString::Printf(TEXT("前面一段是空的（null），取不到 %s"), *InName));
		}
		return FGameTestValue::Unresolved(FString::Printf(TEXT("%s 上没有 %s"), *InBase.ToDisplay(60), *InName), TEXT("path_missing"));
	}

	FGameTestValue CallMethod(const FGameTestValue& InBase, const FString& InName, const TArray<FGameTestValue>& InArgs, FGameTestFlowContext& InContext)
	{
		if (InBase.IsUnresolved())
		{
			return InBase;
		}
		for (const FGameTestValue& Arg : InArgs)
		{
			if (Arg.IsUnresolved())
			{
				return Arg;
			}
		}
		FGameTestValue Out;
		for (const TSharedRef<IGameTestMemberAccess>& Access : GameTestMemberAccess::GetAll())
		{
			if (Access->Handles(InBase) && Access->CallMethod(InBase, InName, InArgs, InContext, Out))
			{
				return Out;
			}
		}
		return FGameTestValue::Unresolved(FString::Printf(TEXT("%s 上没有函数 %s"), *InBase.ToDisplay(60), *InName), TEXT("fn_missing"));
	}

	FGameTestValue Walk(const FGameTestValue& InBase, TArrayView<const FGameTestPathSegment> InSegments, FGameTestFlowContext& InContext, const FString& InWhere)
	{
		FGameTestValue Current = InBase;
		FString Where = InWhere;
		for (const FGameTestPathSegment& Segment : InSegments)
		{
			if (Current.IsUnresolved())
			{
				break;
			}
			switch (Segment.Kind)
			{
			case FGameTestPathSegment::EKind::Index:
				Current = Index(Current, Segment.Index);
				break;
			case FGameTestPathSegment::EKind::Call:
				Current = CallMethod(Current, Segment.Name, Segment.Args, InContext);
				break;
			default:
				Current = Member(Current, Segment.Name, InContext);
				break;
			}
			if (Current.IsUnresolved())
			{
				// 原因前面带上走到哪了，作者一眼看出是哪一段写错。
				return FGameTestValue::Unresolved(FString::Printf(TEXT("%s%s：%s"), *Where, *Segment.ToDisplay(), *Current.GetString()),
					Current.GetCode());
			}
			Where += Segment.ToDisplay();
		}
		return Current;
	}

	FGameTestValue ResolveSource(FName InSource, const FString& InSelector, TArrayView<const FGameTestPathSegment> InSegments, FGameTestFlowContext& InContext)
	{
		const IGameTestValueSource* Source = GameTestValueSources::Get().Find(InSource);
		const FString Root = FString::Printf(TEXT("@%s%s"), *InSource.ToString(), InSelector.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("[%s]"), *InSelector));
		if (!Source)
		{
			return FGameTestValue::Unresolved(FString::Printf(TEXT("不认识的来源 @%s"), *InSource.ToString()), TEXT("path_missing"));
		}
		int32 Consumed = 0;
		const FGameTestValue Base = Source->Resolve(InSelector, InSegments, Consumed, InContext);
		Consumed = FMath::Clamp(Consumed, 0, InSegments.Num());
		FString Where = Root;
		for (int32 Index = 0; Index < Consumed; ++Index)
		{
			Where += InSegments[Index].ToDisplay();
		}
		if (Base.IsUnresolved())
		{
			return FGameTestValue::Unresolved(FString::Printf(TEXT("%s：%s"), *Where, *Base.GetString()), Base.GetCode());
		}
		return Walk(Base, InSegments.RightChop(Consumed), InContext, Where);
	}
}
