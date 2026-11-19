// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ForgeTypes.h"

class UWidgetBlueprint;

/**
 * 适配层：UMG 动画（`UWidgetBlueprint::Animations`）↔ 配置结构。
 *
 * 动画不是控件树的子节点：它在配置里是**顶层字段** `animations`（落在文档的 Extra 里，
 * 通用层不认识它，只负责原样保留）。轨道用控件**路径**寻址（引擎自己按控件名绑定，
 * 路径在配置侧消除同名歧义）、用属性路径选通道。
 *
 * 时间：配置写**秒**，内部换算成帧。引擎建动画时不调 SetTickResolution，保持
 * `UMovieScene` 默认的 60000/1——所以「秒 × 60000」是整数，往返零漂移。
 *
 * 创建与绑定照抄引擎自己的做法（`AnimationTabSummoner` 的公开 API 序列 +
 * `FWidgetAnimationBinding` 的字段语义）；**不调** `BindPossessableObject`——
 * 它要求一个预览用的 `UUserWidget` 实例，批处理环境里构造它是条更脏的依赖。
 */
namespace UmgAnimationCodec
{
	/** 关键帧插值，对应 float 通道的三种 key。 */
	enum class EUmgInterp : uint8
	{
		Cubic,
		Linear,
		Constant,
	};

	/** 一个关键帧。Frame 是内部单位（tick）——配置的秒在解析时换算、导出时换回。 */
	struct FUmgKey
	{
		int32 Frame = 0;
		double Value = 0.0;
		EUmgInterp Interp = EUmgInterp::Cubic;
	};

	/** 一条轨道：给某个控件（或它的 slot）的某个属性打关键帧。 */
	struct FUmgTrack
	{
		/** 从根控件起的完整路径（`RootCanvas/TitleText`），逐段匹配控件名。 */
		FString Target;

		/** 支持的通道见 .cpp 里的白名单（float 属性与 `slot.Padding.*`）。 */
		FString Channel;

		TArray<FUmgKey> Keys;
	};

	/** 一条动画：名字 + 若干轨道（轨道整体重建，见 ComputePlan/Sync）。 */
	struct FUmgAnimation
	{
		FString Name;
		TArray<FUmgTrack> Tracks;
	};

	/** tick 分辨率（引擎默认，1 秒 = 60000 帧）。 */
	constexpr int32 TicksPerSecond = 60000;

	/** 秒 → 帧（四舍五入到整数 tick）。 */
	ASSETFORGE_API int32 SecondsToFrame(double InSeconds);

	/** 帧 → 秒。 */
	ASSETFORGE_API double FrameToSeconds(int32 InFrame);

	/** 支持的通道名（配置里 channel 的取值），给 schema 做枚举。 */
	ASSETFORGE_API TArray<FString> GetChannelNames();

	/**
	 * 一个关键帧 → 配置写法。导出一律写简写 `[时间, 值]`，插值不是缺省的 cubic 时写 `[时间, 值, "linear"]`；
	 * 读取（`ParseAnimations`）简写和对象写法 `{time, value, interp?}` 都认。
	 */
	ASSETFORGE_API TSharedPtr<FJsonValue> KeyToJson(const FUmgKey& InKey);

	/**
	 * 解析文档顶层 `animations` 字段（含字段级校验）。
	 * 关键帧两种写法等价：`{ "time": 0.3, "value": 0.85, "interp": "cubic" }` 或简写 `[0.3, 0.85, "cubic"]`。
	 *
	 * 校验内容：动画名非空且唯一；target 能在**配置的控件树**里解析到、且它的控件名
	 * 在整棵树里唯一（引擎按名绑定，同名控件会让运行时绑错）；channel 在白名单里；
	 * keys 非空、字段完整。任何一条失败都会给出 `animations[i].tracks[j]` 形式的字段路径。
	 */
	ASSETFORGE_API bool ParseAnimations(const FForgeDocument& InDocument, TArray<FUmgAnimation>& OutAnimations, FForgeReport& OutReport);

	/** 比较配置动画与资产现有动画，产出计划（Create / Update / Delete）。 */
	ASSETFORGE_API void ComputePlan(const TArray<FUmgAnimation>& InTarget, const TArray<FUmgAnimation>& InCurrent, TArray<FForgePlanEntry>& OutPlan);

	/** 把资产里的动画读成结构（导出与计划共用）。 */
	ASSETFORGE_API void ReadAnimations(UWidgetBlueprint& InBlueprint, TArray<FUmgAnimation>& OutAnimations, TArray<FForgeDiagnostic>& OutDiagnostics);

	/** 导出：把资产的动画写进文档的 Extra（字段 `animations`；无动画时不写）。 */
	ASSETFORGE_API void ExportToDocument(UWidgetBlueprint& InBlueprint, FForgeDocument& OutDocument, TArray<FForgeDiagnostic>& OutDiagnostics);

	/** 真跑：按名增量——命中整体重建该动画的 MovieScene 与绑定、缺的建、多的删。 */
	ASSETFORGE_API bool Sync(UWidgetBlueprint& InBlueprint, const TArray<FUmgAnimation>& InTarget, TArray<FForgeDiagnostic>& OutDiagnostics);
}
