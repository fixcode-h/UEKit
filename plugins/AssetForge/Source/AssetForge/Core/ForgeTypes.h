// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 契约层：配置文档的中间表示，与具体的资产类型（UMG / 动画蓝图 / 行为树）无关。
 *
 * 这些类型刻意不带 USTRUCT——它们不被蓝图消费、不进序列化通道，只在 C++ 内部流转；
 * 加反射只会让算法层被迫拉进 UObject 体系。命名沿用 UE 给结构体的 F 前缀惯例。
 */

/** 诊断级别。 */
enum class EForgeSeverity : uint8
{
	Error,
	Warning,
	Info,
};

/**
 * 一条诊断。
 *
 * FieldPath 是**字段级路径**（如 `root.children[2].props.Visibility`）而不是行号：
 * UE 的 JSON 解析器不提供行列信息，而字段路径既能让 AI 精确定位，也能直接对回配置。
 */
struct ASSETFORGE_API FForgeDiagnostic
{
	EForgeSeverity Severity = EForgeSeverity::Error;
	FString FieldPath;
	FString Message;

	FForgeDiagnostic() = default;

	FForgeDiagnostic(EForgeSeverity InSeverity, FString InFieldPath, FString InMessage)
		: Severity(InSeverity)
		, FieldPath(MoveTemp(InFieldPath))
		, Message(MoveTemp(InMessage))
	{
	}
};

/** 一个配置节点的种类。 */
enum class EForgeAction : uint8
{
	Create,
	Update,
	Delete,
	Reorder,
	Unchanged,
};

/** 计划中的一条动作。 */
struct ASSETFORGE_API FForgePlanEntry
{
	EForgeAction Action = EForgeAction::Unchanged;

	/** 节点在树里的路径，如 `root/Canvas/HealthBar`。 */
	FString NodePath;

	/** 控件的稳定标识（= UWidget 的 FName）。 */
	FString NodeName;

	/** 补充说明（属性名、旧序→新序、删除原因等）。 */
	FString Detail;
};

/**
 * 一个配置节点。
 *
 * Name 是**导入时的匹配键**：UE 5.8 已删除 UWidget::WidgetGuid，控件身份由
 * UWidgetBlueprint::WidgetVariableNameToGuidMap 以「变量名 → GUID」持久化，
 * 所以配置里的 name 就是引擎自己认的稳定身份。
 */
struct ASSETFORGE_API FForgeNode
{
	/** 控件类：短名（`TextBlock`，按 /Script/UMG 解析）或完整路径（`/Game/...`）。 */
	FString ClassName;

	/** 控件名（= UWidget 的 FName），同一父节点下必须唯一。 */
	FString Name;

	/** 可选的配置文件引用（相对配置根 `Docs/AssetForge/` 的路径）；与 Props.WidgetType 二选一。 */
	FString Ref;

	/** 通用属性（键 = UE 属性名原样）。 */
	TSharedPtr<FJsonObject> Props;

	/** 该节点在其父面板中的插槽属性。 */
	TSharedPtr<FJsonObject> Slot;

	/** 子节点，顺序即控件树顺序。 */
	TArray<FForgeNode> Children;

	/**
	 * 各 kind 自己的扩展字段（如行为树节点上的 decorators / services）。
	 *
	 * 通用管道不认识它们，只负责原样保留——这样加一种资产类型时不必为了几个专属字段
	 * 去改契约层（那是所有 kind 共用的东西）。
	 */
	TSharedPtr<FJsonObject> Extra;
};

/** 一份资产配置文档。 */
struct ASSETFORGE_API FForgeDocument
{
	/** 配置文件的磁盘路径（绝对），用于诊断与清单回写。 */
	FString SourcePath;

	/** 资产种类：`umg-widget` 起手，后续可扩展。 */
	FString Kind;

	/** 格式版本：本插件解析不了的版本直接拒绝，不猜测。 */
	int32 SchemaVersion = 0;

	/** 内容修订号：人/AI 维护，仅用于校验与警告（增量判定走内容哈希）。 */
	int32 Revision = 0;

	/** 目标资产路径，如 `/Game/UI/HUD/WBP_PlayerHUD`。 */
	FString Asset;

	/** 父类路径：C++ 类（`/Script/MyGame.HUDWidget`）或蓝图类（`/Game/..._C`）。 */
	FString Parent;

	/** 根节点。 */
	FForgeNode Root;

	/** 配置文件里未被本插件识别的顶层字段，原样保留以便回写时不丢信息。 */
	TSharedPtr<FJsonObject> Extra;
};

/** 导入结果。 */
struct ASSETFORGE_API FForgeReport
{
	bool bSucceeded = false;
	bool bDryRun = false;

	FString SourcePath;
	FString AssetPath;

	TArray<FForgeDiagnostic> Diagnostics;

	/** 计划中将要执行（dry-run）或已执行（真跑）的动作。 */
	TArray<FForgePlanEntry> Actions;

	/** 统计，便于 AI 与 CLI 快速判断规模。 */
	int32 Created = 0;
	int32 Updated = 0;
	int32 Deleted = 0;

	/** 按动作清单重算三个计数。各 kind 的导入器在填完 Actions 后调它，保证报告口径一致。 */
	void CountActions()
	{
		Created = 0;
		Updated = 0;
		Deleted = 0;

		for (const FForgePlanEntry& Entry : Actions)
		{
			switch (Entry.Action)
			{
			case EForgeAction::Create:
				++Created;
				break;
			case EForgeAction::Update:
				++Updated;
				break;
			case EForgeAction::Delete:
				++Deleted;
				break;
			default:
				break;
			}
		}
	}

	bool HasErrors() const
	{
		for (const FForgeDiagnostic& Diagnostic : Diagnostics)
		{
			if (Diagnostic.Severity == EForgeSeverity::Error)
			{
				return true;
			}
		}
		return false;
	}

	void AddError(FString InFieldPath, FString InMessage)
	{
		Diagnostics.Emplace(EForgeSeverity::Error, MoveTemp(InFieldPath), MoveTemp(InMessage));
	}

	void AddWarning(FString InFieldPath, FString InMessage)
	{
		Diagnostics.Emplace(EForgeSeverity::Warning, MoveTemp(InFieldPath), MoveTemp(InMessage));
	}
};
