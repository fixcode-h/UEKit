// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Input/GameTestHandlerWatch.h"
#include "Record/GameTestRecordShot.h"
#include "Record/GameTestRecordState.h"

class FJsonObject;
class FGameTestRecorderInput;
class FGameTestRecordToolbar;
struct FGameTestMoveSegment;
struct FGameTestKeySite;
class UWorld;
class SWidget;
struct FKey;

/**
 * 录制：把人在游戏窗口里的操作记成一份场景（步骤 JSON），回放就是跑这份场景。
 *
 * **怎么认出点的是哪个控件**：不看控件是什么类，看**谁接住了这次输入**。
 * - Slate 上挂一个 `IInputProcessor`（引擎公开的输入预处理扩展点）拿原始事件，只看不吃（拾取模式除外）；
 * - `GameTestHandlerWatch`（`FSlateDebugging::InputEvent`）告诉我们这次事件被哪个 Slate 控件处理了；
 * - `GameTestSlate` 把处理者反查成 UMG 控件，`GameTestSelectorWriter` 写成选择器（WBP 名、控件路径、嵌套 `>`、列表条目）。
 * 按钮、复选框、BP 或 Lua 里重写了 OnMouseButtonDown 的 WBP、UImage 绑的事件都是同一条规则；不改引擎和项目代码。
 *
 * **录成什么**：
 * - 按下松开都在 UI 控件上 → `click`（目标是选择器，附 hint）；按住挪开了 → `drag`；
 * - 滚轮被 UI 接住 → `wheel target=…`（连着滚合成一条）；
 * - 键盘焦点在输入框里打的字 → `type target=… append=true`（连续字符合成一条），回车、退格这类被控件接住的键 → `key target=…`；
 * - 没被 UI 接住（游戏画面、游戏按键）→ 照旧 `input.cursor` + `key`；
 * - 按住 Ctrl+Alt 点一个控件 → 这次点击不传给游戏，录成 `assert`（拾取断言）。
 * - UI 操作之前不录 `wait seconds`：回放的 click / type 自己会等控件可操作。游戏操作前照旧按间隔补 wait。
 *   开始录制时给 `keepPace` 则所有间隔都录。
 *
 * **录移动**（`Alt+1` / 工具栏 / `gt.record.move`）：开始到结束之间只记角色的轨迹，别的操作都不写步骤，
 *   结束时写一条 `move`（见 `GameTestRecorderMove.cpp`）；中间点了界面就自动结束、这一下点击照常录。
 * - 录制时有一条悬浮的工具栏小窗口（默认在游戏视口中下方，可拖出视口）（`GameTestRecordToolbar`），点它不录；快捷键 Alt+数字，录制器吃掉不传给游戏。
 *
 * 事件先排队、在处理器的 Tick 里统一处理：处理者是在事件路由**之后**才知道的，排队可以把两头对上。
 * 回放发出的合成输入靠 `GameTestInput::IsSendingSynthetic` 跳过，不会录进去。只在游戏线程上用。
 */
class GAMETEST_API FGameTestRecorder
{
public:
	static FGameTestRecorder& Get();

	FGameTestRecorder();
	~FGameTestRecorder();

	/**
	 * 开始录制。`InName` 是场景名（也用来起文件名）。
	 * `bInStartPie`：编辑器里 PIE 正在跑时，在开头加一条 `pie.start`（同一张地图），回放时自己把游戏拉起来。
	 * `bInKeepPace`：UI 操作之间的间隔也录成 wait（默认不录，靠回放的自动等待）。
	 */
	bool Start(const FString& InName, bool bInStartPie, bool bInKeepPace, FString& OutError);

	/** 下一次 Start 用的截图参数（`gt.record.start` 的 `shots`）；不调就按默认（开）。 */
	void SetShotOptions(const FGameTestShotOptions& InOptions)
	{
		NextShotOptions = InOptions;
	}

	/**
	 * 点到容器里的条目时怎么问（`gt.record.start` 的 `askKey` / `askTimeoutSec`，面板 GameTest 设置页）：
	 * `unsure`（默认，拿不准才停下来问）/ `always`（每次都问）/ `never`（不问，录完再定）；
	 * `InTimeoutSeconds` 停多久自动继续，0（默认）= 一直等。
	 */
	void SetKeyAskOptions(const FString& InMode, double InTimeoutSeconds);

	/** 下一次录制怎么推断跳过条件（`gt.record.start inferSkip` / `stateWindowMs`，1.6.0）。 */
	void SetStateOptions(EGameTestInferSkip InMode, int32 InWindowMs)
	{
		NextInferSkip = InMode;
		NextStateWindowMs = InWindowMs;
	}

	/**
	 * `gt.record.key`：给录下的一步定「条目怎么认」。`step` 省略 = 正在问的那一步；
	 * `cond`（`Data.ItemId=1001`）或 `by`（字段数组，按当时的值拼条件）或 `mode`（`index` / `text` / `skip`）；
	 * `rule` 为 true 时存成规则（以后同类条目都这么认）；`all` 为 true 时这次录制里同一列表（条目类 + 容器）已录的步骤也改成这样
	 * （1.5.0，结果里 `applied` 改了几步、`skipped` 改不了的步和原因）；`resume`（默认 true）答完继续游戏。
	 * 答过的认法记在这次录制里，同一列表后面再点到条目直接沿用（hint.key.by = same），不再问。
	 */
	bool AnswerKey(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);

	/** 录下的第 InStep 步（从 0 数）点到的条目站点：`gt.keys` 录制中按步取字段用。没有返回空。 */
	TSharedPtr<FGameTestKeySite> FindKeySite(int32 InStep) const;

	/** 正在暂停等人挑标识。 */
	bool IsAskingKey() const;

	/** 插一条说明（`mark`），可以顺带截一张图；之后在面板里把它改成断言。 */
	bool Mark(const FString& InText, bool bInScreenshot, FString& OutError);

	/**
	 * 停止录制。`bInSave` 时写成场景文件（场景根目录下 `recorded/<名字>.json`，重名加序号）。
	 * OutResult：`path`（相对场景根）、`absolutePath`、`steps`（步数）、`preview`（前几步）。
	 */
	bool Stop(bool bInSave, TSharedPtr<FJsonObject>& OutResult, FString& OutError);

	bool IsRecording() const
	{
		return bRecording;
	}

	/** 录制状态：是否在录、场景名、已录步数、已录秒数、最近几步、处理者从哪来。 */
	TSharedRef<FJsonObject> DescribeStatus(int32 InLastSteps) const;

	/** 模块关闭时调：还在录就丢掉，摘掉输入处理器。 */
	void Shutdown();

	/**
	 * 录制快捷操作（`gt.record.move`、Alt+数字、工具栏共用这一个入口）：
	 * `start` / `end` / `toggle`（录移动开始、结束）、`cancel`（放弃这段移动）、`here`（记下当前位置）、
	 * `near`（断言在这附近，`InRadius` 默认 200）、`pick`（下一次点击录成断言）。
	 */
	bool Quick(const FString& InPhase, double InRadius, FString& OutError);

	/** 正在录移动段。 */
	bool IsMoving() const;

	/** Alt+数字：认领了就返回 true（输入处理器吃掉这个键，不传给游戏）。 */
	bool OnHotkey(const FKey& InKey);

	/** 屏幕坐标是不是落在录制工具栏上：点工具栏不录。 */
	bool IsOnToolbar(const FVector2D& InScreenPosition) const;

	//~ 输入处理器回调（游戏线程）。返回 true 表示吃掉这个事件（只有拾取模式会吃）。
	bool OnMouseButton(const FKey& InButton, bool bInDown, const FVector2D& InScreenPosition, bool bInPickModifiers);
	void OnKey(const FKey& InKey, bool bInDown);
	void OnWheel(float InDelta, const FVector2D& InScreenPosition);
	void OnTick();

private:
	enum class ERawKind : uint8
	{
		MouseDown,
		MouseUp,
		Wheel,
		KeyDown,
		KeyUp,
		Char,
		PickDown,
	};

	/** 排队的原始事件：处理者在路由之后由 HandlerWatch 补上。 */
	struct FRawEvent
	{
		ERawKind Kind = ERawKind::MouseDown;
		/** 键名（鼠标键也是键）；存名字不存 FKey，头文件不必拉进 InputCore。 */
		FName Key;
		FVector2D Position = FVector2D::ZeroVector;
		float Delta = 0.0f;
		TCHAR Character = 0;
		double Time = 0.0;
		/** 入队时的帧号：处理者只配给同一帧入队的事件，别的窗口里没入队的事件不会把处理者错配过来。 */
		uint64 Frame = 0;
		TWeakPtr<SWidget> Handler;
		bool bHasHandler = false;
		/** 命中路径最深的控件：没有 Slate 调试时拿它当处理者的退路，拾取模式也用它。 */
		TWeakPtr<SWidget> HitLeaf;
	};

	/** 按在 UI 上还没松开的那一下：松开时决定是 click 还是 drag。 */
	struct FUiPress
	{
		FName Button;
		FVector2D StartPosition = FVector2D::ZeroVector;
		FString Target;
		TSharedPtr<FJsonObject> Hint;
		FVector2D At = FVector2D(0.5, 0.5);
		/** 点在容器条目上：按下那一刻挑的标识和候选（松开时复查）。 */
		TSharedPtr<FGameTestKeySite> KeySite;
	};

	/** 录下的一步和它点到的条目站点（`gt.keys`、改答案用）。 */
	struct FKeyStep
	{
		TWeakPtr<FJsonObject> Step;
		TSharedPtr<FGameTestKeySite> Site;
	};

	/**
	 * 这次录制里对一个列表（条目类 + 容器）已经定过的认法（1.5.0）：同一列表后面再点到条目直接沿用，不再问；
	 * 改某一步时勾「同一列表的都改」也按它改。只在这次录制里有效，跨录制的靠规则（itemKeys）。
	 */
	struct FKeyChoice
	{
		/** 按哪些字段认（条件左边）；空时看 Mode */
		TArray<FString> By;
		/** index（只按第几个）/ text（先按文字找） */
		FString Mode;
	};

	/** 正在暂停问的那一步。 */
	struct FKeyAsk
	{
		TWeakPtr<FJsonObject> Step;
		TSharedPtr<FGameTestKeySite> Site;
		double StartedAt = 0.0;
		/** 游戏是我们停的（原本就停着的不动它）。 */
		TWeakObjectPtr<UWorld> PausedWorld;
	};

	/** 正在往输入框里打的字：换了控件或做了别的操作时合成一条 type。 */
	struct FTyping
	{
		TWeakPtr<SWidget> Handler;
		FString Target;
		TSharedPtr<FJsonObject> Hint;
		FString Text;
	};

	/** 按下还没松开的键：按下的时刻、发给了哪个控件、是不是打字顺带的键。 */
	struct FHeldKey
	{
		double Time = 0.0;
		FString Target;
		bool bTyping = false;
		/** 在录移动段里按下的键：没写成步骤，结束移动时还按着才补一条 press。 */
		bool bMoveOnly = false;
	};

	void OnHandled(GameTestHandlerWatch::EKind InKind, const GameTestHandlerWatch::FHandled& InRecord);
	void ProcessEvent(const FRawEvent& InEvent, const FRawEvent* InNext);

	void ProcessMouseDown(const FRawEvent& InEvent);
	void ProcessMouseUp(const FRawEvent& InEvent);
	void ProcessWheel(const FRawEvent& InEvent);
	void ProcessKeyDown(const FRawEvent& InEvent, bool bInTyping);
	void ProcessKeyUp(const FRawEvent& InEvent);
	void ProcessChar(const FRawEvent& InEvent);
	void ProcessPick(const FRawEvent& InEvent);
	/** 拾取用的 Ctrl、Alt 按下时已经录成了 key：从步骤尾巴上删掉，松开也不录。 */
	void DropPickModifiers();

	/** 处理者 → 选择器。`OutIsUi` 为 false 表示是游戏画面（没被 UI 接住）。 */
	bool DescribeHandler(const FRawEvent& InEvent, bool& OutIsUi, FString& OutTarget, TSharedPtr<FJsonObject>& OutHint, TSharedPtr<SWidget>& OutSlate,
		TSharedPtr<FGameTestKeySite>* OutKeySite = nullptr) const;

	/** 两次操作之间隔得久，补一条 wait（UI 操作前不补，除非 keepPace）。 */
	void AddGapWait(double InNow, bool bInUiStep);
	void AddStep(const TSharedRef<FJsonObject>& InStep);
	void FlushTyping();

	/** 视口归一化坐标（游戏画面上的点击用）。 */
	bool ToViewportPoint(const FVector2D& InScreenPosition, FVector2D& OutPoint) const;

	void Broadcast(const TCHAR* InPhase) const;

	//~ 条目怎么认（Record/GameTestRecordKeyAsk.cpp）
	/** 点击步骤写好后：复查按下时挑的标识（值变了就换），按询问方式决定要不要停下来问。 */
	void AfterKeyStep(const TSharedRef<FJsonObject>& InStep, const TSharedPtr<FGameTestKeySite>& InSite, bool bInMayAsk);
	/** 按这次录制里定过的认法改这个站点（target 一起改）；对不上（读不到、不唯一）返回 false，站点不动。 */
	bool ApplyKeyChoice(const TSharedPtr<FJsonObject>& InStep, FGameTestKeySite& InOutSite, const FKeyChoice& InChoice, const FString& InBy, FString& OutWhy) const;
	/** 同一列表（条目类 + 容器）的键 */
	static FString KeyChoiceId(const FGameTestKeySite& InSite);
	/** 把认法套到这次录制里同一列表已录的其他步骤上，结果写进 `applied` / `skipped`。 */
	void ApplyChoiceToList(const TSharedPtr<FJsonObject>& InSkipStep, const FGameTestKeySite& InSkipSite, const FString& InChoiceId,
		const FKeyChoice& InChoice, FJsonObject& OutResult);
	void BeginAsk(const TSharedRef<FJsonObject>& InStep, const TSharedPtr<FGameTestKeySite>& InSite);
	/** 结束询问、恢复游戏；暂停的这段时间不算进录制间隔。 */
	void EndAsk();
	/** 每帧：到了自动继续的时间就继续（这一步保持「待定」）。 */
	void TickAsk();
	void DescribeAsk(FJsonObject& OutStatus) const;
	/** Alt+K：接受推荐的（有条件按它、没有保持待定）并继续。 */
	void AcceptAskedKey();
	int32 IndexOfStep(const TSharedPtr<FJsonObject>& InStep) const;

	//~ 录移动和工具栏（Record/GameTestRecorderMove.cpp）
	void TickMove();
	bool BeginMove(FString& OutError);
	bool EndMove(bool bInAuto, FString& OutError);
	void CancelMove(const FString& InWhy);
	bool AddHereStep(FString& OutError);
	bool AddNearStep(double InRadius, FString& OutError);
	/** 移动段里的事件：返回 true 表示丢掉（不写步骤）；点了界面时先自动结束移动段再返回 false。 */
	bool FilterDuringMove(const FRawEvent& InEvent);
	/** 快捷键用掉了 Alt：删掉步骤尾巴上 Alt 的按下，松开也不录。 */
	void ForgetHotkeyModifiers();
	void SetNotice(const FString& InNotice);
	void SyncToolbar();
	void HideToolbar();
	void DescribeQuick(FJsonObject& OutStatus) const;

	bool bRecording = false;
	bool bKeepPace = false;
	FString Name;
	double StartedAt = 0.0;
	double LastEventAt = 0.0;
	TArray<TSharedRef<FJsonObject>> Steps;
	bool bTruncated = false;

	TArray<FRawEvent> Queue;
	TOptional<FUiPress> UiPress;
	TOptional<FTyping> Typing;
	bool bMouseDownOnGame = false;
	bool bPickActive = false;
	TMap<FName, FHeldKey> HeldKeys;

	TUniquePtr<FGameTestMoveSegment> MoveSegment;
	TSharedPtr<FGameTestRecordToolbar> Toolbar;
	/** 运行设置 `move.hotkeys`：项目自己用 Alt+数字时关掉。 */
	bool bHotkeys = true;
	/** Alt+4 / 工具栏「断言」：下一次点击录成断言（和 Ctrl+Alt 点一样）。 */
	bool bPickArmed = false;
	/** 按下落在工具栏上（点按钮、拖动位置）：配对的松开不管落在哪都不录。 */
	bool bToolbarPressed = false;
	/** 工具栏和面板上显示的一句提示（「点了界面，移动已自动结束」）。 */
	FString Notice;
	double NoticeAt = 0.0;

	FDelegateHandle HandledHandle;
	TSharedPtr<FGameTestRecorderInput> InputProcessor;

	/** 录制截图（GameTestRecordShot）：点击时截控件、界面切换时存整屏缩略图、补 opened。 */
	FGameTestRecordShots Shots;
	FGameTestShotOptions NextShotOptions;

	FGameTestRecordState StateWatch;
	EGameTestInferSkip NextInferSkip = EGameTestInferSkip::Auto;
	int32 NextStateWindowMs = FGameTestRecordState::DefaultWindowMs;

	FString AskMode = TEXT("unsure");
	double AskTimeoutSeconds = 0.0;
	TArray<FKeyStep> KeySteps;
	TOptional<FKeyAsk> KeyAsk;
	TMap<FString, FKeyChoice> KeyChoices;

	/**
	 * 录制时把条目的全部字段存一份（1.6.0）：编辑器停了、界面关了也能在步骤详情「换一个标识」里挑。
	 * 文件在 `Saved/<输出目录>/keys/<开始录制的时间>/NNN.json`，路径写进步骤 `hint.fields`；不放场景根，免得撑大仓库。
	 * 实现在 GameTestRecordKeyCache.cpp。
	 */
	void CacheKeyFields(const TSharedRef<FJsonObject>& InStep, const FGameTestKeySite& InSite);
	FString KeyCacheDir;
	int32 KeyCacheCounter = 0;
};
