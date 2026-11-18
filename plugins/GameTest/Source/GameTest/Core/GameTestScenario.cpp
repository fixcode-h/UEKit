// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestScenario.h"

#include "Core/GameTestJson.h"
#include "Core/GameTestRunConfig.h"
#include "Operate/GameTestMove.h"
#include "Operate/GameTestSkipIf.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

bool GameTestScenario::Validate(const TSharedPtr<FJsonObject>& InJson, TArray<FString>& OutErrors)
{
	if (!InJson.IsValid())
	{
		OutErrors.Add(TEXT("场景不是 JSON 对象"));
		return false;
	}

	FString Kind;
	if (!GameTestJson::TryGetString(*InJson, TEXT("kind"), Kind) || Kind.IsEmpty())
	{
		OutErrors.Add(FString::Printf(TEXT("缺少必填字段 kind（应为 '%s'）"), GetKind()));
	}
	else if (!Kind.Equals(GetKind(), ESearchCase::IgnoreCase))
	{
		OutErrors.Add(FString::Printf(TEXT("kind 应为 '%s'，收到 '%s'"), GetKind(), *Kind));
	}

	int32 SchemaVersion = 0;
	if (!GameTestJson::TryGetInt(*InJson, TEXT("schemaVersion"), SchemaVersion))
	{
		OutErrors.Add(FString::Printf(TEXT("缺少必填字段 schemaVersion（当前支持 %d）"), SupportedSchemaVersion));
	}
	else if (SchemaVersion <= 0)
	{
		OutErrors.Add(FString::Printf(TEXT("schemaVersion 应为正数，收到 %d"), SchemaVersion));
	}
	else if (SchemaVersion > SupportedSchemaVersion)
	{
		// 明确拒绝"未来版本"：宁可现在停下，也不要按旧语义跑一份新配置。
		OutErrors.Add(FString::Printf(TEXT("schemaVersion %d 高于本插件支持的 %d（升级插件或把场景降到支持的版本）"),
			SchemaVersion, SupportedSchemaVersion));
	}

	FString Name;
	if (!GameTestJson::TryGetString(*InJson, TEXT("name"), Name) || Name.IsEmpty())
	{
		OutErrors.Add(TEXT("缺少必填字段 name（场景名，进报告与产物目录）"));
	}

	const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
	if (!InJson->TryGetArrayField(TEXT("steps"), Steps) || Steps == nullptr || Steps->Num() == 0)
	{
		OutErrors.Add(TEXT("缺少非空的 steps 数组"));
	}

	return OutErrors.Num() == 0;
}

void GameTestScenario::ApplyLegacyDefaults(const TSharedPtr<FJsonObject>& InJson, TArray<TSharedPtr<FJsonObject>>& InOutSteps)
{
	int32 SchemaVersion = 0;
	if (!InJson.IsValid() || !GameTestJson::TryGetInt(*InJson, TEXT("schemaVersion"), SchemaVersion) || SchemaVersion >= 2)
	{
		return;
	}

	for (const TSharedPtr<FJsonObject>& Step : InOutSteps)
	{
		FString Action;
		if (!Step.IsValid() || !GameTestJson::TryGetString(*Step, TEXT("action"), Action) || Step->HasField(TEXT("via")))
		{
			continue;
		}
		if (Action == TEXT("click") || Action == TEXT("type"))
		{
			Step->SetStringField(TEXT("via"), TEXT("code"));
		}
	}
}

bool GameTestScenario::CollectSteps(
	const TSharedPtr<FJsonObject>& InJson,
	TArray<TSharedPtr<FJsonObject>>& OutSteps,
	TArray<FString>& OutErrors)
{
	OutSteps.Reset();

	if (!Validate(InJson, OutErrors))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
	InJson->TryGetArrayField(TEXT("steps"), Steps);

	for (int32 Index = 0; Index < Steps->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* StepObject = nullptr;
		if ((*Steps)[Index].IsValid() && (*Steps)[Index]->TryGetObject(StepObject) && StepObject != nullptr)
		{
			OutSteps.Add(*StepObject);
		}
		else
		{
			OutErrors.Add(FString::Printf(TEXT("第 %d 步不是 JSON 对象"), Index + 1));
		}
	}

	return OutErrors.Num() == 0;
}

FString GameTestScenario::GetName(const TSharedPtr<FJsonObject>& InJson, const FString& InFallback)
{
	FString Name;
	if (InJson.IsValid() && GameTestJson::TryGetString(*InJson, TEXT("name"), Name) && !Name.IsEmpty())
	{
		return Name;
	}

	return InFallback;
}

int32 GameTestScenario::GetRevision(const TSharedPtr<FJsonObject>& InJson)
{
	int32 Revision = 0;
	if (InJson.IsValid())
	{
		GameTestJson::TryGetInt(*InJson, TEXT("revision"), Revision);
	}
	return FMath::Max(0, Revision);
}

TArray<FString> GameTestScenario::GetTags(const TSharedPtr<FJsonObject>& InJson)
{
	TArray<FString> Tags;
	if (!InJson.IsValid())
	{
		return Tags;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = GameTestJson::TryGetArray(*InJson, TEXT("tags"));
	if (Values == nullptr)
	{
		return Tags;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString Tag;
		// 非字符串项直接跳过而不是报错：tags 是标注信息，不该因为写了个数字就让整份场景不可用。
		if (Value.IsValid() && Value->TryGetString(Tag))
		{
			Tag.TrimStartAndEndInline();
			if (!Tag.IsEmpty())
			{
				Tags.Add(Tag);
			}
		}
	}

	return Tags;
}

TArray<FGameTestScenarioInfo> GameTestScenario::ScanDirectory(const FString& InRootDir)
{
	TArray<FGameTestScenarioInfo> Scenarios;

	// `MakePathRelativeTo` 要求"相对谁"那一边以分隔符结尾，否则它匹配不上、把根目录名一起留在
	// 相对路径里（实测：根给 `.../gametest` 时得到 `gametest/pie/x.json`，而契约要的是 `pie/x.json`）。
	// 调用方给的多半是不带尾斜杠的绝对路径，所以在这里补齐，不指望每个调用点都记着。
	FString RootDir = InRootDir;
	if (!RootDir.EndsWith(TEXT("/")))
	{
		RootDir.AppendChar(TEXT('/'));
	}

	TArray<FString> Found;
	IFileManager::Get().FindFilesRecursive(Found, *InRootDir, TEXT("*.json"), /*Files=*/true, /*Directories=*/false);

	for (const FString& AbsolutePath : Found)
	{
		// 场景根下的运行配置（阻挡界面规则、UI 等待上限）不是场景，不列出来。
		if (FPaths::GetCleanFilename(AbsolutePath) == GameTestRunConfig::FileName()
			&& FPaths::IsSamePath(FPaths::GetPath(AbsolutePath), InRootDir))
		{
			continue;
		}

		FGameTestScenarioInfo Info;
		Info.AbsolutePath = AbsolutePath;

		FString Relative = AbsolutePath;
		FPaths::MakePathRelativeTo(Relative, *RootDir);
		Info.RelativePath = Relative;
		Info.Name = FPaths::GetBaseFilename(Relative);

		FString ParseError;
		const TSharedPtr<FJsonObject> Json = GameTestJson::ParseObjectFromFile(AbsolutePath, ParseError);
		if (Json.IsValid())
		{
			// 走与运行同一条门禁：列举里"看起来能跑"的场景就必须真能跑。
			TArray<FString> Errors;
			TArray<TSharedPtr<FJsonObject>> Steps;
			if (GameTestScenario::CollectSteps(Json, Steps, Errors))
			{
				Info.StepCount = Steps.Num();
			}
			else
			{
				Info.ParseError = FString::Join(Errors, TEXT("；"));
			}

			Info.Name = GameTestScenario::GetName(Json, Info.Name);
			GameTestJson::TryGetString(*Json, TEXT("_note"), Info.Note);
			Info.Revision = GameTestScenario::GetRevision(Json);
			Info.Tags = GameTestScenario::GetTags(Json);
		}
		else
		{
			Info.ParseError = ParseError;
		}

		Scenarios.Add(MoveTemp(Info));
	}

	// 顺序稳定：清单每次刷新、每次列举都长一个样，AI 比较两次结果才有意义。
	Scenarios.Sort([](const FGameTestScenarioInfo& InLeft, const FGameTestScenarioInfo& InRight)
	{
		return InLeft.RelativePath < InRight.RelativePath;
	});

	return Scenarios;
}

bool GameTestScenario::LoadForRun(const FString& InRootDir, const FString& InPath, FGameTestScenarioRun& OutRun, FString& OutError)
{
	FString FullPath = InPath;
	if (FPaths::IsRelative(FullPath))
	{
		FullPath = FPaths::Combine(InRootDir, InPath);
	}
	FullPath = FPaths::ConvertRelativePathToFull(FullPath);

	// 历史按相对路径记：同一个文件不管调用方写的是相对还是绝对路径，都要落到同一个键上
	FString Relative = FullPath;
	FString Root = FPaths::ConvertRelativePathToFull(InRootDir);
	if (!Root.EndsWith(TEXT("/")))
	{
		Root.AppendChar(TEXT('/'));
	}
	if (FPaths::MakePathRelativeTo(Relative, *Root) && !Relative.StartsWith(TEXT("..")))
	{
		OutRun.Scenario = Relative.Replace(TEXT("\\"), TEXT("/"));
	}
	else
	{
		OutRun.Scenario = InPath.Replace(TEXT("\\"), TEXT("/"));
	}

	const TSharedPtr<FJsonObject> Json = GameTestJson::ParseObjectFromFile(FullPath, OutError);
	if (!Json.IsValid())
	{
		OutError = FString::Printf(TEXT("%s：%s"), *InPath, *OutError);
		return false;
	}

	TArray<FString> Errors;
	OutRun.Steps.Reset();
	if (!CollectSteps(Json, OutRun.Steps, Errors))
	{
		OutError = FString::Printf(TEXT("%s：%s"), *InPath, *FString::Join(Errors, TEXT("；")));
		return false;
	}

	ApplyLegacyDefaults(Json, OutRun.Steps);
	OutRun.Json = Json;
	OutRun.Name = GetName(Json, FPaths::GetBaseFilename(InPath));
	OutRun.Revision = GetRevision(Json);
	OutRun.StepDelayMs = -1;
	GameTestJson::TryGetInt(*Json, TEXT("stepDelayMs"), OutRun.StepDelayMs);
	OutRun.UiWaitTimeoutMs = -1;
	GameTestJson::TryGetInt(*Json, TEXT("uiWaitTimeoutMs"), OutRun.UiWaitTimeoutMs);
	OutRun.ClickVia.Reset();
	GameTestJson::TryGetString(*Json, TEXT("clickVia"), OutRun.ClickVia);
	OutRun.Move = GameTestJson::TryGetObject(*Json, TEXT("move"));
	if (OutRun.Move.IsValid() && !GameTestMove::ValidateDefaults(*OutRun.Move, OutError))
	{
		OutError = FString::Printf(TEXT("%s：%s"), *InPath, *OutError);
		return false;
	}
	bool bSetup = true;
	OutRun.bSkipSetup = GameTestJson::TryGetBool(*Json, TEXT("setup"), bSetup) && !bSetup;
	OutRun.SkipIf = Json->TryGetField(TEXT("skipIf"));
	if (OutRun.SkipIf.IsValid())
	{
		FGameTestSkipOptions Options;
		if (!GameTestSkipIf::ApplyOptions(*OutRun.SkipIf, Options, OutError))
		{
			OutError = FString::Printf(TEXT("%s：%s"), *InPath, *OutError);
			return false;
		}
	}
	return true;
}

TArray<FString> GameTestScenario::FindByTag(const FString& InRootDir, const FString& InTag)
{
	TArray<FString> Out;
	for (const FGameTestScenarioInfo& Info : ScanDirectory(InRootDir))
	{
		if (Info.Tags.ContainsByPredicate([&InTag](const FString& InItem) { return InItem.Equals(InTag, ESearchCase::IgnoreCase); }))
		{
			Out.Add(Info.RelativePath);
		}
	}
	return Out;
}
