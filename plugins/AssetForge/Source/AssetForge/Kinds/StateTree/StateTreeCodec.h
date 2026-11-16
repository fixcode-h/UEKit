// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

// 只在 ASSETFORGE_WITH_STATETREE 时有内容（UE 5.8），见 AssetForge.Build.cs
#if ASSETFORGE_WITH_STATETREE

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

struct FInstancedPropertyBag;
class UStateTree;
class UStateTreeEditorData;
class UStateTreeState;
struct FStateTreeEditorNode;
struct FStateTreeTransition;

/**
 * 状态树的编解码：`UStateTree` 的编辑器数据 ↔ 配置文档。
 *
 * 分工与其余 kind 一致——本层只做「资产 ↔ 配置世界」的转换，不含同步策略
 * （谁建谁删、增量匹配）与编译；那些在 `StateTreeForge`。
 *
 * 结构映射（设计决策 2）：
 * - 状态做 `FForgeNode`（`children` = 子状态），状态自身的字段与内部列表进 `Extra`；
 * - 节点（任务 / 条件 / 考量 / 评估器 / 全局任务）在 `Extra` 里是 JSON 数组，
 *   元素形如 `{ "type": "...", "name": "...", "props": { ... } }`。
 *
 * 引用格式（设计决策 4，沿用引擎自身的拼接）：
 * - 状态路径 = `UStateTreeState::GetPath()`（`Root/Combat/Attack`）；
 * - 可绑定描述符引用 = `StatePath/Name`（`Global/Parameters`、`Root/Combat/Wait`），
 *   属性路径以 `.` 追加。
 */
namespace StateTreeCodec
{
	/** 文档顶层与状态 / 节点 / 转移的字段名（导出与导入共用，防止两边漂移）。 */
	namespace Field
	{
		// 资产级
		const TCHAR* const Schema = TEXT("schema");
		const TCHAR* const Parameters = TEXT("parameters");
		const TCHAR* const Evaluators = TEXT("evaluators");
		const TCHAR* const GlobalTasks = TEXT("globalTasks");
		const TCHAR* const GlobalTasksCompletion = TEXT("globalTasksCompletion");
		const TCHAR* const Bindings = TEXT("bindings");
		const TCHAR* const Source = TEXT("source");
		const TCHAR* const Target = TEXT("target");

		// 状态
		const TCHAR* const Type = TEXT("type");
		const TCHAR* const Selection = TEXT("selection");
		const TCHAR* const TasksCompletion = TEXT("tasksCompletion");
		const TCHAR* const Tag = TEXT("tag");
		const TCHAR* const Description = TEXT("description");
		const TCHAR* const Weight = TEXT("weight");
		const TCHAR* const Enabled = TEXT("enabled");
		const TCHAR* const CustomTickRate = TEXT("customTickRate");
		const TCHAR* const CopyParameterBindingsOnTick = TEXT("copyParameterBindingsOnTick");
		const TCHAR* const CheckPrerequisites = TEXT("checkPrerequisitesWhenActivatingChildDirectly");
		const TCHAR* const LinkedAsset = TEXT("linkedAsset");
		const TCHAR* const LinkedSubtree = TEXT("linkedSubtree");
		const TCHAR* const RequiredEvent = TEXT("requiredEvent");
		const TCHAR* const EnterConditions = TEXT("enterConditions");
		const TCHAR* const Tasks = TEXT("tasks");
		const TCHAR* const Considerations = TEXT("considerations");
		const TCHAR* const SingleTask = TEXT("singleTask");
		const TCHAR* const Transitions = TEXT("transitions");

		// 节点
		const TCHAR* const Name = TEXT("name");
		const TCHAR* const Props = TEXT("props");

		// 转移
		const TCHAR* const Trigger = TEXT("trigger");
		const TCHAR* const Event = TEXT("event");
		const TCHAR* const To = TEXT("to");
		const TCHAR* const Priority = TEXT("priority");
		const TCHAR* const Delay = TEXT("delay");
		const TCHAR* const DelayVariance = TEXT("delayVariance");
		const TCHAR* const Conditions = TEXT("conditions");
		const TCHAR* const Consume = TEXT("consume");
		const TCHAR* const Payload = TEXT("payload");

		// 事件
		const TCHAR* const EventTag = TEXT("tag");
	}

	// ---- 导出方向 ----

	/** 把资产读成配置文档（`root` = 主根状态；资产级字段进 `Extra`）。 */
	ASSETFORGE_API void ExportDocument(
		const UStateTree& InTree,
		FForgeDocument& OutDocument,
		TArray<FForgeDiagnostic>& OutDiagnostics);

	// ---- 引用（绑定）服务，导出与导入共用 ----

	/** 收集全部可绑定结构：StructID → 引用字符串（`StatePath/Name`）。 */
	ASSETFORGE_API void CollectStructReferences(
		const UStateTreeEditorData& InEditorData,
		TMap<FGuid, FString>& OutReferences);

	/**
	 * 解析一个绑定引用（`StatePath/Name[.属性路径]`）：匹配到唯一描述符时输出其 ID 与
	 * 属性路径字符串；匹配不到或多个时返回 false 并给出错误。
	 */
	ASSETFORGE_API bool ResolveStructReference(
		const UStateTreeEditorData& InEditorData,
		const FString& InReference,
		FGuid& OutStructID,
		FString& OutPropertyPath,
		FString& OutError);

	// ---- 节点类型解析（导入方向） ----

	/**
	 * 解析配置里的 `type`：返回节点结构（C++ 节点）或蓝图类（二选一非空）。
	 * 两者都解析不到时报错。
	 */
	ASSETFORGE_API bool ResolveNodeType(
		const FString& InType,
		UScriptStruct*& OutStruct,
		UClass*& OutClass,
		FString& OutError);

	/** 编辑节点与目标类型是否一致（struct 节点比结构，蓝图节点比类）。 */
	ASSETFORGE_API bool IsNodeTypeMatch(
		const FStateTreeEditorNode& InNode,
		UScriptStruct* InStruct,
		UClass* InClass);

	/** 按名字解析枚举（接受短名与 `Enum::Value` 全名）；导入方向解析配置用。 */
	ASSETFORGE_API bool ParseEnumByName(
		const UEnum* InEnum,
		const FString& InText,
		int64& OutValue,
		FString& OutError);

	// ---- 参数（PropertyBag）编解码 ----

	/** 导出参数：属性名 → 值；范围外类型给诊断并跳过。 */
	ASSETFORGE_API void ExportPropertyBag(
		const FInstancedPropertyBag& InBag,
		TSharedPtr<FJsonObject>& OutJson,
		TArray<FForgeDiagnostic>& OutDiagnostics,
		const FString& InFieldPath);

	/**
	 * 按配置重建参数集合：配置里有的（重建类型 + 写值），配置里没有的删除。
	 * 类型不支持时给诊断，不影响其余字段。
	 */
	ASSETFORGE_API void ImportPropertyBag(
		FInstancedPropertyBag& InOutBag,
		const TSharedPtr<FJsonObject>& InJson,
		TArray<FForgeDiagnostic>& OutDiagnostics,
		const FString& InFieldPath);
}

#endif // ASSETFORGE_WITH_STATETREE
