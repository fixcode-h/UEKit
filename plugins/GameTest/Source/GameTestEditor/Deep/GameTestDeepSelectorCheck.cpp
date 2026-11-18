// Copyright Epic Games, Inc. All Rights Reserved.

// 选择器的深度检查：`target` 写的界面（WBP）在不在、路径最后一段的控件在不在那棵树里。
// 读资产的 WidgetTree，不开游戏也能查；运行时才生成的条目（列表里的每一行）只查到容器。

#include "Deep/GameTestDeepSelectorCheck.h"

#include "Core/Flow/Deep/GameTestDeepCheck.h"
#include "Core/Script/GameTestDiag.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Widgets/GameTestWidgetTreeReader.h"

namespace GameTestDeepSelector
{
	const FName Key(TEXT("selectors"));

	/** `WBP_Shop:Root/List[2]>BtnBuy` 里第一层（`>` 之前）路径的最后一段控件名；屏本身（`.`）或写不清时为空。 */
	FString LastWidgetName(const FString& InTarget)
	{
		FString Path;
		if (!InTarget.Split(TEXT(":"), nullptr, &Path))
		{
			return FString();
		}
		Path.Split(TEXT(">"), &Path, nullptr);
		FString Last = Path;
		Path.Split(TEXT("/"), nullptr, &Last, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		Last.Split(TEXT("["), &Last, nullptr);
		Last.Split(TEXT("#"), &Last, nullptr);
		Last.TrimStartAndEndInline();
		return Last == TEXT(".") ? FString() : Last;
	}

	class FSelectorCheck final : public IGameTestDeepCheck
	{
	public:
		virtual FName GetKey() const override { return Key; }

		virtual void CheckScript(const FGameTestScript&, FGameTestDeepEnv&) const override
		{
			Names.Reset();
		}

		virtual void CheckObject(const FJsonObject& InJson, FGameTestDeepEnv& InEnv) const override
		{
			FString Target;
			// 句柄（@x）、变量（$item>Btn）、表达式要运行时才知道指哪
			if (!InJson.TryGetStringField(TEXT("target"), Target) || Target.IsEmpty() || Target.Contains(TEXT("$"))
				|| Target.StartsWith(TEXT("@")) || Target.StartsWith(TEXT("=")) || Target.StartsWith(TEXT(">")))
			{
				return;
			}
			const FString Screen = GameTestWidgetProbe::ScreenOf(Target);
			if (Screen.IsEmpty())
			{
				return;
			}
			++InEnv.Stats.Selectors;
			const TSet<FString>* Tree = TreeOf(Screen);
			FGameTestDiagSink::FScope Scope(InEnv.Sink, TEXT("target"));
			if (Tree == nullptr)
			{
				InEnv.Sink.Error(TEXT("selector_missing"), FString::Printf(TEXT("找不到界面 %s（WBP 名字写错了？gt.widgets 能看界面的控件树）"), *Screen));
				return;
			}
			const FString Widget = LastWidgetName(Target);
			if (!Widget.IsEmpty() && !Tree->Contains(Widget))
			{
				InEnv.Sink.Error(TEXT("selector_missing"), FString::Printf(TEXT("%s 的控件树里没有 %s（改名了？用 gt.widgets 看现在的名字）"), *Screen, *Widget));
			}
		}

	private:
		/** 一个界面的全部控件名（含父 WBP 继承来的）；同一次检查里缓存，一个场景几十个步骤点同一个界面只读一次资产。 */
		const TSet<FString>* TreeOf(const FString& InScreen) const
		{
			if (const TOptional<TSet<FString>>* Cached = Names.Find(InScreen))
			{
				return Cached->IsSet() ? &Cached->GetValue() : nullptr;
			}
			TOptional<TSet<FString>>& Slot = Names.Add(InScreen);
			FGameTestWidgetTreeQuery Query;
			(InScreen.StartsWith(TEXT("/")) ? Query.Asset : Query.Class) = InScreen;
			Query.Limit = 1000;
			TSet<FString> Set;
			// 读控件树一页有字数上限：按 nextOffset 翻完，免得大界面后半截的控件被当成不存在
			for (int32 Page = 0; Page < 50; ++Page)
			{
				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				FString Error;
				if (!GameTestWidgetTreeReader::Read(Query, Out, Error) || !Out.IsValid())
				{
					return Page == 0 ? nullptr : &(Slot = MoveTemp(Set)).GetValue();
				}
				const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
				if (Out->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& Node : *Nodes)
					{
						const TSharedPtr<FJsonObject> Object = Node.IsValid() ? Node->AsObject() : nullptr;
						FString Name;
						if (Object.IsValid() && Object->TryGetStringField(TEXT("name"), Name))
						{
							Set.Add(Name);
						}
					}
				}
				int32 Next = 0;
				if (!Out->TryGetNumberField(TEXT("nextOffset"), Next) || Next <= Query.Offset)
				{
					break;
				}
				Query.Offset = Next;
			}
			Slot = MoveTemp(Set);
			return &Slot.GetValue();
		}

		mutable TMap<FString, TOptional<TSet<FString>>> Names;
	};
}

namespace GameTestDeepSelectorCheck
{
	void Register()
	{
		GameTestDeepChecks::Get().Register(MakeShared<GameTestDeepSelector::FSelectorCheck>());
	}

	void Unregister()
	{
		GameTestDeepChecks::Get().Unregister(GameTestDeepSelector::Key);
	}
}
