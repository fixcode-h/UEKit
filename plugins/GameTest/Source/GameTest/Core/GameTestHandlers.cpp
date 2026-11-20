// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestHandlerShared.h"

#include "Core/GameTestJson.h"
#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Ref/GameTestRef.h"
#include "Observe/Ref/GameTestRefAmbient.h"
#include "Observe/Ref/GameTestRefResolve.h"
#include "Core/GameTestWorld.h"
#include "Observe/GameTestHintPick.h"
#include "Observe/GameTestSelector.h"
#include "Observe/GameTestSnapshot.h"
#include "Observe/GameTestWidgetNode.h"

#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Engine/World.h"

namespace GameTestHandlersRefImpl
{
	/** 一次解析借用的句柄名：解析完马上递归用掉，不需要每次不同。 */
	const TCHAR* RefHandle = TEXT("__ref");

	FString CompactJson(const TSharedPtr<FJsonValue>& InValue)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(InValue, FString(), Writer);
		return Out;
	}

	TSharedPtr<FJsonValue> ParseJson(const FString& InText)
	{
		TSharedPtr<FJsonValue> Out;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(InText);
		FJsonSerializer::Deserialize(Reader, Out);
		return Out;
	}
}

namespace GameTestHandlers
{
	bool TargetText(const FJsonObject& InParams, const TCHAR* InField, FString& OutText, FString& OutError)
	{
		OutText.Reset();
		const TSharedPtr<FJsonValue> Value = InParams.TryGetField(InField);
		if (!Value.IsValid() || Value->IsNull())
		{
			return true;
		}
		FString Text;
		if (Value->TryGetString(Text))
		{
			if (Text.StartsWith(TEXT("@")) && !Text.StartsWith(TEXT("@@")))
			{
				// `@句柄`：gt.step 单步、录制时捕获的句柄，句柄表跨调用保留
				OutText = Text;
				return true;
			}
			if (!Text.StartsWith(TEXT("$")) && !GameTestRefForm::IsName(Text))
			{
				OutError = FString::Printf(TEXT("selector_removed：%s 是旧的选择器写法「%s」，v4 不认：写 {\"obj\": 引用, \"part\": \"部件名\"}（面板「升级脚本」会转）"), InField, *Text.Left(80));
				return false;
			}
		}
		else if (Value->Type != EJson::Object)
		{
			OutError = FString::Printf(TEXT("%s 要写 {\"obj\": 引用, \"part\": \"部件名\"} 或一个引用"), InField);
			return false;
		}
		OutText = FString(RefTargetPrefix) + GameTestHandlersRefImpl::CompactJson(Value);
		return true;
	}

	FString DescribeTarget(const FString& InTarget)
	{
		if (!InTarget.StartsWith(RefTargetPrefix))
		{
			return InTarget;
		}
		FGameTestTarget Target;
		return FGameTestTarget::FromJson(GameTestHandlersRefImpl::ParseJson(InTarget.Mid(FCString::Strlen(RefTargetPrefix))), Target) ? Target.Describe() : InTarget;
	}

	/** `@@ref:` → 按对象引用解析出控件，借一个句柄交回老的句柄路径（Slate 段、子树采样都在那边）。 */
	bool TryResolveRefTarget(const FString& InTarget, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles, TSharedPtr<FGameTestWidgetNode>& OutNode, FString& OutMessage)
	{
		const TSharedPtr<FJsonValue> Json = GameTestHandlersRefImpl::ParseJson(InTarget.Mid(FCString::Strlen(RefTargetPrefix)));
		FGameTestFlowContext Temp;
		FGameTestFlowContext* Flow = GameTestRefAmbient::Get();
		if (Flow == nullptr)
		{
			// 没在跑场景（gt.resolve、证据采集）：只有内联引用能解析，名字和 $var 报找不到
			Temp.World = GameTestWorld::FindGameWorld();
			Temp.Handles = InHandles;
			Flow = &Temp;
		}
		else if (Flow->World == nullptr)
		{
			Flow->World = GameTestWorld::FindGameWorld();
		}
		const FGameTestTargetResult Result = GameTestRefResolve::ResolveTarget(Json, *Flow);
		GameTestRefResolve::WritePayload(Result.Ref, *Flow);
		if (!Result.IsOk())
		{
			OutMessage = FString::Printf(TEXT("%s：%s"), *Result.Code.ToString(), *Result.Error);
			return false;
		}
		UWidget* Widget = Result.GetWidget();
		if (Widget == nullptr)
		{
			OutMessage = FString::Printf(TEXT("%s 解析到的是 %s，不是界面控件：这个动作只能落在控件上"), *Result.Target.Describe(), *GameTestRefResolve::DisplayName(Result.Get()));
			return false;
		}
		if (InHandles == nullptr)
		{
			OutMessage = TEXT("句柄表不可用");
			return false;
		}
		InHandles->Add(GameTestHandlersRefImpl::RefHandle, Widget);
		const FString Inner = FString(TEXT("@")) + GameTestHandlersRefImpl::RefHandle + (Result.Target.Slate.IsEmpty() ? FString() : TEXT(">>") + Result.Target.Slate);
		return TryResolveTarget(Inner, InHandles, OutNode, OutMessage);
	}

	/**
	 * 选择器 / `@句柄` → 控件节点。
	 *
	 * 放在这里（而不是某个子域的匿名 namespace）是因为观察与交互两个子域都要用：
	 * 各写一份必然漂移。
	 */
	bool TryResolveTarget(const FString& InTarget, TMap<FString, TWeakObjectPtr<UWidget>>* InHandles, TSharedPtr<FGameTestWidgetNode>& OutNode, FString& OutMessage, const FJsonObject* InHint, FString* OutPickNote)
	{
		if (InTarget.IsEmpty())
		{
			OutMessage = TEXT("没有给 target");
			return false;
		}
		if (InTarget.StartsWith(RefTargetPrefix))
		{
			return TryResolveRefTarget(InTarget, InHandles, OutNode, OutMessage);
		}

		if (InTarget.StartsWith(TEXT("@")))
		{
			// `@句柄>子路径`（1.9.0）：循环 each 的 `$item>BtnClaim` 会被换成 `@__v_…>BtnClaim`，从这个控件自己的树往下走。
			FString Handle = InTarget.RightChop(1);
			FString Tail;
			int32 Hop = INDEX_NONE;
			if (Handle.FindChar(TEXT('>'), Hop))
			{
				Tail = Handle.Mid(Hop);
				Handle.LeftInline(Hop);
			}
			if (InHandles == nullptr)
			{
				OutMessage = TEXT("句柄表不可用");
				return false;
			}

			const TWeakObjectPtr<UWidget>* Found = InHandles->Find(Handle);
			if (Found == nullptr)
			{
				OutMessage = FString::Printf(TEXT("没有名为 '%s' 的句柄（先用 find 捕获它）"), *Handle);
				return false;
			}

			UWidget* Widget = Found->Get();
			if (Widget == nullptr)
			{
				OutMessage = FString::Printf(TEXT("句柄 '%s' 已失效（控件被销毁或世界重载过），重新 find 一次"), *Handle);
				return false;
			}

			// 句柄每次重新采一遍子树：拿到的永远是此刻的状态与条目，而不是捕获那一刻的快照。
			if (Tail.IsEmpty())
			{
				OutNode = GameTestSnapshot::CaptureSubtree(Widget, FString(), /*InIsScreenRoot=*/false);
				return OutNode.IsValid();
			}
			// 带子路径：把句柄控件当成一个屏，子路径照「屏名:路径」的规则走（`>>` 进 Slate 时从它自己开始）。
			const TSharedPtr<FGameTestWidgetNode> Root = GameTestSnapshot::CaptureSubtree(Widget, FString(), /*InIsScreenRoot=*/true, /*bInLight=*/true);
			if (!Root.IsValid())
			{
				OutMessage = FString::Printf(TEXT("句柄 '%s' 的控件树采不到"), *Handle);
				return false;
			}
			const FString Rest = Tail.StartsWith(TEXT(">>")) ? TEXT("*:.") + Tail : TEXT("*:") + Tail.RightChop(1);
			TArray<TSharedPtr<FGameTestWidgetNode>> Scope;
			Scope.Add(Root);
			if (!GameTestSelector::ResolveText(Scope, Rest, OutNode, OutMessage))
			{
				OutMessage = FString::Printf(TEXT("在 %s 里找 %s：%s"), *Widget->GetName(), *Tail, *OutMessage);
				return false;
			}
			return true;
		}

		UWorld* World = GameTestWorld::FindGameWorld();
		if (World == nullptr)
		{
			OutMessage = TEXT("当前没有正在跑的游戏世界（先 pie.start，或 wait until=pieReady）");
			return false;
		}

		// 只要结构不要属性：回放的自动等待每帧都会走到这里，属性导出是最贵的一块。
		TArray<TSharedPtr<FGameTestWidgetNode>> Screens;
		GameTestSnapshot::CaptureScreens(World, Screens, /*bInLight=*/true);
		if (Screens.Num() == 0 && !InTarget.StartsWith(TEXT(">>")))
		{
			OutMessage = TEXT("世界里没有挂在视口上的 UMG（用 widget.add 造一个靶子，或确认界面真的被打开了）");
			return false;
		}

		TArray<TSharedPtr<FGameTestWidgetNode>> Ambiguous;
		if (GameTestSelector::ResolveText(Screens, InTarget, OutNode, OutMessage, InHint != nullptr ? &Ambiguous : nullptr))
		{
			// 唯一命中但文字和录制时对不上：按位置写的选择器在容器顺序变了时会指到别的按钮，在同容器里找文字对得上的。
			FString RematchNote;
			if (InHint != nullptr && GameTestHintPick::Rematch(Screens, InTarget, *InHint, OutNode, RematchNote) && OutPickNote != nullptr)
			{
				*OutPickNote = RematchNote;
			}
			return true;
		}
		if (InHint == nullptr || Ambiguous.Num() < 2)
		{
			return false;
		}

		// 同一个 WBP 的几个实例都命中：按录制线索挑回录的那一个；挑不出就照旧算没找到（回放接着等，超时报原因）。
		FString Note;
		FString Why;
		if (GameTestHintPick::Pick(Ambiguous, *InHint, OutNode, Note, Why))
		{
			if (OutPickNote != nullptr)
			{
				*OutPickNote = Note;
			}
			return true;
		}
		OutMessage = FString::Printf(TEXT("%s；按录制线索也挑不出：%s"), *OutMessage, *Why);
		return false;
	}

	bool TryResolveStepTarget(FGameTestStepContext& InContext, TSharedPtr<FGameTestWidgetNode>& OutNode, FString& OutError)
	{
		FString PickNote;
		if (!TryResolveTarget(InContext.Step->Target, InContext.Handles, OutNode, OutError,
				GameTestHintPick::GetHint(InContext.Step->Params), &PickNote))
		{
			return false;
		}
		if (!PickNote.IsEmpty() && InContext.Payload.IsValid())
		{
			InContext.Payload->SetStringField(TEXT("picked"), PickNote);
		}

		InContext.TouchedWidget = OutNode->Widget;

		// 矩形照抄快照的坐标（截图标注用的同一份数）：换算留给读的人，写入的人不做第二套。
		InContext.bTouchedHasRect = OutNode->bHasGeometry && OutNode->Size.X >= 1.0 && OutNode->Size.Y >= 1.0;
		InContext.TouchedMin = OutNode->Position;
		InContext.TouchedMax = OutNode->Position + OutNode->Size;
		return true;
	}

	bool IsActuallyVisible(const FGameTestWidgetNode& InNode)
	{
		return InNode.Visibility == TEXT("Visible")
			|| InNode.Visibility == TEXT("HitTestInvisible")
			|| InNode.Visibility == TEXT("SelfHitTestInvisible");
	}

	bool TryReadClientField(const FJsonObject& InParams, FString& OutClientText, bool& bOutSpecified, FString& OutError)
	{
		bOutSpecified = false;
		OutClientText.Reset();

		if (!InParams.Values.Contains(TEXT("client")))
		{
			return true;
		}

		bOutSpecified = true;

		if (GameTestJson::TryGetString(InParams, TEXT("client"), OutClientText))
		{
			return true;
		}

		double Number = 0.0;
		if (GameTestJson::TryGetNumber(InParams, TEXT("client"), Number))
		{
			OutClientText = FString::FromInt(FMath::RoundToInt(Number));
			return true;
		}

		OutError = TEXT("client 字段只能是 \"server\" 或从 0 起的序号");
		return false;
	}

	UWorld* ResolveStepWorld(const FGameTestStepContext& InContext, FString& OutError)
	{
		FString ClientText;
		bool bSpecified = false;
		if (!TryReadClientField(*InContext.Step->Params, ClientText, bSpecified, OutError))
		{
			return nullptr;
		}

		UWorld* World = GameTestWorld::ResolveWorld(ClientText, bSpecified, OutError);
		if (World == nullptr && OutError.IsEmpty())
		{
			OutError = TEXT("当前没有正在跑的游戏世界（先 pie.start，或 wait until=pieReady）");
		}
		return World;
	}
}
