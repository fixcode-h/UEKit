// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestAction.h"

#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestTypes.h"

#include "Dom/JsonObject.h"

namespace
{
	/** 编辑距离：只用来给写错的动作名提候选，超过 3 直接不算候选。 */
	int32 EditDistance(const FString& InA, const FString& InB)
	{
		const int32 LengthA = InA.Len();
		const int32 LengthB = InB.Len();
		if (LengthA == 0 || LengthB == 0)
		{
			return FMath::Max(LengthA, LengthB);
		}

		TArray<int32> Previous;
		TArray<int32> Current;
		Previous.SetNum(LengthB + 1);
		Current.SetNum(LengthB + 1);

		for (int32 Column = 0; Column <= LengthB; ++Column)
		{
			Previous[Column] = Column;
		}

		for (int32 Row = 1; Row <= LengthA; ++Row)
		{
			Current[0] = Row;
			for (int32 Column = 1; Column <= LengthB; ++Column)
			{
				const int32 Cost = (InA[Row - 1] == InB[Column - 1]) ? 0 : 1;
				Current[Column] = FMath::Min3(
					Previous[Column] + 1,
					Current[Column - 1] + 1,
					Previous[Column - 1] + Cost);
			}
			Previous = Current;
		}

		return Previous[LengthB];
	}

	/**
	 * 逗号分隔的字段清单 → **追加**到 OutParams。
	 *
	 * 必须经过一个临时数组：`FString::ParseIntoArray` 会先把输出数组清空
	 * （实测踩过——它把已经装好的公共字段白名单一起冲掉，于是每个动作都报
	 * "不认识字段 action（可用：）"，看起来像词汇表坏了）。
	 */
	void AppendParams(const TCHAR* InParams, TArray<FString>& OutParams)
	{
		TArray<FString> Parsed;
		FString(InParams).ParseIntoArray(Parsed, TEXT(","), /*CullEmpty=*/true);
		for (FString& Param : Parsed)
		{
			Param.TrimStartAndEndInline();
		}
		OutParams.Append(Parsed);
	}
}

namespace
{
	/** 主模块自带的动作（顺序即文档顺序）。可选模块的动作由它们自己登记。 */
	TArray<FGameTestActionInfo> MakeBuiltinActions()
	{
		return {
			{ TEXT("ping"), TEXT(""), false, false,
				TEXT("通道连通性检查：回显世界、PIE 与编辑器状态，不改变任何东西"),
				&GameTestHandlers::HandlePing },
			{ TEXT("mark"), TEXT("text"), false, false,
				TEXT("只把一句说明写进报告，不做任何操作（给 AI 写自己的判断）"),
				&GameTestHandlers::HandleMark },
			{ TEXT("editor.ensure"), TEXT("assetsLoaded,noModal"), false, true,
				TEXT("编辑器就绪门禁：等资产加载完、没有模态框，之后才允许动编辑器"),
				&GameTestHandlers::HandleEditorEnsure },
			{ TEXT("pie.start"), TEXT("map,width,height,netMode,players"), false, true,
				TEXT("启动 PIE（默认新窗口 + 固定分辨率）；netMode=listenServer 可开监听服务器会话（players 为玩家窗口总数，至少 1），并等到全部世界跑起来"),
				&GameTestHandlers::HandlePieStart },
			{ TEXT("pie.stop"), TEXT(""), false, true,
				TEXT("退出 PIE。请求只是入队，用 wait until=pieStopped 等它真的结束"),
				&GameTestHandlers::HandlePieStop },
			{ TEXT("level.load"), TEXT("map"), false, true,
				TEXT("在编辑器里加载关卡（PIE 之前切换起始地图用它）"),
				&GameTestHandlers::HandleLevelLoad },
			{ TEXT("widget.add"), TEXT("class,zOrder"), false, false,
				TEXT("按类路径创建 UUserWidget 并加进视口——测试靶子不必依赖真实游戏流程"),
				&GameTestHandlers::HandleWidgetAdd },
			{ TEXT("widget.remove"), TEXT(""), true, false,
				TEXT("把目标控件从视口移除并丢弃"),
				&GameTestHandlers::HandleWidgetRemove },
			{ TEXT("find"), TEXT("into"), true, false,
				TEXT("把选择器解析成句柄（捕获此刻的控件），之后用 @句柄 引用它；into 缺省用路径末段当句柄名"),
				&GameTestHandlers::HandleFind },
			{ TEXT("snapshot"), TEXT("geometry,props"), false, false,
				TEXT("控件树快照：AI 的主要观察手段；给 target 就只取那棵子树，props=true 带出类自己的属性"),
				&GameTestHandlers::HandleSnapshot },
			{ TEXT("wait"), TEXT("until,seconds,target,actor,tag,point,radius"), false, false,
				TEXT("跨帧等待谓词成立：pieReady/pieStopped/widget/widgetGone/seconds/actorNear（actorNear 用 actor 或 tag + point + radius）"),
				&GameTestHandlers::HandleWait },
			{ TEXT("click"), TEXT(""), true, false,
				TEXT("代码级点击：按钮走 Slate ExecuteOnClick，复选走 Slate 状态回调，菜单锚走开合"),
				&GameTestHandlers::HandleClick },
			{ TEXT("type"), TEXT("text,commit"), true, false,
				TEXT("输入框写文本；commit=true 才广播 OnTextCommitted（游戏逻辑多半绑在提交上）"),
				&GameTestHandlers::HandleType },
			{ TEXT("select"), TEXT("index,property,equals,add"), true, false,
				TEXT("列表族（ListView / TileView / TreeView）选中条目：按索引、或按数据项属性匹配（属性名大小写不敏感）；TreeView 的匹配范围是根条目"),
				&GameTestHandlers::HandleSelect },
			{ TEXT("scroll"), TEXT("mode,value,index,widget"), true, false,
				TEXT("滚动：ScrollBox 用 offset/end/start/widget，列表族用 index"),
				&GameTestHandlers::HandleScroll },
			{ TEXT("set"), TEXT("value,checked,option,index,expanded,property,equals,open,key"), true, false,
				TEXT("通用赋值：滑条数值/复选/下拉选项/切换器页号/展开态（含 TreeView 条目展开）/菜单开合/按键；给了该控件不认的字段即硬错误"),
				&GameTestHandlers::HandleSet },
			{ TEXT("screenshot"), TEXT("name,annotate"), false, false,
				TEXT("截游戏窗口（含 UMG）落盘，返回相对路径与像素尺寸；annotate 画编号框并给出图例"),
				&GameTestHandlers::HandleScreenshot },
			{ TEXT("console"), TEXT("command,expectContains"), false, false,
				TEXT("执行一条控制台命令（等价于在控制台里敲它）；给 expectContains 时输出必须包含该子串，否则本步失败（用来让世界状态可被场景判定）"),
				&GameTestHandlers::HandleConsole },
			{ TEXT("input.cursor"), TEXT("x,y"), false, false,
				TEXT("把光标移到视口内归一化坐标（0–1），用于驱动直读鼠标位置的游戏逻辑（如瞄准）；移动的是真实光标"),
				&GameTestHandlers::HandleInputCursor },
			{ TEXT("key"), TEXT("key,phase"), false, false,
				TEXT("按键：键盘键或鼠标键（UE 的键名，如 W、SpaceBar、LeftMouseButton），走 Slate 输入入口，和真实按键同一条路；phase=tap（默认，按下隔一帧松开）/ press / release；鼠标键在光标当前位置按（先 input.cursor）。录制生成的场景用它回放"),
				&GameTestHandlers::HandleKey },
			{ TEXT("actor.get"), TEXT("class,tag,client,locallyControlled"), false, false,
				TEXT("在游戏世界里按 class 或 tag 找 actor（唯一命中），回报对象路径、位置、朝向与速度；多客户端会话可用 client（\"server\" 或 0 起序号）寻址世界、locallyControlled 区分本端与远端"),
				&GameTestHandlers::HandleActorGet },
			{ TEXT("net.compare"), TEXT("class,tag,from,to,tolerance"), false, false,
				TEXT("跨端一致性断言：同一玩家对应的 actor 在两端世界的位姿偏差不超过 tolerance（厘米）；from/to 各可给 client 与 locallyControlled（缺省 from=server、to=client0）；给 timeoutMs 则跨帧等到一致"),
				&GameTestHandlers::HandleNetCompare },
			{ TEXT("assert"), TEXT("exists,visible,enabled,text,textContains,count,selected,selectedProperty,stateKey,equals"), true, false,
				TEXT("断言目标状态（stateKey+equals 可断言快照 state 里的任意字段）；失败信息里带实际值，不做静默通过"),
				&GameTestHandlers::HandleAssert },
			{ TEXT("probe"), TEXT("pointX,pointY,expect"), true, false,
				TEXT("只读命中测试：这个点上最终会命中谁（返回命中路径与最顶层控件；被遮挡时失败并给出遮挡者）"),
				&GameTestHandlers::HandleProbe },
		};
	}

	/** 登记表本体：第一次用到时装入主模块的动作，之后可选模块往里加。 */
	TArray<FGameTestActionInfo>& GetRegistry()
	{
		static TArray<FGameTestActionInfo> Registry = MakeBuiltinActions();
		return Registry;
	}
}

const TArray<FGameTestActionInfo>& GameTestActions::GetAll()
{
	return GetRegistry();
}

void GameTestActions::Register(const FGameTestActionInfo& InInfo)
{
	check(InInfo.Handler != nullptr);
	TArray<FGameTestActionInfo>& Registry = GetRegistry();
	Registry.RemoveAll([&InInfo](const FGameTestActionInfo& Info) { return Info.Name == InInfo.Name; });
	Registry.Add(InInfo);
}

void GameTestActions::Unregister(FName InAction)
{
	GetRegistry().RemoveAll([InAction](const FGameTestActionInfo& Info) { return Info.Name == InAction; });
}

FGameTestHandler GameTestActions::FindHandler(FName InAction)
{
	const FGameTestActionInfo* Info = Find(InAction);
	return Info != nullptr ? Info->Handler : nullptr;
}

const TArray<FGameTestOptionalAction>& GameTestActions::GetOptionalActions()
{
	// 只是提示表：模块加载后以它登记的为准。
	static const TArray<FGameTestOptionalAction> Table = {
		{ TEXT("stack.push"), TEXT("GameTestCommonUI"), TEXT("CommonUI") },
		{ TEXT("stack.pop"), TEXT("GameTestCommonUI"), TEXT("CommonUI") },
		{ TEXT("ability.input"), TEXT("GameTestGAS"), TEXT("GameplayAbilities") },
		{ TEXT("input.start"), TEXT("GameTestEnhancedInput"), TEXT("EnhancedInput") },
		{ TEXT("input.stop"), TEXT("GameTestEnhancedInput"), TEXT("EnhancedInput") },
	};
	return Table;
}

const TArray<FString>& GameTestActions::GetCommonParams()
{
	static const TArray<FString> Common = {
		TEXT("action"),
		TEXT("target"),
		TEXT("note"),
		TEXT("timeoutMs"),
		TEXT("onFail"),
	};
	return Common;
}

const FGameTestActionInfo* GameTestActions::Find(FName InAction)
{
	for (const FGameTestActionInfo& Info : GetAll())
	{
		if (Info.Name == InAction)
		{
			return &Info;
		}
	}
	return nullptr;
}

void GameTestActions::Suggest(const FString& InWritten, TArray<FString>& OutSuggestions)
{
	const FString Written = InWritten.ToLower();
	for (const FGameTestActionInfo& Info : GetAll())
	{
		const FString Name = Info.Name.ToString().ToLower();
		const bool bContains = Name.Contains(Written) || Written.Contains(Name);
		if (bContains || EditDistance(Name, Written) <= 3)
		{
			OutSuggestions.Add(Info.Name.ToString());
		}
	}
}

bool GameTestActions::ParseStep(
	const TSharedPtr<FJsonObject>& InJson,
	int32 InIndex,
	FGameTestStep& OutStep,
	TArray<FString>& OutErrors)
{
	OutStep = FGameTestStep();
	OutStep.Index = InIndex;
	OutStep.Params = InJson;

	if (!InJson.IsValid())
	{
		OutErrors.Add(TEXT("步骤不是 JSON 对象"));
		return false;
	}

	FString ActionText;
	if (!GameTestJson::TryGetString(*InJson, TEXT("action"), ActionText) || ActionText.IsEmpty())
	{
		OutErrors.Add(TEXT("缺少必填字段 action"));
		return false;
	}

	OutStep.Action = FName(*ActionText);

	// 动作名写错是最常见的写法错误：这里直接列出候选，别让它退化成一个"未知动作"。
	const FGameTestActionInfo* Info = Find(OutStep.Action);
	if (Info == nullptr)
	{
		// 可选模块的动作：说清楚缺哪个插件，别让人以为是拼错了。
		for (const FGameTestOptionalAction& Optional : GetOptionalActions())
		{
			if (OutStep.Action == FName(Optional.Name))
			{
				OutErrors.Add(FString::Printf(
					TEXT("动作 %s 由 %s 模块提供，但它没有加载：在项目里启用 %s 插件后重启编辑器"),
					*ActionText, Optional.Module, Optional.Plugin));
				return false;
			}
		}

		TArray<FString> Suggestions;
		Suggest(ActionText, Suggestions);
		OutErrors.Add(FString::Printf(TEXT("未知动作 '%s'%s"), *ActionText,
			Suggestions.Num() > 0 ? *FString::Printf(TEXT("；是否想写：%s"), *FString::Join(Suggestions, TEXT(" / "))) : TEXT("")));
		return false;
	}

	GameTestJson::TryGetString(*InJson, TEXT("target"), OutStep.Target);
	GameTestJson::TryGetString(*InJson, TEXT("note"), OutStep.Note);
	GameTestJson::TryGetInt(*InJson, TEXT("timeoutMs"), OutStep.TimeoutMs);

	FString FailModeText;
	if (GameTestJson::TryGetString(*InJson, TEXT("onFail"), FailModeText))
	{
		if (FailModeText.Equals(TEXT("stop"), ESearchCase::IgnoreCase))
		{
			OutStep.FailMode = EGameTestFailMode::Stop;
		}
		else if (FailModeText.Equals(TEXT("continue"), ESearchCase::IgnoreCase))
		{
			OutStep.FailMode = EGameTestFailMode::Continue;
		}
		else
		{
			OutErrors.Add(FString::Printf(TEXT("onFail 只能是 stop 或 continue，收到 '%s'"), *FailModeText));
		}
	}

	if (Info->bNeedsTarget && OutStep.Target.IsEmpty())
	{
		OutErrors.Add(FString::Printf(TEXT("动作 %s 需要 target（选择器或 @句柄）"), *ActionText));
	}

	// 字段白名单：拼错的键在这里被抓住，而不是在动作里被静默忽略。
	TArray<FString> AllowedParams;
	AllowedParams.Append(GetCommonParams());
	AppendParams(Info->Params, AllowedParams);

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : InJson->Values)
	{
		if (!AllowedParams.Contains(Pair.Key))
		{
			OutErrors.Add(FString::Printf(TEXT("%s 不认识字段 '%s'（可用：%s）"),
				*ActionText, *Pair.Key, *FString::Join(AllowedParams, TEXT(", "))));
		}
	}

	return OutErrors.Num() == 0;
}

TSharedPtr<FJsonObject> GameTestActions::DescribeAll()
{
	const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("note"),
		TEXT("target 写法：'屏名:路径' 或 '路径'（全局搜索）或 '@句柄'；路径段可以是 名字 / #类名 / #类名#序号 / text=子串 / *"));

	const TSharedPtr<FJsonObject> Common = MakeShared<FJsonObject>();
	Common->SetStringField(TEXT("action"), TEXT("必填：动作名"));
	Common->SetStringField(TEXT("target"), TEXT("选择器或 @句柄"));
	Common->SetStringField(TEXT("note"), TEXT("写进报告的一句说明"));
	Common->SetStringField(TEXT("timeoutMs"), TEXT("本步超时；缺省用设置里的 DefaultStepTimeoutMs"));
	Common->SetStringField(TEXT("onFail"), TEXT("stop（默认）或 continue"));
	Root->SetObjectField(TEXT("common"), Common);

	TArray<TSharedPtr<FJsonValue>> ActionValues;
	for (const FGameTestActionInfo& Info : GetAll())
	{
		const TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("name"), Info.Name.ToString());
		Action->SetBoolField(TEXT("targetRequired"), Info.bNeedsTarget);
		Action->SetBoolField(TEXT("editorOnly"), Info.bEditorOnly);
		Action->SetStringField(TEXT("summary"), Info.Summary);
		Action->SetStringField(TEXT("module"), Info.Module);

		TArray<FString> Params;
		AppendParams(Info.Params, Params);
		TArray<TSharedPtr<FJsonValue>> ParamValues;
		ParamValues.Reserve(Params.Num());
		for (const FString& Param : Params)
		{
			ParamValues.Add(MakeShared<FJsonValueString>(Param));
		}
		Action->SetArrayField(TEXT("params"), ParamValues);

		ActionValues.Add(MakeShared<FJsonValueObject>(Action));
	}
	Root->SetArrayField(TEXT("actions"), ActionValues);

	// 没加载的可选动作也列出来：面板据此提示「启用某插件后可用」。
	TArray<TSharedPtr<FJsonValue>> Unavailable;
	for (const FGameTestOptionalAction& Optional : GetOptionalActions())
	{
		if (Find(FName(Optional.Name)) == nullptr)
		{
			const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Optional.Name);
			Item->SetStringField(TEXT("module"), Optional.Module);
			Item->SetStringField(TEXT("plugin"), Optional.Plugin);
			Unavailable.Add(MakeShared<FJsonValueObject>(Item));
		}
	}
	Root->SetArrayField(TEXT("unavailable"), Unavailable);

	return Root;
}
