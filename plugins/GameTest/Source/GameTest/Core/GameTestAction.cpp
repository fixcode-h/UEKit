// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/GameTestAction.h"
#include "GameTestCompat.h"

#include "Core/GameTestHandlerShared.h"
#include "Core/GameTestJson.h"
#include "Core/GameTestTypes.h"
#include "Operate/GameTestSkipIf.h"

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
				TEXT("启动 PIE（默认新窗口 + 固定分辨率）；netMode=listenServer 可开监听服务器会话（players 为玩家窗口总数，至少 1），并等到全部世界跑起来；PIE 已经在跑同一张地图（单人）时沿用当前会话"),
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
			{ TEXT("click"), TEXT("via,at,button"), true, false,
				TEXT("点击控件：先等控件出现、可见、可用、位置稳定。via=code（默认，可在场景或运行配置的 clickVia 改）不动鼠标：找目标往上（不出所在 WBP）最近的按钮 / 复选框 / 菜单锚点，调它的委托（按钮 OnPressed → OnClicked → OnReleased）；目标是 WBP 时点它里面唯一可见的按钮；都没有时在控件位置发虚拟鼠标事件，不移系统光标。via=pointer 是真实鼠标：还要求点击点没被挡住，移光标按下松开，核对接住按下的是不是目标。at=[x,y] 控件内位置（默认中心），button 只对 pointer 有效"),
				&GameTestHandlers::HandleClick },
			{ TEXT("drag"), TEXT("at,toTarget,toAt,by,button"), true, false,
				TEXT("拖动：从控件内 at 处按下，分几帧移到 toTarget 控件的 toAt 处（或按视口归一化位移 by=[dx,dy]）再松开；起点同 click 一样先等可操作"),
				&GameTestHandlers::HandleDrag },
			{ TEXT("wheel"), TEXT("delta,at"), false, false,
				TEXT("滚轮：给 target 时先等控件可操作、把光标移到控件上再滚；不给时在光标当前位置滚。delta 正数向上"),
				&GameTestHandlers::HandleWheel },
			{ TEXT("type"), TEXT("text,commit,via,append"), true, false,
				TEXT("在输入框里打字：默认真实按键——把键盘焦点给控件，先全选（append=true 时不全选、接着原文字打），逐字发字符，commit=true 时再按回车；via=code 直接写文本并广播 OnTextChanged / OnTextCommitted"),
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
			{ TEXT("key"), TEXT("key,phase,holdMs"), false, false,
				TEXT("按键：键盘键或鼠标键（UE 的键名，如 W、SpaceBar、LeftMouseButton），走 Slate 输入入口，和真实按键同一条路；phase=tap（默认，按下隔一帧松开；给 holdMs 时按住这么多毫秒再松开，即长按）/ press / release（按住期间还要做别的操作时用，如按住 Alt 点按钮）；press 按住的 Alt / Ctrl / Shift 会带进之后的点击和按键事件；鼠标键在光标当前位置按（先 input.cursor）；给 target 时键盘事件发给这个控件（输入框里的回车、退格），否则发给游戏视口"),
				&GameTestHandlers::HandleKey },
			{ TEXT("actor.get"), TEXT("class,tag,client,locallyControlled"), false, false,
				TEXT("在游戏世界里按 class 或 tag 找 actor（唯一命中），回报对象路径、位置、朝向与速度；多客户端会话可用 client（\"server\" 或 0 起序号）寻址世界、locallyControlled 区分本端与远端"),
				&GameTestHandlers::HandleActorGet },
			{ TEXT("move"), TEXT("to,toActor,toTag,yaw,pitch,radius,via,fallback,teleportCommand,drive,forwardKey,path,jumps,fly,client"), false, false,
				TEXT("让本地玩家的角色走到一个点：to=[x,y,z]，或 toActor / toTag 走到某个 actor 旁边；到了把镜头转到 yaw / pitch。via=auto（默认：有导航网格就寻路，否则按录制的 path 走、离轨迹远先探路接上，没有 path 就现场探路）/ nav / path / probe / teleport；fallback=teleport 时都走不到就瞬移（默认不瞬移）；drive=input（默认，推摇杆 AddMovementInput）/ keys（按住 forwardKey 并转镜头）。不核对角色是不是在录制时的起点；结果里 used 写实际用的走法，tried 写试过哪些、为什么不行"),
				&GameTestHandlers::HandleMove },
			{ TEXT("net.compare"), TEXT("class,tag,from,to,tolerance"), false, false,
				TEXT("跨端一致性断言：同一玩家对应的 actor 在两端世界的位姿偏差不超过 tolerance（厘米）；from/to 各可给 client 与 locallyControlled（缺省 from=server、to=client0）；给 timeoutMs 则跨帧等到一致"),
				&GameTestHandlers::HandleNetCompare },
			{ TEXT("assert"), TEXT("exists,visible,enabled,text,textContains,textLike,checked,count,selected,selectedProperty,stateKey,field,image,any,equals"), true, false,
				TEXT("断言目标状态（stateKey+equals 可断言快照 state 里的任意字段；textLike 忽略数字和标点比文字；checked 勾选；field=@self/@lua/@item/@itemLua.路径 + equals 读 WBP 或 Lua 字段；image 贴图名；any 任一成立）；失败信息里带实际值，不做静默通过"),
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
		// 录制写的线索（资源路径、类、文字、位置）：只给人看和解析失败时找候选，执行时不读。
		TEXT("hint"),
		// 已达成就跳过（1.6.0）：回放到这一步时条件已经成立就不做（GameTestSkipIf）
		TEXT("skipIf"),
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

	// 已达成就跳过：条件写错和拼错字段一样在开跑前报出来
	if (InJson->HasField(TEXT("skipIf")))
	{
		FString SkipError;
		if (!GameTestSkipIf::ValidateStep(*InJson, SkipError))
		{
			OutErrors.Add(FString::Printf(TEXT("skipIf：%s"), *SkipError));
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

	for (const auto& Pair : InJson->Values)
	{
		const FString PairKey = GAMETEST_JSON_KEY(Pair.Key);
		if (!AllowedParams.Contains(PairKey))
		{
			OutErrors.Add(FString::Printf(TEXT("%s 不认识字段 '%s'（可用：%s）"),
				*ActionText, *PairKey, *FString::Join(AllowedParams, TEXT(", "))));
		}
	}

	return OutErrors.Num() == 0;
}

TSharedPtr<FJsonObject> GameTestActions::DescribeAll()
{
	const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("note"),
		TEXT("target 写法：'屏名:路径' 或 '路径'（全局搜索）或 '@句柄'；屏名可写 WBP 类名或资源路径；路径段可以是 名字 / #类名 / #类名#序号 / text=子串 / * / .. / .（屏本身）/ 列表[序号] / 列表[属性=值]；'A>B' 进入嵌套的 WBP 实例，'A>>类型[text=子串]' 找纯 Slate 控件（下拉项），'>>…' 在游戏窗口里找"));

	const TSharedPtr<FJsonObject> Common = MakeShared<FJsonObject>();
	Common->SetStringField(TEXT("action"), TEXT("必填：动作名"));
	Common->SetStringField(TEXT("target"), TEXT("选择器或 @句柄"));
	Common->SetStringField(TEXT("note"), TEXT("写进报告的一句说明"));
	Common->SetStringField(TEXT("timeoutMs"), TEXT("本步超时；缺省用设置里的 DefaultStepTimeoutMs"));
	Common->SetStringField(TEXT("onFail"), TEXT("stop（默认）或 continue"));
	Common->SetStringField(TEXT("skipIf"), TEXT("已达成就跳过：{条件, target?, also?, settleMs?, verify?, verifyTimeoutMs?}，条件同 assert；回放时成立就不做这一步（记 already，算通过），also 连带跳过后面几步，verify 真做了以后核对条件成立"));
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
