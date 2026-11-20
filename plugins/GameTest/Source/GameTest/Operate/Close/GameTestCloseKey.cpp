// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/Close/GameTestCloser.h"

#include "Core/Script/GameTestDiag.h"
#include "Dom/JsonObject.h"
#include "Input/GameTestInput.h"

#include "InputCoreTypes.h"

namespace GameTestCloseKeyImpl
{
	class FCloser final : public IGameTestCloser
	{
	public:
		virtual FName GetKey() const override { return TEXT("key"); }
		virtual FString GetSummary() const override { return TEXT("按键发给游戏视口（key 默认 Escape）"); }
		virtual const TArray<FString>& GetFields() const override
		{
			static const TArray<FString> Fields = { TEXT("key") };
			return Fields;
		}

		virtual void Validate(const FJsonObject& InHow, FGameTestDiagSink& InSink) const override
		{
			FString Key;
			if (InHow.TryGetStringField(TEXT("key"), Key) && !FKey(FName(*Key)).IsValid())
			{
				InSink.Error(TEXT("bad_type"), FString::Printf(TEXT("by=key 的 key 不是 UE 的键名：%s（Escape、BackSpace、Q……）"), *Key));
			}
		}

		virtual bool Close(FGameTestCloseRun& InRun) const override
		{
			FString KeyName = TEXT("Escape");
			InRun.How.TryGetStringField(TEXT("key"), KeyName);
			const FKey Key(FName(*KeyName));
			if (!Key.IsValid())
			{
				InRun.Message = FString::Printf(TEXT("不认识键名 %s"), *KeyName);
				return false;
			}
			// 按下、松开在同一帧发：Slate 同步处理按键事件，和 key 动作的 tap 收到的是同一对事件
			FString Message;
			if (!GameTestInput::SendKey(Key, true, Message) || !GameTestInput::SendKey(Key, false, Message))
			{
				InRun.Message = Message;
				return false;
			}
			InRun.Message = FString::Printf(TEXT("按了 %s"), *KeyName);
			return true;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterCloseKey()
	{
		GameTestClosers::Get().Register(MakeShared<GameTestCloseKeyImpl::FCloser>());
	}
}
