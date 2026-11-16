// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Niagara/NiagaraForge.h"
#include "AssetForgeCompat.h"

#include "Core/ForgeDiff.h"
#include "Core/ForgeDoc.h"
#include "Core/ForgeJsonText.h"
#include "Core/ForgePipeline.h"
#include "Core/ForgePropertyCodec.h"

#include "NiagaraEditorUtilities.h"
#include "NiagaraSystemFactoryNew.h"
#include "AssetForge.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "NiagaraUserRedirectionParameterStore.h"

#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace NiagaraForge
{

namespace
{
	// ------------------------------------------------------------------
	// 资产与模板
	// ------------------------------------------------------------------

	UNiagaraSystem* FindSystem(const FString& InAssetPath)
	{
		if (InAssetPath.IsEmpty())
		{
			return nullptr;
		}

		return Cast<UNiagaraSystem>(LoadObject<UObject>(nullptr, *InAssetPath));
	}

	/** 模板引用解析：兼容 `/Path/X` 与 `/Path/X.X` 两种写法。 */
	UNiagaraEmitter* ResolveTemplate(const FString& InReference, FString& OutError)
	{
		if (InReference.IsEmpty())
		{
			OutError = TEXT("缺少模板来源：发射器的 class 必须是模板 Emitter 的资产路径（如 `/Niagara/DefaultAssets/Templates/Emitters/SimpleSpriteBurst`）");
			return nullptr;
		}

		FString ObjectPath = InReference;
		if (!ObjectPath.Contains(TEXT(".")))
		{
			const FString AssetName = FPackageName::GetShortName(ObjectPath);
			ObjectPath = FString::Printf(TEXT("%s.%s"), *ObjectPath, *AssetName);
		}

		UNiagaraEmitter* Emitter = LoadObject<UNiagaraEmitter>(nullptr, *ObjectPath);
		if (Emitter == nullptr)
		{
			OutError = FString::Printf(TEXT("找不到模板 Emitter：`%s`"), *InReference);
		}

		return Emitter;
	}

	FNiagaraEmitterHandle* FindHandleByName(UNiagaraSystem& InSystem, FName InName)
	{
		for (FNiagaraEmitterHandle& Handle : InSystem.GetEmitterHandles())
		{
			if (Handle.IsValid() && Handle.GetName() == InName)
			{
				return &Handle;
			}
		}

		return nullptr;
	}

	FNiagaraEmitterHandle* FindHandleById(UNiagaraSystem& InSystem, const FGuid& InId)
	{
		for (FNiagaraEmitterHandle& Handle : InSystem.GetEmitterHandles())
		{
			if (Handle.IsValid() && Handle.GetId() == InId)
			{
				return &Handle;
			}
		}

		return nullptr;
	}

	// ------------------------------------------------------------------
	// 用户参数：JSON ↔ 系统层存储
	//
	// 全部走 `FNiagaraUserRedirectionParameterStore` 的底层 API，**不**经编辑器的
	// `FNiagaraSystemViewModel`：后者需要编辑器上下文，无头 commandlet 下会撞
	// NiagaraMessageManager 的断言（`MessageAssetKey != FGuid()`，2026-09-15 实测崩溃）。
	// ------------------------------------------------------------------

	/**
	 * 配置里的用户参数用 Niagara 的**语义名**（`User.X`），而 `FNiagaraUserRedirectionParameterStore`
	 * 内部存的是**重定向后的短名**（`X`）——两侧各转一次，名字在同一套规则下对齐。
	 */
	FString ToStoreName(const FString& InConfigName)
	{
		FString Name = InConfigName;
		Name.RemoveFromStart(TEXT("User."));
		return Name;
	}

	FString ToConfigName(const FString& InStoreName)
	{
		return InStoreName.StartsWith(TEXT("User."))
			? InStoreName
			: FString::Printf(TEXT("User.%s"), *InStoreName);
	}

	bool FindUserVariable(const UNiagaraSystem& InSystem, const FString& InName, FNiagaraVariable& OutVariable)
	{
		TArray<FNiagaraVariable> UserParameters;
		InSystem.GetExposedParameters().GetUserParameters(UserParameters);

		const FName StoreName(*ToStoreName(InName));
		for (const FNiagaraVariable& Variable : UserParameters)
		{
			if (Variable.GetName() == StoreName)
			{
				OutVariable = Variable;
				return true;
			}
		}

		return false;
	}

	/** 系统里还没有这个参数时，按 JSON 值的形态推断类型——数字一律 float。 */
	/**
	 * 把模板脚本里的**可写参数**（RapidIterationParameters，即模块输入的固化值）列进日志。
	 *
	 * 配置作者需要知道"这个发射器能调什么"——用户参数（`User.*`）要模板引用才有效，
	 * 而模块输入是**总能调**的那一层。名字形如 `Constants.<发射器名>.<模块>.<输入>`。
	 *
	 * **必须传复制品**：发射器改名时引擎把参数名的发射器段一并换了，模板与复制品的名字不同，
	 * 写模板的名字会落在不驱动任何模块的孤参数上。
	 */
	void LogEmitterParameters(UNiagaraEmitter& InEmitter, const FString& InEmitterName)
	{
		const FVersionedNiagaraEmitterData* EmitterData = InEmitter.GetLatestEmitterData();
		if (EmitterData == nullptr)
		{
			return;
		}

		const TPair<const TCHAR*, const UNiagaraScript*> Scripts[] = {
			{ TEXT("EmitterSpawn"), EmitterData->EmitterSpawnScriptProps.Script },
			{ TEXT("EmitterUpdate"), EmitterData->EmitterUpdateScriptProps.Script },
			{ TEXT("Spawn"), EmitterData->SpawnScriptProps.Script },
			{ TEXT("Update"), EmitterData->UpdateScriptProps.Script },
		};

		for (const TPair<const TCHAR*, const UNiagaraScript*>& ScriptEntry : Scripts)
		{
			if (ScriptEntry.Value == nullptr)
			{
				continue;
			}

			TArray<FNiagaraVariable> Parameters;
			ScriptEntry.Value->RapidIterationParameters.GetParameters(Parameters);
			for (const FNiagaraVariable& Parameter : Parameters)
			{
				UE_LOG(LogAssetForge, Display, TEXT("[可写参数] 发射器=%s | 脚本=%s | %s | %s"),
					*InEmitterName,
					ScriptEntry.Key,
					*Parameter.GetName().ToString(),
					*Parameter.GetType().GetName());
			}
		}
	}

	const FNiagaraTypeDefinition* InferTypeFromJson(const TSharedPtr<FJsonValue>& InValue)
	{
		if (!InValue.IsValid())
		{
			return nullptr;
		}

		switch (InValue->Type)
		{
		case EJson::Boolean:
			return &FNiagaraTypeDefinition::GetBoolDef();
		case EJson::Number:
			return &FNiagaraTypeDefinition::GetFloatDef();
		case EJson::Array:
		{
			switch (InValue->AsArray().Num())
			{
			case 2: return &FNiagaraTypeDefinition::GetVec2Def();
			case 3: return &FNiagaraTypeDefinition::GetVec3Def();
			case 4: return &FNiagaraTypeDefinition::GetVec4Def();
			default: return nullptr;
			}
		}
		case EJson::Object:
			return &FNiagaraTypeDefinition::GetColorDef();
		default:
			return nullptr;
		}
	}

	bool TryGetVectorItems(const TSharedPtr<FJsonValue>& InValue, TArray<TSharedPtr<FJsonValue>>& OutItems)
	{
		if (!InValue.IsValid() || InValue->Type != EJson::Array)
		{
			return false;
		}

		OutItems = InValue->AsArray();
		return true;
	}

	/** 在发射器的四个脚本里按名找可写参数（RapidIterationParameters）；找到返回其所在脚本。 */
	UNiagaraScript* FindEmitterParameter(UNiagaraEmitter& InEmitter, const FString& InName, FNiagaraVariable& OutVariable)
	{
		FVersionedNiagaraEmitterData* EmitterData = InEmitter.GetLatestEmitterData();
		if (EmitterData == nullptr)
		{
			return nullptr;
		}

		UNiagaraScript* Scripts[] = {
			EmitterData->EmitterSpawnScriptProps.Script,
			EmitterData->EmitterUpdateScriptProps.Script,
			EmitterData->SpawnScriptProps.Script,
			EmitterData->UpdateScriptProps.Script,
		};

		const FName ParameterName(*InName);
		for (UNiagaraScript* Script : Scripts)
		{
			if (Script == nullptr)
			{
				continue;
			}

			TArray<FNiagaraVariable> Parameters;
			Script->RapidIterationParameters.GetParameters(Parameters);
			for (const FNiagaraVariable& Parameter : Parameters)
			{
				if (Parameter.GetName() == ParameterName)
				{
					OutVariable = Parameter;
					return Script;
				}
			}
		}

		return nullptr;
	}

	/** 四个脚本槽位（顺序固定：0=EmitterSpawn, 1=EmitterUpdate, 2=Spawn, 3=Update）。 */
	void GetEmitterSlotScripts(UNiagaraEmitter& InEmitter, UNiagaraScript* OutScripts[4])
	{
		FVersionedNiagaraEmitterData* EmitterData = InEmitter.GetLatestEmitterData();
		OutScripts[0] = EmitterData != nullptr ? EmitterData->EmitterSpawnScriptProps.Script : nullptr;
		OutScripts[1] = EmitterData != nullptr ? EmitterData->EmitterUpdateScriptProps.Script : nullptr;
		OutScripts[2] = EmitterData != nullptr ? EmitterData->SpawnScriptProps.Script : nullptr;
		OutScripts[3] = EmitterData != nullptr ? EmitterData->UpdateScriptProps.Script : nullptr;
	}

	/** 去掉参数名的第二段（发射器段）：`Constants.Emitter.X.Y` → `Constants.X.Y`。
	 *  引擎在复制发射器时按新发射器名重建了参数名（模板 `Constants.Emitter.X` → 复制品
	 *  `Constants.<发射器名>.X`），两侧的同一个参数只差这一段——对照它们靠这把刀。 */
	FString StripEmitterSegment(const FString& InName)
	{
		const int32 FirstDot = InName.Find(TEXT("."), ESearchCase::CaseSensitive);
		if (FirstDot == INDEX_NONE)
		{
			return InName;
		}
		const int32 SecondDot = InName.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstDot + 1);
		if (SecondDot == INDEX_NONE)
		{
			return InName;
		}
		return InName.Left(FirstDot) + InName.Mid(SecondDot);
	}

	/**
	 * 在单个脚本的 rapid iteration 参数里找与配置名对应的那个。
	 *
	 * `bAllowExactMatch` 分的是两侧判据：**模板**侧必须是"名字不等且去发射器段相等"——全等
	 * 说明配置写的是模板自己的名字（`Constants.Emitter.*`），写下去只会造一个不驱动任何模块
	 * 的孤参数；**复制品**侧找的正是同名参数（配置名就是复制品的真实名）。
	 *
	 * 复制品侧允许全等是 2026-09-19 的修正：同一个参数确实会同时挂在两个脚本槽里（实测
	 * `Constants.<发射器>.GravityForce.Gravity` 在复制品上出现两次），拿模板侧的"名字不等"
	 * 判据去遍历复制品，会让每个槽都匹配失败——一个参数也写不进去，还静默无声。
	 */
	bool FindParameterInScript(
		UNiagaraScript& InScript,
		const FString& InName,
		bool bAllowExactMatch,
		FNiagaraVariable& OutVariable)
	{
		const FName ParameterName(*InName);
		const FString StrippedName = StripEmitterSegment(InName);

		TArray<FNiagaraVariable> Parameters;
		InScript.RapidIterationParameters.GetParameters(Parameters);

		if (bAllowExactMatch)
		{
			for (const FNiagaraVariable& Parameter : Parameters)
			{
				if (Parameter.GetName() == ParameterName)
				{
					OutVariable = Parameter;
					return true;
				}
			}
		}

		for (const FNiagaraVariable& Parameter : Parameters)
		{
			if (Parameter.GetName() != ParameterName
				&& StripEmitterSegment(Parameter.GetName().ToString()) == StrippedName)
			{
				OutVariable = Parameter;
				return true;
			}
		}

		return false;
	}

	/**
	 * 在模板里找与配置参数对应的那个参数；命中时给出它所在的脚本槽位。类型与槽位的权威
	 * 来源是模板。
	 *
	 * 名字按**去发射器段**对照：配置写的是复制品的名字（`Constants.<发射器名>.X`），模板里
	 * 叫 `Constants.Emitter.X`。**全等不算命中**——那说明配置写的是模板自己的名字，导入会
	 * 创建出一个不驱动任何模块的孤参数（配置看起来生效了，画面上没有），宁可在这里报错。
	 */
	bool FindTemplateParameter(UNiagaraEmitter& InTemplate, const FString& InName, FNiagaraVariable& OutVariable, int32& OutSlot)
	{
		UNiagaraScript* Scripts[4];
		GetEmitterSlotScripts(InTemplate, Scripts);

		for (int32 Slot = 0; Slot < 4; ++Slot)
		{
			if (Scripts[Slot] != nullptr
				&& FindParameterInScript(*Scripts[Slot], InName, /*bAllowExactMatch=*/false, OutVariable))
			{
				OutSlot = Slot;
				return true;
			}
		}

		return false;
	}

	/** 写入一个参数；失败给 Warning——静默失败会让人以为配置生效了。 */
	template<typename TValueType>
	bool WriteParameterValue(
		FNiagaraParameterStore& InStore,
		const TValueType& InValue,
		const FNiagaraVariable& InVariable,
		TArray<FForgeDiagnostic>& OutDiagnostics,
		const FString& InFieldPath)
	{
		if (InStore.SetParameterValue(InValue, InVariable, /*bAdd=*/true))
		{
			return true;
		}

		OutDiagnostics.Emplace(
			EForgeSeverity::Warning,
			InFieldPath,
			FString::Printf(
				TEXT("参数 `%s`（%s）写不进 RapidIterationParameters"),
				*InVariable.GetName().ToString(),
				*InVariable.GetType().GetName()));
		return false;
	}

	/**
	 * 把一条**可写参数**（模块输入的固化值）写进发射器。
	 *
	 * 类型与槽位的**权威来源是模板**（按去发射器段对照找到对应参数）；写入用**配置里的名字**
	 * ——它写的是复制品的真实参数名（模板名的发射器段在复制时被换掉了）；复制品上的参数存储
	 * 是"按需创建"的（编辑器改模块参数也走同一条路），所以按名创建（`bAdd=true`）。
	 */
	bool ApplyEmitterParameter(
		UNiagaraEmitter& InEmitter,
		UNiagaraEmitter& InTemplate,
		const FString& InName,
		const TSharedPtr<FJsonValue>& InValue,
		TArray<FForgeDiagnostic>& OutDiagnostics,
		const FString& InFieldPath)
	{
		FNiagaraVariable Variable;
		int32 Slot = INDEX_NONE;
		if (!FindTemplateParameter(InTemplate, InName, Variable, Slot))
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Error,
				InFieldPath,
				FString::Printf(
					TEXT("模板里没有与 `%s` 对应的可写参数（导入日志里的 [可写参数] 行列出了复制品的可用名字，形如 `Constants.<发射器名>.<模块>.<输入>`——不要写模板的名字）"),
					*InName));
			return false;
		}

		const FNiagaraTypeDefinition& Type = Variable.GetType();

		// 写入用配置里的名字（复制品的真实参数名）；模板只提供类型与存在性。
		const FNiagaraVariable LocalVariable(Type, FName(*InName));

		// **同名的槽可能不止一个**：`GravityForce.Gravity` 与 `Drag.Rotational Drag` 会同时出现在
		// 两个脚本里（模板、复制品都一样）——配置里的一个值代表"这个参数取这个值"，每个已有该
		// 参数的槽都要写。只写模板命中的第一个槽，会让"先到先得"的读回读到另一个槽的默认值，
		// 计划于是永远报差异、参数也不生效（2026-09-16 实测）。
		UNiagaraScript* TargetScripts[4];
		GetEmitterSlotScripts(InEmitter, TargetScripts);

		auto WriteAllSlots = [&](const auto& InTypedValue) -> bool
		{
			bool bWroteAny = false;
			for (int32 TargetSlot = 0; TargetSlot < 4; ++TargetSlot)
			{
				FNiagaraVariable Existing;
				if (TargetScripts[TargetSlot] == nullptr
					|| !FindParameterInScript(
						*TargetScripts[TargetSlot], InName, /*bAllowExactMatch=*/true, Existing))
				{
					continue;
				}

				WriteParameterValue(
					TargetScripts[TargetSlot]->RapidIterationParameters,
					InTypedValue, LocalVariable, OutDiagnostics, InFieldPath);
				bWroteAny = true;
			}

			// 模板里有、复制品却一个槽都找不到，说明两侧的参数名对不上（复制品的参数是
			// "按需创建"的）——静默返回会让配置看起来生效了，实际画面上什么都没变。
			if (!bWroteAny)
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Warning,
					InFieldPath,
					FString::Printf(
						TEXT("复制品的四个脚本槽里都没有 `%s`，本次没写入任何槽"),
						*InName));
			}

			return bWroteAny;
		};

		if (Type == FNiagaraTypeDefinition::GetFloatDef())
		{
			return WriteAllSlots(static_cast<float>(InValue->AsNumber()));
		}
		if (Type == FNiagaraTypeDefinition::GetIntDef())
		{
			return WriteAllSlots(static_cast<int32>(InValue->AsNumber()));
		}
		if (Type == FNiagaraTypeDefinition::GetBoolDef())
		{
			return WriteAllSlots(InValue->AsBool());
		}
		if (Type == FNiagaraTypeDefinition::GetVec2Def())
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			if (TryGetVectorItems(InValue, Items) && Items.Num() >= 2)
			{
				return WriteAllSlots(FVector2f(
					static_cast<float>(Items[0]->AsNumber()),
					static_cast<float>(Items[1]->AsNumber())));
			}
		}
		else if (Type == FNiagaraTypeDefinition::GetVec3Def())
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			if (TryGetVectorItems(InValue, Items) && Items.Num() >= 3)
			{
				return WriteAllSlots(FVector3f(
					static_cast<float>(Items[0]->AsNumber()),
					static_cast<float>(Items[1]->AsNumber()),
					static_cast<float>(Items[2]->AsNumber())));
			}
		}
		else if (Type == FNiagaraTypeDefinition::GetVec4Def())
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			if (TryGetVectorItems(InValue, Items) && Items.Num() >= 4)
			{
				return WriteAllSlots(FVector4f(
					static_cast<float>(Items[0]->AsNumber()),
					static_cast<float>(Items[1]->AsNumber()),
					static_cast<float>(Items[2]->AsNumber()),
					static_cast<float>(Items[3]->AsNumber())));
			}
		}
		else if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (InValue->TryGetObject(Object) && Object != nullptr)
			{
				return WriteAllSlots(FLinearColor(
					(*Object)->GetNumberField(TEXT("R")),
					(*Object)->GetNumberField(TEXT("G")),
					(*Object)->GetNumberField(TEXT("B")),
					(*Object)->GetNumberField(TEXT("A"))));
			}
		}

		OutDiagnostics.Emplace(
			EForgeSeverity::Error,
			InFieldPath,
			FString::Printf(TEXT("暂不支持的可写参数类型 `%s`（或值的形态不对）"), *Type.GetName()));
		return false;
	}

	/**
	 * 把某个发射器节点的 props 写进目标发射器（参数日志 + 逐条写入）。
	 *
	 * **新建与复用两条路径共用**：同名发射器会走"复用"——不重建、但参数照写，配置才是
	 * 参数的唯一真源（跳过写入会让"改了配置、资产却不变"）。
	 */
	void ApplyChildParameters(
		UNiagaraEmitter& InTemplate,
		UNiagaraEmitter& InTarget,
		const FForgeNode& InChild,
		FForgeReport& OutReport)
	{
		LogEmitterParameters(InTarget, InChild.Name);

		if (!InChild.Props.IsValid())
		{
			return;
		}

		for (const auto& Pair : InChild.Props->Values)
		{
			const FString ParameterName(ASSETFORGE_JSON_KEY(Pair.Key));
			ApplyEmitterParameter(
				InTarget,
				InTemplate,
				ParameterName,
				Pair.Value,
				OutReport.Diagnostics,
				FString::Printf(TEXT("root.children.%s.props.%s"), *InChild.Name, *ParameterName));
		}
	}

	/** 都定义在下面（读值一节）；这里先声明，供导出的读值与剪默认判断使用。 */
	TSharedPtr<FJsonValue> ReadParameterValue(const FNiagaraParameterStore& InStore, const FNiagaraVariable& InVariable);
	bool AreJsonValuesEqual(const TSharedPtr<FJsonValue>& InA, const TSharedPtr<FJsonValue>& InB);

	/**
	 * 读发射器的**全部**可写参数（导出方向）。
	 *
	 * 传了 `InTemplate` 时**剪掉"配置里没写且与模板对应参数值相同"**的那些：它们没有偏离模板
	 * 默认、配置里也不写——不剪会让计划永远报一遍「仅资产」，回环就没有空计划可言。配置里
	 * **显式写了**的参数即使等于默认值也保留（写了就是意图；剪掉会让它永远显示成「仅配置」）。
	 * 模板按去发射器段对照（与写入侧同一把刀）；类型面与写入侧一致（float / int / bool /
	 * vec2-4 / position / color——只读 float/int/bool 的那一版会让其余类型永远显示成「仅配置」）。
	 */
	void ExportEmitterParameters(
		UNiagaraEmitter& InEmitter,
		const UNiagaraEmitter* InTemplate,
		const TSet<FString>* InConfiguredNames,
		const TSharedPtr<FJsonObject>& OutProps)
	{
		FVersionedNiagaraEmitterData* EmitterData = InEmitter.GetLatestEmitterData();
		if (EmitterData == nullptr || !OutProps.IsValid())
		{
			return;
		}

		UNiagaraScript* Scripts[4] = {
			EmitterData->EmitterSpawnScriptProps.Script,
			EmitterData->EmitterUpdateScriptProps.Script,
			EmitterData->SpawnScriptProps.Script,
			EmitterData->UpdateScriptProps.Script,
		};

		UNiagaraScript* TemplateScripts[4] = { nullptr, nullptr, nullptr, nullptr };
		if (InTemplate != nullptr)
		{
			// `GetEmitterSlotScripts` 只读模板，签名却是非 const——就地转一次。
			GetEmitterSlotScripts(*const_cast<UNiagaraEmitter*>(InTemplate), TemplateScripts);
		}

		for (int32 Slot = 0; Slot < 4; ++Slot)
		{
			UNiagaraScript* Script = Scripts[Slot];
			if (Script == nullptr)
			{
				continue;
			}

			TArray<FNiagaraVariable> Parameters;
			Script->RapidIterationParameters.GetParameters(Parameters);
			for (const FNiagaraVariable& Parameter : Parameters)
			{
				const TSharedPtr<FJsonValue> Value = ReadParameterValue(Script->RapidIterationParameters, Parameter);
				if (!Value.IsValid())
				{
					continue;
				}

				const bool bConfigured = InConfiguredNames != nullptr && InConfiguredNames->Contains(Parameter.GetName().ToString());
				if (!bConfigured && TemplateScripts[Slot] != nullptr)
				{
					FNiagaraVariable TemplateVariable;
					int32 TemplateSlot = INDEX_NONE;
					if (FindTemplateParameter(
							*const_cast<UNiagaraEmitter*>(InTemplate),
							Parameter.GetName().ToString(),
							TemplateVariable,
							TemplateSlot)
						&& TemplateScripts[TemplateSlot] != nullptr)
					{
						const TSharedPtr<FJsonValue> TemplateValue = ReadParameterValue(
							TemplateScripts[TemplateSlot]->RapidIterationParameters,
							TemplateVariable);
						if (TemplateValue.IsValid() && AreJsonValuesEqual(Value, TemplateValue))
						{
							continue;
						}
					}
				}

				OutProps->SetField(Parameter.GetName().ToString(), Value);
			}
		}
	}

	bool ApplyUserParameter(
		UNiagaraSystem& InSystem,
		const FString& InName,
		const TSharedPtr<FJsonValue>& InValue,
		TArray<FForgeDiagnostic>& OutDiagnostics,
		const FString& InFieldPath)
	{
		if (!InValue.IsValid())
		{
			OutDiagnostics.Emplace(EForgeSeverity::Error, InFieldPath, TEXT("值为空"));
			return false;
		}

		// 类型优先取系统里**已声明**的（模板引用的参数会随发射器出现在系统层）；
		// 系统里没有的才按值的形态推断，写入时一并添加（SetParameterValue 的 bAdd）。
		FNiagaraVariable Variable;
		if (!FindUserVariable(InSystem, InName, Variable))
		{
			const FNiagaraTypeDefinition* InferredType = InferTypeFromJson(InValue);
			if (InferredType == nullptr)
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Error,
					InFieldPath,
					FString::Printf(TEXT("系统里没有参数 `%s`，也无法从值推断类型"), *InName));
				return false;
			}

			Variable = FNiagaraVariable(*InferredType, FName(*ToStoreName(InName)));
		}

		FNiagaraUserRedirectionParameterStore& Store = InSystem.GetExposedParameters();
		const FNiagaraTypeDefinition& Type = Variable.GetType();

		if (Type == FNiagaraTypeDefinition::GetFloatDef())
		{
			return WriteParameterValue(Store, static_cast<float>(InValue->AsNumber()), Variable, OutDiagnostics, InFieldPath);
		}

		if (Type == FNiagaraTypeDefinition::GetIntDef())
		{
			return WriteParameterValue(Store, static_cast<int32>(InValue->AsNumber()), Variable, OutDiagnostics, InFieldPath);
		}

		if (Type == FNiagaraTypeDefinition::GetBoolDef())
		{
			return WriteParameterValue(Store, InValue->AsBool(), Variable, OutDiagnostics, InFieldPath);
		}

		if (Type == FNiagaraTypeDefinition::GetVec2Def())
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			TryGetVectorItems(InValue, Items);
			const FVector2f Value = Items.Num() >= 2
				? FVector2f(static_cast<float>(Items[0]->AsNumber()), static_cast<float>(Items[1]->AsNumber()))
				: FVector2f(0.f, 0.f);
			return WriteParameterValue(Store, Value, Variable, OutDiagnostics, InFieldPath);
		}

		if (Type == FNiagaraTypeDefinition::GetVec3Def())
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			TryGetVectorItems(InValue, Items);
			const FVector3f Value = Items.Num() >= 3
				? FVector3f(
					static_cast<float>(Items[0]->AsNumber()),
					static_cast<float>(Items[1]->AsNumber()),
					static_cast<float>(Items[2]->AsNumber()))
				: FVector3f(0.f, 0.f, 0.f);
			return WriteParameterValue(Store, Value, Variable, OutDiagnostics, InFieldPath);
		}

		if (Type == FNiagaraTypeDefinition::GetVec4Def())
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			TryGetVectorItems(InValue, Items);
			const FVector4f Value = Items.Num() >= 4
				? FVector4f(
					static_cast<float>(Items[0]->AsNumber()),
					static_cast<float>(Items[1]->AsNumber()),
					static_cast<float>(Items[2]->AsNumber()),
					static_cast<float>(Items[3]->AsNumber()))
				: FVector4f(0.f, 0.f, 0.f, 0.f);
			return WriteParameterValue(Store, Value, Variable, OutDiagnostics, InFieldPath);
		}

		if (Type == FNiagaraTypeDefinition::GetPositionDef())
		{
			// Position 是 LWC 类型：必须走它专属的 API（模板版本会撞 `ensure(IsLWCType == false)`）。
			TArray<TSharedPtr<FJsonValue>> Items;
			TryGetVectorItems(InValue, Items);
			const FVector Value = Items.Num() >= 3
				? FVector(Items[0]->AsNumber(), Items[1]->AsNumber(), Items[2]->AsNumber())
				: FVector::ZeroVector;
			return Store.SetPositionParameterValue(Value, Variable.GetName(), /*bAdd=*/true);
		}

		if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FLinearColor Value = FLinearColor::White;
			if (InValue->TryGetObject(Object) && Object != nullptr)
			{
				Value.R = static_cast<float>((*Object)->GetNumberField(TEXT("R")));
				Value.G = static_cast<float>((*Object)->GetNumberField(TEXT("G")));
				Value.B = static_cast<float>((*Object)->GetNumberField(TEXT("B")));
				Value.A = static_cast<float>((*Object)->GetNumberField(TEXT("A")));
			}
			return WriteParameterValue(Store, Value, Variable, OutDiagnostics, InFieldPath);
		}

		OutDiagnostics.Emplace(
			EForgeSeverity::Error,
			InFieldPath,
			FString::Printf(TEXT("参数 `%s`：暂不支持的参数类型 %s"), *InName, *Type.GetName()));
		return false;
	}

	/** 从参数存储里读一个参数的当前值（导出与计划共用；用户参数与发射器参数同一套类型面）。 */
	TSharedPtr<FJsonValue> ReadParameterValue(const FNiagaraParameterStore& InStore, const FNiagaraVariable& InVariable)
	{
		const FNiagaraTypeDefinition& Type = InVariable.GetType();

		if (Type == FNiagaraTypeDefinition::GetFloatDef())
		{
			return ForgePropertyCodec::ShortenFloatValue(InStore.GetParameterValue<float>(InVariable));
		}

		if (Type == FNiagaraTypeDefinition::GetIntDef())
		{
			return MakeShared<FJsonValueNumber>(InStore.GetParameterValue<int32>(InVariable));
		}

		if (Type == FNiagaraTypeDefinition::GetBoolDef())
		{
			return MakeShared<FJsonValueBoolean>(InStore.GetParameterValue<bool>(InVariable));
		}

		if (Type == FNiagaraTypeDefinition::GetVec2Def())
		{
			const FVector2f Value = InStore.GetParameterValue<FVector2f>(InVariable);
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Add(ForgePropertyCodec::ShortenFloatValue(Value.X));
			Items.Add(ForgePropertyCodec::ShortenFloatValue(Value.Y));
			return MakeShared<FJsonValueArray>(Items);
		}

		if (Type == FNiagaraTypeDefinition::GetVec3Def())
		{
			const FVector3f Value = InStore.GetParameterValue<FVector3f>(InVariable);
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Add(ForgePropertyCodec::ShortenFloatValue(Value.X));
			Items.Add(ForgePropertyCodec::ShortenFloatValue(Value.Y));
			Items.Add(ForgePropertyCodec::ShortenFloatValue(Value.Z));
			return MakeShared<FJsonValueArray>(Items);
		}

		if (Type == FNiagaraTypeDefinition::GetVec4Def())
		{
			const FVector4f Value = InStore.GetParameterValue<FVector4f>(InVariable);
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Add(ForgePropertyCodec::ShortenFloatValue(Value.X));
			Items.Add(ForgePropertyCodec::ShortenFloatValue(Value.Y));
			Items.Add(ForgePropertyCodec::ShortenFloatValue(Value.Z));
			Items.Add(ForgePropertyCodec::ShortenFloatValue(Value.W));
			return MakeShared<FJsonValueArray>(Items);
		}

		if (Type == FNiagaraTypeDefinition::GetPositionDef())
		{
			const FVector* Value = InStore.GetPositionParameterValue(InVariable.GetName());
			if (Value == nullptr)
			{
				return nullptr;
			}
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Add(MakeShared<FJsonValueNumber>(Value->X));
			Items.Add(MakeShared<FJsonValueNumber>(Value->Y));
			Items.Add(MakeShared<FJsonValueNumber>(Value->Z));
			return MakeShared<FJsonValueArray>(Items);
		}

		if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			const FLinearColor Value = InStore.GetParameterValue<FLinearColor>(InVariable);
			const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetField(TEXT("R"), ForgePropertyCodec::ShortenFloatValue(Value.R));
			Object->SetField(TEXT("G"), ForgePropertyCodec::ShortenFloatValue(Value.G));
			Object->SetField(TEXT("B"), ForgePropertyCodec::ShortenFloatValue(Value.B));
			Object->SetField(TEXT("A"), ForgePropertyCodec::ShortenFloatValue(Value.A));
			return MakeShared<FJsonValueObject>(Object);
		}

		return nullptr;
	}

	/** 读系统层某个用户参数的当前值（导出与计划共用）。 */
	TSharedPtr<FJsonValue> ReadUserParameterValue(const UNiagaraSystem& InSystem, const FNiagaraVariable& InVariable)
	{
		return ReadParameterValue(InSystem.GetExposedParameters(), InVariable);
	}

	/** 两个 JSON 值的"内容等价"：包进单字段对象后比规范串（字段顺序与浮点写法都归一）。 */
	bool AreJsonValuesEqual(const TSharedPtr<FJsonValue>& InA, const TSharedPtr<FJsonValue>& InB)
	{
		if (!InA.IsValid() || !InB.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject> WrapperA = MakeShared<FJsonObject>();
		const TSharedPtr<FJsonObject> WrapperB = MakeShared<FJsonObject>();
		WrapperA->SetField(TEXT("v"), InA);
		WrapperB->SetField(TEXT("v"), InB);
		return ForgeJsonText::ToCanonicalString(WrapperA) == ForgeJsonText::ToCanonicalString(WrapperB);
	}

	// ------------------------------------------------------------------
	// 当前状态（计划与导出共用）
	// ------------------------------------------------------------------

	/**
	 * 从已有配置里取「发射器名 → 模板来源」与「发射器名 → 配置里写过的参数名」——
	 * Niagara 不求反血缘，来源只能靠配置对照；写过的键集供导出的剪默认使用
	 *（配置显式写了默认值也是作者的意图，剪掉会让它永远显示成「仅配置」）。
	 */
	void LoadKnownTemplates(
		const FString& InAssetPath,
		TMap<FString, FString>& OutTemplates,
		TMap<FString, TSet<FString>>& OutConfiguredProps,
		TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		const FString ConfigPath = ForgePipeline::MakeDefaultConfigPath(InAssetPath, Kind);

		TSharedPtr<FJsonObject> Json;
		FString ParseError;
		// 走通用层的 LoadObject（读文件 → **剥注释** → 解析）：配置里带注释是常态
		//（自检配置就带），用裸解析器会让带注释的配置在这里莫名「对照失败」。
		if (!ForgeJsonText::LoadObject(ConfigPath, Json, ParseError) || !Json.IsValid())
		{
			OutDiagnostics.Emplace(
				EForgeSeverity::Warning,
				TEXT("asset"),
				FString::Printf(TEXT("模板来源对照：配置读取或解析失败（%s）"), *ParseError));
			return;
		}

		const TSharedPtr<FJsonObject>* RootObject = nullptr;
		if (!Json->TryGetObjectField(TEXT("root"), RootObject) || RootObject == nullptr)
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (!(*RootObject)->TryGetArrayField(TEXT("children"), Children) || Children == nullptr)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
		{
			const TSharedPtr<FJsonObject>* Child = nullptr;
			if (!ChildValue->TryGetObject(Child) || Child == nullptr)
			{
				continue;
			}

			FString Name;
			FString ClassName;
			(*Child)->TryGetStringField(TEXT("name"), Name);
			(*Child)->TryGetStringField(TEXT("class"), ClassName);
			if (!Name.IsEmpty() && !ClassName.IsEmpty())
			{
				OutTemplates.Add(Name, ClassName);
			}

			// 配置里写过的参数名（剪默认要区分"没写"与"写了默认值"）。
			if (!Name.IsEmpty())
			{
				const TSharedPtr<FJsonObject>* ChildProps = nullptr;
				if ((*Child)->TryGetObjectField(TEXT("props"), ChildProps) && ChildProps != nullptr)
				{
					TSet<FString>& ConfiguredNames = OutConfiguredProps.FindOrAdd(Name);
					for (const TPair<FString, TSharedPtr<FJsonValue>>& PropPair : (*ChildProps)->Values)
					{
						ConfiguredNames.Add(PropPair.Key);
					}
				}
			}
		}
	}

	/** 把系统的当前状态读成计划用的节点：children = 发射器（名 + 来源对照），props = 用户参数。 */
	void ReadCurrentNode(
		const UNiagaraSystem& InSystem,
		const FString& InAssetPath,
		FForgeNode& OutRoot,
		TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		OutRoot.ClassName = SystemRootClass;
		OutRoot.Name = FPackageName::GetShortName(InAssetPath);

		TMap<FString, FString> KnownTemplates;
		TMap<FString, TSet<FString>> ConfiguredProps;
		LoadKnownTemplates(InAssetPath, KnownTemplates, ConfiguredProps, OutDiagnostics);

		for (const FNiagaraEmitterHandle& Handle : InSystem.GetEmitterHandles())
		{
			if (!Handle.IsValid())
			{
				continue;
			}

			FForgeNode Child;
			Child.Name = Handle.GetName().ToString();

			if (const FString* Known = KnownTemplates.Find(Child.Name))
			{
				Child.ClassName = *Known;
			}
			else
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Warning,
					FString::Printf(TEXT("root.children[%s].class"), *Child.Name),
					TEXT("该发射器的模板来源未知（没有配置可对照）——导出后的配置需要人工补上 class 才能重新导入"));
			}

			// 可写参数（模块输入）也读出来：回环以"内容等价"为准——值没变即空计划。
			if (UNiagaraEmitter* ExistingEmitter = Handle.GetInstance().Emitter; ExistingEmitter != nullptr)
			{
				// 模板来源取自配置（Niagara 不求反血缘）；传进去才能剪掉"与模板默认相同"的参数。
				const FString* KnownTemplatePath = KnownTemplates.Find(Child.Name);
				FString TemplateResolveError;
				UNiagaraEmitter* KnownTemplate = KnownTemplatePath != nullptr
					? ResolveTemplate(*KnownTemplatePath, TemplateResolveError)
					: nullptr;

				TSharedPtr<FJsonObject> EmitterProps = MakeShared<FJsonObject>();
				ExportEmitterParameters(*ExistingEmitter, KnownTemplate, ConfiguredProps.Find(Child.Name), EmitterProps);
				if (EmitterProps->Values.Num() > 0)
				{
					Child.Props = EmitterProps;
				}
			}

			OutRoot.Children.Add(Child);
		}

		TArray<FNiagaraVariable> UserParameters;
		InSystem.GetExposedParameters().GetUserParameters(UserParameters);

		TSharedPtr<FJsonObject> Props;
		for (const FNiagaraVariable& Variable : UserParameters)
		{
			const TSharedPtr<FJsonValue> Value = ReadUserParameterValue(InSystem, Variable);
			if (Value.IsValid())
			{
				if (!Props.IsValid())
				{
					Props = MakeShared<FJsonObject>();
				}
				Props->SetField(ToConfigName(Variable.GetName().ToString()), Value);
			}
		}

		OutRoot.Props = Props;
	}

	bool SaveAssetPackage(UObject* InAsset, FString& OutError)
	{
		UPackage* Package = InAsset->GetOutermost();
		Package->MarkPackageDirty();

		const FString PackageFileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;

		if (!UPackage::SavePackage(Package, InAsset, *PackageFileName, SaveArgs))
		{
			OutError = FString::Printf(TEXT("保存失败：%s"), *PackageFileName);
			return false;
		}

		return true;
	}
}

bool ExportAsset(const FString& InAssetPath, FForgeDocument& OutDocument, FForgeReport& OutReport)
{
	OutReport.AssetPath = InAssetPath;

	UNiagaraSystem* System = FindSystem(InAssetPath);
	if (System == nullptr)
	{
		OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("找不到 Niagara 系统：%s"), *InAssetPath));
		return false;
	}

	OutDocument.Kind = Kind;
	OutDocument.SchemaVersion = ForgeDoc::CurrentSchemaVersion;
	OutDocument.Asset = InAssetPath;
	OutDocument.Parent.Reset();

	ReadCurrentNode(*System, InAssetPath, OutDocument.Root, OutReport.Diagnostics);

	OutReport.bSucceeded = true;
	return true;
}

bool ImportDocument(FForgeDocument& InDocument, bool bDryRun, FForgeReport& OutReport)
{
	OutReport.bDryRun = bDryRun;
	OutReport.SourcePath = InDocument.SourcePath;
	OutReport.AssetPath = InDocument.Asset;

	// 契约：root 只承载「发射器清单 + 用户参数」。
	if (!InDocument.Root.ClassName.IsEmpty() && InDocument.Root.ClassName != SystemRootClass)
	{
		OutReport.AddError(
			TEXT("root.class"),
			FString::Printf(TEXT("本类型的 root.class 固定为 `%s`"), SystemRootClass));
		return false;
	}

	// 发射器清单的校验：名字非空、不重名、模板可解析。
	TSet<FName> DeclaredNames;
	for (int32 Index = 0; Index < InDocument.Root.Children.Num(); ++Index)
	{
		const FForgeNode& Child = InDocument.Root.Children[Index];
		const FString FieldPath = FString::Printf(TEXT("root.children[%d]"), Index);

		if (Child.Name.IsEmpty())
		{
			OutReport.AddError(FieldPath, TEXT("发射器必须有名字（它是导入时的匹配键）"));
			continue;
		}

		if (DeclaredNames.Contains(FName(*Child.Name)))
		{
			OutReport.AddError(FieldPath, FString::Printf(TEXT("发射器名 `%s` 重复"), *Child.Name));
			continue;
		}
		DeclaredNames.Add(FName(*Child.Name));

		// child.props 是**可写参数**（模块输入）覆盖——效果调参就落在这里
		// （导入日志里的 [可写参数] 行列出该模板的全部可用名字）。

		FString TemplateError;
		UNiagaraEmitter* Template = ResolveTemplate(Child.ClassName, TemplateError);
		if (Template == nullptr)
		{
			OutReport.AddError(FieldPath + TEXT(".class"), TemplateError);
			continue;
		}

		// 参数名在这里校验（与写入阶段同一判据）：模板是名字的权威来源，复制品上的参数存储
		// 是"按需创建"的——不查模板的话，dry-run 会给出"参数写错也没问题"的假计划。
		if (Child.Props.IsValid())
		{
			for (const auto& Pair : Child.Props->Values)
			{
				const FString ParameterName(ASSETFORGE_JSON_KEY(Pair.Key));
				FNiagaraVariable Variable;
				int32 Slot = INDEX_NONE;
				if (!FindTemplateParameter(*Template, ParameterName, Variable, Slot))
				{
					OutReport.AddError(
						FString::Printf(TEXT("%s.props.%s"), *FieldPath, *ParameterName),
						FString::Printf(
							TEXT("模板里没有可写参数 `%s`（导入日志里的 [可写参数] 行列出了全部可用名字）"),
							*ParameterName));
				}
			}
		}
	}

	if (InDocument.Root.Props.IsValid() && InDocument.Root.Props->Values.Num() > 0)
	{
		// 参数名的形态只做提示性校验：真正的类型检查在写入时按系统声明进行。
		for (const auto& Pair : InDocument.Root.Props->Values)
		{
			const FString KeyString(ASSETFORGE_JSON_KEY(Pair.Key));
			if (!KeyString.StartsWith(TEXT("User.")))
			{
				OutReport.AddError(
					TEXT("root.props.") + KeyString,
					FString::Printf(TEXT("用户参数名应以 `User.` 开头（当前是 `%s`）"), *KeyString));
			}
		}
	}

	if (OutReport.HasErrors())
	{
		return false;
	}

	UNiagaraSystem* System = FindSystem(InDocument.Asset);

	// 计划：发射器按名匹配做增删；参数按值比较。
	FForgeNode CurrentRoot;
	if (System != nullptr)
	{
		ReadCurrentNode(*System, InDocument.Asset, CurrentRoot, OutReport.Diagnostics);
	}
	else
	{
		CurrentRoot.ClassName = SystemRootClass;
		CurrentRoot.Name = FPackageName::GetShortName(InDocument.Asset);
	}

	ForgeDiff::ComputeRootPlan(InDocument.Root, CurrentRoot, OutReport.Actions);
	OutReport.CountActions();

	if (bDryRun)
	{
		OutReport.bSucceeded = !OutReport.HasErrors();
		return OutReport.bSucceeded;
	}

	if (OutReport.HasErrors())
	{
		return false;
	}

	// 1) 目标系统：不存在就建（空系统 + 默认节点），存在就复用。
	bool bCreatedAsset = false;
	if (System == nullptr)
	{
		UPackage* Package = CreatePackage(*InDocument.Asset);
		if (Package == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("无法创建包：%s"), *InDocument.Asset));
			return false;
		}

		FString PackagePath;
		FString AssetName;
		InDocument.Asset.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (AssetName.IsEmpty())
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(
				TEXT("资产路径 `%s` 不合法，应形如 /Game/GAS/Cue/NS_MuzzleFlash"), *InDocument.Asset));
			return false;
		}

		System = NewObject<UNiagaraSystem>(Package, FName(*AssetName), RF_Public | RF_Standalone);
		if (System == nullptr)
		{
			OutReport.AddError(TEXT("asset"), FString::Printf(TEXT("创建 Niagara 系统失败：%s"), *InDocument.Asset));
			return false;
		}

		UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes=*/true);
		FAssetRegistryModule::AssetCreated(System);
		Package->MarkPackageDirty();
		bCreatedAsset = true;
	}

	// 2) 发射器同步：先删掉配置里没有的（配置是唯一真源），再按模板补配置里声明的。
	{
		TArray<FGuid> HandlesToRemove;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if (Handle.IsValid() && !DeclaredNames.Contains(Handle.GetName()))
			{
				HandlesToRemove.Add(Handle.GetId());
			}
		}

		for (const FGuid& Id : HandlesToRemove)
		{
			if (FNiagaraEmitterHandle* Handle = FindHandleById(*System, Id))
			{
				System->RemoveEmitterHandle(*Handle);
			}
		}

		for (const FForgeNode& Child : InDocument.Root.Children)
		{
			const FName Name(*Child.Name);

			// 同名视为同一个发射器：**不重建**（换模板请换名字），但参数照写——配置是参数的
			// 唯一真源，跳过写入会让"改了配置、资产却不变"（也会让计划永远报同一个差异）。
			if (const FNiagaraEmitterHandle* ExistingHandle = FindHandleByName(*System, Name))
			{
				FString ExistingTemplateError;
				UNiagaraEmitter* ExistingTemplate = ResolveTemplate(Child.ClassName, ExistingTemplateError);
				if (ExistingTemplate == nullptr)
				{
					OutReport.AddError(TEXT("root.children.") + Child.Name + TEXT(".class"), ExistingTemplateError);
					continue;
				}

				if (UNiagaraEmitter* ExistingEmitter = ExistingHandle->GetInstance().Emitter)
				{
					ApplyChildParameters(*ExistingTemplate, *ExistingEmitter, Child, OutReport);
				}
				continue;
			}

			FString TemplateError;
			UNiagaraEmitter* Template = ResolveTemplate(Child.ClassName, TemplateError);
			if (Template == nullptr)
			{
				OutReport.AddError(TEXT("root.children.") + Child.Name + TEXT(".class"), TemplateError);
				continue;
			}

			// 用编辑器的 AddEmitterToSystem 做**完整复制**：运行时版 AddEmitterHandle 复制不出
			// rapid iteration 参数（实测"清单里有、复制品上写不进去"），而这个工具函数会连脚本
			// 与参数一起搬。**千万别碰 UserParameters::***——那条路走 SystemViewModel，无头模式
			// 必崩（2026-09-15 实测）；参数改写走 GetExposedParameters 底层。
			const FGuid NewEmitterId = FNiagaraEditorUtilities::AddEmitterToSystem(
				*System,
				*Template,
				Template->GetExposedVersion().VersionGuid,
				/*bCreateCopy=*/true);

			FNiagaraEmitterHandle NewHandle;
			for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				if (Handle.GetId() == NewEmitterId)
				{
					Handle.SetName(Name, *System);
					NewHandle = Handle;
					break;
				}
			}

			// 复制品实例：参数日志与参数写入都用它（模板只提供类型与槽位）。
			if (UNiagaraEmitter* AddedEmitter = NewHandle.GetInstance().Emitter)
			{
				ApplyChildParameters(*Template, *AddedEmitter, Child, OutReport);
			}

			if (!NewHandle.IsValid())
			{
				OutReport.AddError(
					TEXT("root.children.") + Child.Name,
					FString::Printf(TEXT("复制模板 `%s` 失败"), *Child.ClassName));
			}
		}
	}

	if (OutReport.HasErrors())
	{
		return false;
	}

	// 3) 用户参数（系统层默认值）。
	if (InDocument.Root.Props.IsValid())
	{
		for (const auto& Pair : InDocument.Root.Props->Values)
		{
			const FString KeyString(ASSETFORGE_JSON_KEY(Pair.Key));
			ApplyUserParameter(
				*System,
				KeyString,
				Pair.Value,
				OutReport.Diagnostics,
				TEXT("root.props.") + KeyString);
		}
	}

	if (OutReport.HasErrors())
	{
		return false;
	}

	// 3.5) 保存前的复核：把每个发射器的可写参数**读回**、与配置对照。
	//
	// 这一步是为一个踩过的坑立的岗（2026-09-16）：`GravityForce.Gravity` 与
	// `Drag.Rotational Drag` 写入返回成功、下一次运行却读到模板默认——复核把
	// 「写入根本没落进存储」与「落进去了但保存/加载丢」分开，日志直接指认是哪一边。
	for (const FForgeNode& Child : InDocument.Root.Children)
	{
		const FName EmitterName(*Child.Name);
		const FNiagaraEmitterHandle* Handle = FindHandleByName(*System, EmitterName);
		if (Handle == nullptr || !Child.Props.IsValid())
		{
			continue;
		}

		UNiagaraEmitter* Emitter = Handle->GetInstance().Emitter;
		if (Emitter == nullptr)
		{
			continue;
		}

		TSharedPtr<FJsonObject> ReadBack = MakeShared<FJsonObject>();
		ExportEmitterParameters(*Emitter, /*InTemplate=*/nullptr, /*InConfiguredNames=*/nullptr, ReadBack);

		for (const auto& Pair : Child.Props->Values)
		{
			const FString ParameterName(ASSETFORGE_JSON_KEY(Pair.Key));

			// 读回对象的键也是 FSharedString——按字符串比较找同名项。
			const TSharedPtr<FJsonValue>* Actual = nullptr;
			for (const auto& ReadBackPair : ReadBack->Values)
			{
				if (FString(ASSETFORGE_JSON_KEY(ReadBackPair.Key)) == ParameterName)
				{
					Actual = &ReadBackPair.Value;
					break;
				}
			}

			if (Actual == nullptr)
			{
				// 读不到有两种可能：完全没进存储，或进了但名字/类型对不上——把存储里
				// 「去发射器段」相同的候选一并报出来，一次定位。
				FString Candidates;
				UNiagaraScript* SlotScripts[4];
				GetEmitterSlotScripts(*Emitter, SlotScripts);
				for (int32 Slot = 0; Slot < 4; ++Slot)
				{
					TArray<FNiagaraVariable> StoredParameters;
					if (SlotScripts[Slot] != nullptr)
					{
						SlotScripts[Slot]->RapidIterationParameters.GetParameters(StoredParameters);
					}

					for (const FNiagaraVariable& Stored : StoredParameters)
					{
						if (StripEmitterSegment(Stored.GetName().ToString()) == StripEmitterSegment(ParameterName))
						{
							Candidates += FString::Printf(
								TEXT("「%s」类型=%s；"), *Stored.GetName().ToString(), *Stored.GetType().GetName());
						}
					}
				}

				OutReport.Diagnostics.Emplace(
					EForgeSeverity::Warning,
					FString::Printf(TEXT("root.children.%s.props.%s"), *Child.Name, *ParameterName),
					FString::Printf(
						TEXT("写后读回不到该参数；存储里段相同的项：%s"),
						Candidates.IsEmpty() ? TEXT("（无）") : *Candidates));
			}
			else if (!AreJsonValuesEqual(*Actual, Pair.Value))
			{
				// 值不同：把资产侧实际值、以及存储里所有「段相同」的项（名字+类型）都报出来——
				// 2026-09-16 实测 Drag/Gravity 两个模块的输入写不进去，这一行是唯一线索来源。
				FString Stored;
				UNiagaraScript* SlotScripts[4];
				GetEmitterSlotScripts(*Emitter, SlotScripts);
				for (int32 Slot = 0; Slot < 4; ++Slot)
				{
					TArray<FNiagaraVariable> StoredParameters;
					if (SlotScripts[Slot] != nullptr)
					{
						SlotScripts[Slot]->RapidIterationParameters.GetParameters(StoredParameters);
					}

					for (const FNiagaraVariable& StoredParameter : StoredParameters)
					{
						if (StripEmitterSegment(StoredParameter.GetName().ToString()) == StripEmitterSegment(ParameterName))
						{
							Stored += FString::Printf(TEXT("「%s」类型=%s；"),
								*StoredParameter.GetName().ToString(), *StoredParameter.GetType().GetName());
						}
					}
				}

				OutReport.Diagnostics.Emplace(
					EForgeSeverity::Warning,
					FString::Printf(TEXT("root.children.%s.props.%s"), *Child.Name, *ParameterName),
					FString::Printf(TEXT("写后读回值不同；存储里段相同的项：%s"), *Stored));
			}
		}
	}

	// 4) 落盘：内容等价且不是刚创建时不必写盘（幂等）。
	if (OutReport.Actions.Num() == 0 && !bCreatedAsset && !System->GetOutermost()->IsDirty())
	{
		OutReport.bSucceeded = true;
		return true;
	}

	FString SaveError;
	if (!SaveAssetPackage(System, SaveError))
	{
		OutReport.AddError(TEXT("asset"), SaveError);
		return false;
	}

	OutReport.bSucceeded = true;
	return true;
}

}
