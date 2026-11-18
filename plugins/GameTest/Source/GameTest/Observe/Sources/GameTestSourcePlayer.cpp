// Copyright Epic Games, Inc. All Rights Reserved.

// 取值来源：本地玩家相关的单例 @gi @gs @gm @ps @pc @pawn @hud（协议第 4.3 节）。一个类按名字登记七次，
// 怎么拿到对象收在 GameTestObjectRoots::Player，这里只是把它接进来源注册表。

#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Sources/GameTestObjectRoots.h"
#include "Observe/Sources/GameTestValueSource.h"

namespace
{
	class FPlayerSource final : public IGameTestValueSource
	{
	public:
		FPlayerSource(const TCHAR* InKey, const TCHAR* InSummary)
			: Key(InKey)
			, Summary(InSummary)
		{
		}

		virtual FName GetKey() const override { return Key; }
		virtual FString GetSummary() const override { return Summary; }
		virtual FString GetSyntax() const override { return FString::Printf(TEXT("@%s.<属性>"), *Key.ToString()); }

		virtual FGameTestValue Resolve(const FString& InSelector, TArrayView<const FGameTestPathSegment> InRest, int32& OutConsumed,
			FGameTestFlowContext& InContext) const override
		{
			OutConsumed = 0;
			if (InContext.World == nullptr)
			{
				return FGameTestValue::Unresolved(TEXT("没有游戏世界（游戏没开？）"));
			}
			FString Error;
			UObject* Object = GameTestObjectRoots::Player(*InContext.World, Key.ToString(), Error);
			return Object != nullptr ? FGameTestValue::FromObject(Object) : FGameTestValue::Unresolved(Error);
		}

	private:
		FName Key;
		FString Summary;
	};
}

namespace GameTestBuiltin
{
	void RegisterSourcePlayer()
	{
		TGameTestRegistry<IGameTestValueSource>& Sources = GameTestValueSources::Get();
		Sources.Register(MakeShared<FPlayerSource>(TEXT("gi"), TEXT("GameInstance")));
		Sources.Register(MakeShared<FPlayerSource>(TEXT("gs"), TEXT("GameState")));
		Sources.Register(MakeShared<FPlayerSource>(TEXT("gm"), TEXT("GameMode（只有服务器 / 单机）")));
		Sources.Register(MakeShared<FPlayerSource>(TEXT("ps"), TEXT("本地玩家的 PlayerState")));
		Sources.Register(MakeShared<FPlayerSource>(TEXT("pc"), TEXT("本地玩家的 PlayerController")));
		Sources.Register(MakeShared<FPlayerSource>(TEXT("pawn"), TEXT("本地玩家控制的 Pawn")));
		Sources.Register(MakeShared<FPlayerSource>(TEXT("hud"), TEXT("本地玩家的 HUD")));
	}
}
