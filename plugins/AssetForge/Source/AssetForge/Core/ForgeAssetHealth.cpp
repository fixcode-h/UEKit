#include "Core/ForgeAssetHealth.h"

#include "Engine/Blueprint.h"
#include "Editor.h"
#include "AssetForge.h"
#include "Logging/TokenizedMessage.h"
#include "EditorValidatorSubsystem.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/Material.h"
#include "MaterialEditorUtilities.h"
#include "Materials/MaterialInterface.h"
#include "RHIDefinitions.h"

namespace ForgeAssetHealth
{

const TCHAR* const FieldPath = TEXT("asset.health");

namespace
{
	/**
	 * 蓝图：**强制编译一次**再读编译器日志。
	 *
	 * 为什么是"强制"：导入只写了属性，蓝图的编译状态可能是"未编译"——那时
	 * `Status` 看起来正常，而真正的错误要编译才会现形（正是 M_ForgeShowcase 那类
	 * "编辑器里红字、管道没反应"的来源）。
	 */
	void CheckBlueprint(UBlueprint* InBlueprint, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		FCompilerResultsLog Results;
		Results.bLogInfoOnly = false;
		Results.bAnnotateMentionedNodes = false;

		FKismetEditorUtilities::CompileBlueprint(
			InBlueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			const EMessageSeverity::Type Severity = Message->GetSeverity();
			if (Severity != EMessageSeverity::Error && Severity != EMessageSeverity::Warning)
			{
				continue;
			}

			OutDiagnostics.Emplace(
				Severity == EMessageSeverity::Error ? EForgeSeverity::Error : EForgeSeverity::Warning,
				FieldPath,
				Message->ToText().ToString());
		}
	}

	/**
	 * 材质：**先强制重编**，再读编译产物上的错误列表。
	 *
	 * 为什么必须重编：`GetMaterialResource` 读的是"最近一次成功编译"的产物——配置改坏了
	 * 但还没重编时它里面是旧数据（甚至为空），于是"导入端看不到材质报错"（2026-09-15
	 * 实测 M_ForgeShowcase：编辑器里报错、管道侧一无所知）。
	 */
	void CheckMaterial(UMaterialInterface* InMaterial, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		UMaterial* BaseMaterial = InMaterial->GetMaterial();
		if (BaseMaterial == nullptr)
		{
			return;
		}

		// 强制重编：把"配置改坏了但还没重编"的状态逼出来——否则读到的永远是旧产物。
		BaseMaterial->ForceRecompileForRendering();

		// 再走一次**编辑器编译路径**：节点级诊断（如「Sampler type is Color, should be Linear Color」）
		// 是编辑器编译填进各表达式 `LastErrorText` 的——渲染侧重编不写它（实测）。
		// 标准编辑循环：只有它才稳定触发编辑器侧重编（PostEditChange 单独调可能因'无修改'被跳过）。
		BaseMaterial->Modify();
		BaseMaterial->PreEditChange(nullptr);
		BaseMaterial->PostEditChange();

		const FMaterialResource* Resource = BaseMaterial->GetMaterialResource(GMaxRHIShaderPlatform);
		if (Resource != nullptr)
		{
			for (const FString& Error : Resource->GetCompileErrors())
			{
				OutDiagnostics.Emplace(EForgeSeverity::Error, FieldPath, Error);
			}
		}

		// **节点级诊断**：`GetCompileErrors` 只给"硬错误"，而编辑器「统计」面板里的告警
		// （如「SM6_Q (Node_TextureSample) Sampler type is Color, should be Linear Color」）
		// 落在各表达式自己的 `LastErrorText` 上——逐个收，按警告报
		// （需求侧 2026-09-15 指认 M_ForgeShowcase 的报错即此类）。
		if (const UMaterialEditorOnlyData* EditorOnly = BaseMaterial->GetEditorOnlyData())
		{
			for (const TObjectPtr<UMaterialExpression>& Expression : EditorOnly->ExpressionCollection.Expressions)
			{
				if (Expression != nullptr && !Expression->LastErrorText.IsEmpty())
				{
					OutDiagnostics.Emplace(
						EForgeSeverity::Warning,
						FieldPath,
						FString::Printf(TEXT("%s: %s"), *Expression->GetName(), *Expression->LastErrorText));
				}
			}
		}
	}

	/**
	 * 通用验证：编辑器验证框架（DataValidation）对**所有资产类型**跑注册的验证器——
	 * 蓝图/材质/Niagara/数据资产各有自己的规则，比"按类型硬编码检查"覆盖得全。
	 * 错误与警告都收进报告（需求侧 2026-09-15：警告也要暴露）。
	 */
	void CheckValidation(UObject* InAsset, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		if (GEditor == nullptr)
		{
			return;
		}

		UEditorValidatorSubsystem* Validator = GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>();
		if (Validator == nullptr)
		{
			return;
		}

		FValidateAssetsSettings Settings;
		Settings.bCollectPerAssetDetails = true;
		Settings.bLoadAssetsForValidation = false;   // 资产就在内存里（刚导入完）
		Settings.ValidationUsecase = EDataValidationUsecase::Manual;
		Settings.bCaptureAssetLoadLogs = true;       // 加载期的错误与警告也算结果
		Settings.bCaptureLogsDuringValidation = true; // 验证期间写进日志的警告一并捕获

		TArray<FAssetData> AssetList;
		AssetList.Add(FAssetData(InAsset));

		FValidateAssetsResults Results;
		Validator->ValidateAssetsWithSettings(AssetList, Settings, Results);

		// 逐条详情（按对象路径索引）：错误与警告都要。
		for (const TPair<FString, FValidateAssetsDetails>& Pair : Results.AssetsDetails)
		{
			for (const FText& Error : Pair.Value.ValidationErrors)
			{
				OutDiagnostics.Emplace(EForgeSeverity::Error, FieldPath, Error.ToString());
			}
			for (const FText& Warning : Pair.Value.ValidationWarnings)
			{
				OutDiagnostics.Emplace(EForgeSeverity::Warning, FieldPath, Warning.ToString());
			}
		}

		// 跨资产的消息（如"引用了缺失资产"），同样错误与警告都要。
		for (const TSharedRef<FTokenizedMessage>& Message : Results.ValidatorMessages)
		{
			const EMessageSeverity::Type Severity = Message->GetSeverity();
			if (Severity == EMessageSeverity::Error || Severity == EMessageSeverity::Warning)
			{
				OutDiagnostics.Emplace(
					Severity == EMessageSeverity::Error ? EForgeSeverity::Error : EForgeSeverity::Warning,
					FieldPath,
					Message->ToText().ToString());
			}
		}
	}
}

/**
 * 资产健康检查：**两条**互补的覆盖面，都只依赖传入的这个资产本身。
 *
 * - 类型专属的强制编译（蓝图 / 材质）：把"还没编译"的状态逼出来。
 * - 通用验证框架（DataValidation）：所有资产类型，错误与警告都收。
 *
 * ⚠️ **不要再用「清空全局 Message Log 列表 → 重编 → 读回」那一套**。它曾短暂存在过，已删除：
 * 那些列表是全局共享的——清空会破坏用户正看着的面板、读取会引入与本次导入无关的陈旧消息
 * （`LoadErrors` 累积、`MapCheck` 由用户手动触发、`PIE` 只在运行时写），而 `ForgePipeline`
 * 会把任何 Error 判成导入失败，于是健康的导入会永久失败。按名读取未注册的列表还会在用户的
 * 日志窗口里创建空标签项。更直接的是：那条通道按 11 个硬编码名字读，**实测一条都读不到**
 * （`openspec/changes/archive/2026-09-15-asset-forge/tasks.md` 有记录）。
 * 真要捕某类消息，方向是 hook 写入侧或读对应工具的统计快照，不是读回全局列表。
 */
void CheckAsset(UObject* InAsset, TArray<FForgeDiagnostic>& OutDiagnostics)
{
	if (InAsset == nullptr)
	{
		return;
	}

	if (UBlueprint* Blueprint = Cast<UBlueprint>(InAsset))
	{
		CheckBlueprint(Blueprint, OutDiagnostics);
	}
	else if (UMaterialInterface* Material = Cast<UMaterialInterface>(InAsset))
	{
		CheckMaterial(Material, OutDiagnostics);
	}

	// 通用验证：所有类型都跑（覆盖蓝图/材质之外的资产，且给出各自的错误与警告）。
	CheckValidation(InAsset, OutDiagnostics);
}

}
