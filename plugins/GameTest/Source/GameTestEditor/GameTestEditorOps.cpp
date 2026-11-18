// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameTestEditorOps.h"

#include "Api/GameTestCppApiScanner.h"
#include "Widgets/GameTestWidgetTreeReader.h"

#include "AgentPanelBridge.h"
#include "AgentPanelBridgeUtil.h"
#include "Dom/JsonObject.h"

namespace GameTestEditorOps
{
namespace
{
	TArray<FString> RegisteredOps;

	/** gt.widgets：读 WBP 资产的控件树（不用开游戏）。 */
	bool Widgets(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		FGameTestWidgetTreeQuery Query;
		Query.Asset = AgentPanel::Str(Args, TEXT("asset"));
		Query.Class = AgentPanel::Str(Args, TEXT("class"));
		Query.Filter = AgentPanel::Str(Args, TEXT("filter"));
		Query.Offset = AgentPanel::Int(Args, TEXT("offset"), 0);
		Query.Limit = AgentPanel::Int(Args, TEXT("limit"), 200);
		return GameTestWidgetTreeReader::Read(Query, Out, Error);
	}

	/** gt.api.scan：重扫 C++ 项目接口，写 _api.json 的 cpp 段（dryRun 只扫不写）。 */
	bool ApiScan(const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& Out, FString& Error)
	{
		const bool bDryRun = AgentPanel::Bool(Args, TEXT("dryRun"), false);
		FGameTestCppApiScanResult Result;
		if (!GameTestCppApiScanner::ScanAndWrite(bDryRun, Result, Error))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
		Counts->SetNumberField(TEXT("items"), Result.Items);
		Counts->SetNumberField(TEXT("rejected"), Result.Rejected);
		Out->SetObjectField(TEXT("counts"), Counts);
		Out->SetStringField(TEXT("path"), Result.Path);
		Out->SetBoolField(TEXT("changed"), Result.bChanged);
		if (bDryRun)
		{
			Out->SetBoolField(TEXT("dryRun"), true);
		}
		// 不能用的函数原样列出（为什么不能用）：一般不多，写项目接口的人最想看这个
		if (Result.Section.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Rejected = nullptr;
			if (Result.Section->TryGetArrayField(TEXT("rejected"), Rejected) && Rejected != nullptr && Rejected->Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Head(Rejected->GetData(), FMath::Min(Rejected->Num(), 30));
				Out->SetArrayField(TEXT("rejected"), Head);
			}
			const TArray<TSharedPtr<FJsonValue>>* Folders = nullptr;
			if (Result.Section->TryGetArrayField(TEXT("folders"), Folders) && Folders != nullptr)
			{
				Out->SetArrayField(TEXT("folders"), *Folders);
			}
		}
		return true;
	}

	void Add(const TCHAR* Name, bool (*Op)(const TSharedPtr<FJsonObject>&, TSharedPtr<FJsonObject>&, FString&))
	{
		if (FAgentPanelBridge::Get().AddExtensionOp(Name, Op))
		{
			RegisteredOps.Add(Name);
		}
	}
}

void Register()
{
	Add(TEXT("gt.widgets"), &Widgets);
	Add(TEXT("gt.api.scan"), &ApiScan);
}

void Unregister()
{
	for (const FString& Name : RegisteredOps)
	{
		FAgentPanelBridge::Get().RemoveOp(Name);
	}
	RegisteredOps.Reset();
}
}
