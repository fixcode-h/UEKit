// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestHandlerShared.h"

#include "Core/GameTestJson.h"
#include "Observe/GameTestItemMatch.h"
#include "Observe/GameTestWidgetNode.h"
#include "Operate/GameTestOps.h"

#include "Blueprint/UserWidget.h"
#include "Components/ListView.h"
#include "Components/TreeView.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	/**
	 * 列表族的条目定位：`index`，或 `property` + `equals`（属性名大小写不敏感、跨类型退化到文本）。
	 * `select` 与 `set expanded`（TreeView）共用这一份——两边的语义必须一致。
	 */
	bool ResolveListIndex(const TSharedPtr<FGameTestWidgetNode>& InNode, const FJsonObject& InParams, int32& OutIndex, FString& OutError)
	{
		if (GameTestJson::TryGetInt(InParams, TEXT("index"), OutIndex))
		{
			return true;
		}

		FString Property;
		if (!GameTestJson::TryGetString(InParams, TEXT("property"), Property))
		{
			OutError = TEXT("需要 index，或者 property + equals");
			return false;
		}

		const TSharedPtr<FJsonValue> ExpectedValue = InParams.TryGetField(TEXT("equals"));
		if (!ExpectedValue.IsValid())
		{
			OutError = TEXT("用 property 时必须同时给 equals（要匹配的值）");
			return false;
		}

		OutIndex = GameTestItemMatch::FindIndexByProperty(InNode->Items, Property, *ExpectedValue);
		if (OutIndex == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("没有 %s=%s 的条目；%s"),
				*Property, *ExpectedValue->AsString(),
				*GameTestItemMatch::DescribeProperty(InNode->Items, Property, /*InLimit=*/10));
			return false;
		}

		return true;
	}

	/** 条目下标 → 数据项（`UObject*`）。越界或条目失效时返回 nullptr。 */
	UObject* ListItemAt(const TSharedPtr<FGameTestWidgetNode>& InNode, int32 InIndex)
	{
		if (!InNode->Items.IsValidIndex(InIndex) || !InNode->Items[InIndex].IsValid())
		{
			return nullptr;
		}

		return InNode->Items[InIndex]->Item.Get();
	}
}

namespace GameTestHandlers
{
	EGameTestStepStatus HandleClick(FGameTestStepContext& InContext)
	{
		// 点击方式：步骤的 via > 场景 / 运行配置的 clickVia（执行器注入到 ClickVia）> 默认 code。
		// code 调对象的委托（按钮的 OnPressed / OnClicked / OnReleased），找不到按钮这类控件时用虚拟鼠标，不动系统光标；
		// pointer 是真实鼠标（移光标、按下松开、核对接住的控件），要验证遮挡、悬停时才用。
		FString Via;
		GameTestJson::TryGetString(*InContext.Step->Params, TEXT("via"), Via);
		if (Via.IsEmpty())
		{
			Via = InContext.ClickVia.IsEmpty() ? TEXT("code") : InContext.ClickVia;
		}
		if (Via.Equals(TEXT("pointer"), ESearchCase::IgnoreCase))
		{
			return HandleClickPointer(InContext);
		}
		if (Via.Equals(TEXT("code"), ESearchCase::IgnoreCase))
		{
			return HandleClickCode(InContext);
		}
		InContext.Message = FString::Printf(TEXT("via 只能是 code（默认，调控件的点击委托，不动鼠标）或 pointer（真实鼠标），收到 '%s'"), *Via);
		return StepFailed;
	}

	EGameTestStepStatus HandleType(FGameTestStepContext& InContext)
	{
		// 默认按真实按键打字（给焦点、逐字发字符）；`via: "code"` 才按控件类直接 SetText。
		FString Via;
		GameTestJson::TryGetString(*InContext.Step->Params, TEXT("via"), Via);
		if (Via.IsEmpty() || Via.Equals(TEXT("keys"), ESearchCase::IgnoreCase))
		{
			return HandleTypeKeys(InContext);
		}
		if (!Via.Equals(TEXT("code"), ESearchCase::IgnoreCase))
		{
			InContext.Message = FString::Printf(TEXT("via 只能是 keys（默认，真实按键）或 code（直接写文本），收到 '%s'"), *Via);
			return StepFailed;
		}

		FString Text;
		if (!GameTestJson::TryGetString(*InContext.Step->Params, TEXT("text"), Text))
		{
			InContext.Message = TEXT("type 需要 text");
			return StepFailed;
		}

		bool bCommit = false;
		GameTestJson::TryGetBool(*InContext.Step->Params, TEXT("commit"), bCommit);

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		if (!TryResolveStepTarget(InContext, Node, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		if (!GameTestOps::SetText(Node->Widget.Get(), Text, bCommit, InContext.Message, InContext.Payload))
		{
			return StepFailed;
		}

		InContext.Payload->SetStringField(TEXT("path"), Node->Path);
		InContext.Payload->SetStringField(TEXT("text"), Text);
		return StepSucceeded;
	}

	EGameTestStepStatus HandleSelect(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		if (!TryResolveStepTarget(InContext, Node, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		// ListView / TileView / TreeView 同源（都派生自 UListView），选中语义一致；
		// TreeView 的匹配范围是根条目（子条目由游戏侧的取子委托提供，插件没有公开读接口）。
		UListView* List = Cast<UListView>(Node->Widget.Get());
		if (List == nullptr)
		{
			InContext.Message = FString::Printf(TEXT("%s（%s）不是列表族（ListView / TileView / TreeView）；选条目只能用在它们上"),
				*Node->Path, *Node->ClassName);
			return StepFailed;
		}

		int32 Index = INDEX_NONE;
		if (!ResolveListIndex(Node, Params, Index, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		bool bAdd = false;
		GameTestJson::TryGetBool(Params, TEXT("add"), bAdd);

		if (!GameTestOps::SelectListItem(List, Index, bAdd, InContext.Message, InContext.Payload))
		{
			return StepFailed;
		}

		InContext.Payload->SetStringField(TEXT("path"), Node->Path);
		return StepSucceeded;
	}

	EGameTestStepStatus HandleScroll(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;

		FString Mode = TEXT("offset");
		GameTestJson::TryGetString(Params, TEXT("mode"), Mode);

		double Value = 0.0;
		GameTestJson::TryGetNumber(Params, TEXT("value"), Value);

		int32 Index = 0;
		GameTestJson::TryGetInt(Params, TEXT("index"), Index);

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		if (!TryResolveStepTarget(InContext, Node, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		UWidget* TargetWidget = nullptr;
		FString WidgetSelector;
		if (GameTestJson::TryGetString(Params, TEXT("widget"), WidgetSelector))
		{
			TSharedPtr<FGameTestWidgetNode> Inner;
			FString InnerError;
			if (!TryResolveTarget(WidgetSelector, InContext.Handles, Inner, InnerError))
			{
				InContext.Message = InnerError;
				return StepFailed;
			}
			TargetWidget = Inner->Widget.Get();
		}

		if (!GameTestOps::Scroll(Node->Widget.Get(), Mode, Value, Index, TargetWidget, InContext.Message, InContext.Payload))
		{
			return StepFailed;
		}

		InContext.Payload->SetStringField(TEXT("path"), Node->Path);
		return StepSucceeded;
	}

	EGameTestStepStatus HandleSet(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;

		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		if (!TryResolveStepTarget(InContext, Node, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		// TreeView 的展开先在这里拦下：它要节点的条目清单来定位条目（`index` 或 `property`+`equals`），
		// 而 `SetValue` 只认控件自身的字段。
		if (UTreeView* Tree = Cast<UTreeView>(Node->Widget.Get()))
		{
			bool bExpanded = false;
			if (GameTestJson::TryGetBool(Params, TEXT("expanded"), bExpanded))
			{
				int32 Index = INDEX_NONE;
				if (!ResolveListIndex(Node, Params, Index, Error))
				{
					InContext.Message = Error;
					return StepFailed;
				}

				UObject* Item = ListItemAt(Node, Index);
				if (Item == nullptr)
				{
					InContext.Message = FString::Printf(TEXT("第 %d 个条目已失效（列表被重建过？先 snapshot 看一眼）"), Index);
					return StepFailed;
				}

				if (!GameTestOps::SetListItemExpansion(Tree, Item, bExpanded, InContext.Message, InContext.Payload))
				{
					return StepFailed;
				}

				InContext.Payload->SetStringField(TEXT("path"), Node->Path);
				return StepSucceeded;
			}
		}

		if (!GameTestOps::SetValue(Node->Widget.Get(), Params, InContext.Message, InContext.Payload))
		{
			return StepFailed;
		}

		InContext.Payload->SetStringField(TEXT("path"), Node->Path);
		return StepSucceeded;
	}

	EGameTestStepStatus HandleWidgetAdd(FGameTestStepContext& InContext)
	{
		const FJsonObject& Params = *InContext.Step->Params;

		FString ClassPath;
		if (!GameTestJson::TryGetString(Params, TEXT("class"), ClassPath) || ClassPath.IsEmpty())
		{
			InContext.Message = TEXT("widget.add 需要 class（控件类路径或资产名）");
			return StepFailed;
		}

		int32 ZOrder = 0;
		GameTestJson::TryGetInt(Params, TEXT("zOrder"), ZOrder);

		UUserWidget* Widget = nullptr;
		if (!GameTestOps::AddWidgetToViewport(InContext.World, ClassPath, ZOrder, Widget, InContext.Message))
		{
			return StepFailed;
		}

		InContext.Payload->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
		InContext.Payload->SetStringField(TEXT("name"), Widget->GetFName().ToString());
		return StepSucceeded;
	}

	EGameTestStepStatus HandleWidgetRemove(FGameTestStepContext& InContext)
	{
		TSharedPtr<FGameTestWidgetNode> Node;
		FString Error;
		if (!TryResolveStepTarget(InContext, Node, Error))
		{
			InContext.Message = Error;
			return StepFailed;
		}

		UUserWidget* UserWidget = Cast<UUserWidget>(Node->Widget.Get());
		if (UserWidget == nullptr)
		{
			InContext.Message = FString::Printf(TEXT("%s 不是 UUserWidget（widget.remove 只对整屏有效）"), *Node->Path);
			return StepFailed;
		}

		if (!GameTestOps::RemoveWidget(UserWidget, InContext.Message))
		{
			return StepFailed;
		}

		return StepSucceeded;
	}
}
