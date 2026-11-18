// Copyright Epic Games, Inc. All Rights Reserved.

// 录制器的几件「录成块」的事（1.10.0，定稿第 9.4A、9.7、10 节），从 GameTestRecorder.cpp 拆出来，那边只留入口调用：
// - Alt+6 / Alt+7（工具栏「等到」「分支」）：下一次点到的控件录成 `wait until=cond` 或空的按状态分支（`if branch=state`）；
// - 录完是一个步骤段：录到的步骤装进 `group`（`recorded: true`）；
// - 从这里接着录：录到的段插进已有场景的某一步后面，不另存新文件。

#include "Record/GameTestRecorder.h"

#include "Core/GameTestJson.h"
#include "Observe/GameTestSelectorWriter.h"
#include "GameTest.h"
#include "GameTestSettings.h"

#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace GameTestRecordSegmentDetail
{
	/** 点到按钮里的字或图标时往外找几层，找到按钮 / 复选框就按它录：等它能点、按它分路才有意义。 */
	constexpr int32 MaxClimb = 4;

	enum class EControlKind : uint8
	{
		/** 按钮：能点 / 显示但不能点 / 其余 */
		Button,
		/** 复选框：勾上 / 其余 */
		CheckBox,
		/** 别的控件：显示 / 其余 */
		Other,
	};

	UWidget* FindControl(UWidget& InWidget, EControlKind& OutKind)
	{
		UWidget* Current = &InWidget;
		for (int32 Climb = 0; Current != nullptr && Climb <= MaxClimb; ++Climb)
		{
			if (Current->IsA<UButton>())
			{
				OutKind = EControlKind::Button;
				return Current;
			}
			if (Current->IsA<UCheckBox>())
			{
				OutKind = EControlKind::CheckBox;
				return Current;
			}
			Current = Current->GetParent();
		}
		OutKind = EControlKind::Other;
		return &InWidget;
	}

	/** 一个控件条件：{target, <字段>: <值>} */
	TSharedRef<FJsonObject> MakeCond(const FString& InTarget, const TCHAR* InField, bool bInValue)
	{
		const TSharedRef<FJsonObject> Cond = MakeShared<FJsonObject>();
		Cond->SetStringField(TEXT("target"), InTarget);
		Cond->SetBoolField(InField, bInValue);
		return Cond;
	}

	TSharedRef<FJsonValue> EmptyList()
	{
		return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
	}

	bool InsertInObject(FJsonObject& InOutObject, const FString& InId, const TSharedPtr<FJsonValue>& InSegment);

	/**
	 * 在步骤数组里找 id 为 InId 的步骤（含所有块里的子列表），把段插在它后面，结果写进 OutList。
	 * InSegment 为空时只找不插（开始录制时核对用）。数组是只读的，所以按原样复制一份再插，找到后由上一层换回去。
	 */
	bool InsertAfterId(const TArray<TSharedPtr<FJsonValue>>& InList, const FString& InId, const TSharedPtr<FJsonValue>& InSegment, TArray<TSharedPtr<FJsonValue>>& OutList)
	{
		bool bFound = false;
		for (const TSharedPtr<FJsonValue>& Item : InList)
		{
			OutList.Add(Item);
			if (bFound || !Item.IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Item->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
			{
				continue;
			}
			FString Id;
			if ((*Object)->HasField(TEXT("action")) && (*Object)->TryGetStringField(TEXT("id"), Id) && Id == InId)
			{
				if (InSegment.IsValid())
				{
					OutList.Add(InSegment);
				}
				bFound = true;
				continue;
			}
			bFound = InsertInObject(**Object, InId, InSegment);
		}
		return bFound;
	}

	/** 在对象的每个数组字段里找（块的 do / then / else、elif 的每一项、守护的 do……）；hint 是录制的线索，不是步骤，不进去找。 */
	bool InsertInObject(FJsonObject& InOutObject, const FString& InId, const TSharedPtr<FJsonValue>& InSegment)
	{
		TArray<FString> Keys;
		InOutObject.Values.GetKeys(Keys);
		for (const FString& Key : Keys)
		{
			if (Key == TEXT("hint"))
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (!InOutObject.TryGetArrayField(Key, Array) || Array == nullptr)
			{
				continue;
			}
			TArray<TSharedPtr<FJsonValue>> Next;
			if (InsertAfterId(*Array, InId, InSegment, Next))
			{
				if (InSegment.IsValid())
				{
					InOutObject.SetArrayField(Key, Next);
				}
				return true;
			}
		}
		return false;
	}

	/** 接着录的场景：相对场景根的路径 → 绝对路径；跑出场景根（../）的不认。 */
	bool ResolveScenario(const FString& InRelative, FString& OutAbsolute, FString& OutError)
	{
		FString Root = FPaths::ConvertRelativePathToFull(UGameTestSettings::ResolveScenarioRootDir());
		OutAbsolute = FPaths::ConvertRelativePathToFull(FPaths::Combine(Root, InRelative));
		if (!Root.EndsWith(TEXT("/")))
		{
			Root.AppendChar(TEXT('/'));
		}
		FString Relative = OutAbsolute;
		if (!FPaths::MakePathRelativeTo(Relative, *Root) || Relative.StartsWith(TEXT("..")))
		{
			OutError = FString::Printf(TEXT("接着录的场景要在场景根里：%s"), *InRelative);
			return false;
		}
		if (!IFileManager::Get().FileExists(*OutAbsolute))
		{
			OutError = FString::Printf(TEXT("接着录的场景不存在：%s（先在编辑页存盘，或刷新场景列表）"), *InRelative);
			return false;
		}
		return true;
	}
}

void FGameTestRecorder::AddPickBlock(EPick InPick, UWidget& InWidget, double InTime)
{
	using namespace GameTestRecordSegmentDetail;

	EControlKind Kind = EControlKind::Other;
	UWidget* Control = FindControl(InWidget, Kind);
	FGameTestWrittenTarget Written;
	FString Error;
	if (Control == nullptr || !GameTestSelectorWriter::Write(*Control, Written, Error))
	{
		// 外层的按钮写不出选择器时退回点到的那个控件，按普通控件录
		Kind = EControlKind::Other;
		Error.Reset();
		if (!GameTestSelectorWriter::Write(InWidget, Written, Error))
		{
			UE_LOG(LogGameTest, Warning, TEXT("录制：拾取的控件写不出选择器（%s）"), *Error);
			SetNotice(TEXT("这个控件写不出选择器，没录"));
			return;
		}
	}

	AddGapWait(InTime, /*bInUiStep=*/true);
	const TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
	if (InPick == EPick::Wait)
	{
		// 等到：按钮等它能点，别的等它出现（勾选这种会变来变去的状态不适合当「等到」，也按出现）
		Step->SetStringField(TEXT("action"), TEXT("wait"));
		Step->SetStringField(TEXT("until"), TEXT("cond"));
		Step->SetObjectField(TEXT("cond"), Kind == EControlKind::Button ? MakeCond(Written.Target, TEXT("enabled"), true) : MakeCond(Written.Target, TEXT("visible"), true));
		Step->SetStringField(TEXT("note"), Kind == EControlKind::Button ? TEXT("录制时 Alt+6：等到这个按钮能点") : TEXT("录制时 Alt+6：等到这个控件出现"));
	}
	else
	{
		// 空的按状态分支（定稿 9.4A）：分法和面板「一键分路」的一样，各条路的步骤在面板里往里拖
		Step->SetStringField(TEXT("action"), TEXT("if"));
		Step->SetStringField(TEXT("branch"), TEXT("state"));
		if (Kind == EControlKind::Button)
		{
			Step->SetObjectField(TEXT("cond"), MakeCond(Written.Target, TEXT("enabled"), true));
			TArray<TSharedPtr<FJsonValue>> Both;
			Both.Add(MakeShared<FJsonValueObject>(MakeCond(Written.Target, TEXT("visible"), true)));
			Both.Add(MakeShared<FJsonValueObject>(MakeCond(Written.Target, TEXT("enabled"), false)));
			const TSharedRef<FJsonObject> ShownCond = MakeShared<FJsonObject>();
			ShownCond->SetArrayField(TEXT("all"), Both);
			const TSharedRef<FJsonObject> Shown = MakeShared<FJsonObject>();
			Shown->SetObjectField(TEXT("cond"), ShownCond);
			Shown->SetField(TEXT("then"), EmptyList());
			TArray<TSharedPtr<FJsonValue>> Elif;
			Elif.Add(MakeShared<FJsonValueObject>(Shown));
			Step->SetArrayField(TEXT("elif"), Elif);
		}
		else if (Kind == EControlKind::CheckBox)
		{
			Step->SetObjectField(TEXT("cond"), MakeCond(Written.Target, TEXT("checked"), true));
		}
		else
		{
			Step->SetObjectField(TEXT("cond"), MakeCond(Written.Target, TEXT("visible"), true));
		}
		Step->SetField(TEXT("then"), EmptyList());
		Step->SetField(TEXT("else"), EmptyList());
		Step->SetStringField(TEXT("note"), TEXT("录制时 Alt+7：按这个控件的状态分路，各条路的步骤在面板里填"));
	}
	if (Written.Hint.IsValid())
	{
		Step->SetObjectField(TEXT("hint"), Written.Hint);
	}
	AddStep(Step);
	SetNotice(InPick == EPick::Wait ? TEXT("已录「等到」") : TEXT("已录空的按状态分支，到面板里填各条路"));
}

FString FGameTestRecorder::DescribePickArmed() const
{
	switch (PickArmed)
	{
	case EPick::Assert:
		return TEXT("下一次点击录成断言");
	case EPick::Wait:
		return TEXT("下一次点到的控件录成「等到」");
	case EPick::Branch:
		return TEXT("下一次点到的控件录成按状态分支");
	default:
		return FString();
	}
}

const TCHAR* FGameTestRecorder::PickName(EPick InPick)
{
	switch (InPick)
	{
	case EPick::Assert:
		return TEXT("assert");
	case EPick::Wait:
		return TEXT("wait");
	case EPick::Branch:
		return TEXT("branch");
	default:
		return TEXT("");
	}
}

TSharedRef<FJsonObject> FGameTestRecorder::MakeSegment(const TArray<TSharedPtr<FJsonValue>>& InSteps) const
{
	const TSharedRef<FJsonObject> Group = MakeShared<FJsonObject>();
	Group->SetStringField(TEXT("action"), TEXT("group"));
	Group->SetStringField(TEXT("name"), Name);
	Group->SetBoolField(TEXT("recorded"), true);
	Group->SetArrayField(TEXT("do"), InSteps);
	return Group;
}

bool FGameTestRecorder::PrepareInsert(FString& OutError)
{
	if (InsertScenario.IsEmpty())
	{
		return true;
	}
	FString Absolute;
	if (!GameTestRecordSegmentDetail::ResolveScenario(InsertScenario, Absolute, OutError))
	{
		return false;
	}
	FString ParseError;
	const TSharedPtr<FJsonObject> Json = GameTestJson::ParseObjectFromFile(Absolute, ParseError);
	if (!Json.IsValid())
	{
		OutError = FString::Printf(TEXT("接着录的场景读不了：%s（%s）"), *InsertScenario, *ParseError);
		return false;
	}
	if (!InsertAfter.IsEmpty() && !GameTestRecordSegmentDetail::InsertInObject(*Json, InsertAfter, TSharedPtr<FJsonValue>()))
	{
		OutError = FString::Printf(TEXT("场景 %s 里没有 id 为 %s 的步骤：先在编辑页存盘（存盘时才补 id），再接着录"), *InsertScenario, *InsertAfter);
		return false;
	}
	return true;
}

bool FGameTestRecorder::SaveIntoScenario(const TArray<TSharedPtr<FJsonValue>>& InSteps, FJsonObject& OutResult, FString& OutError)
{
	FString Absolute;
	if (!GameTestRecordSegmentDetail::ResolveScenario(InsertScenario, Absolute, OutError))
	{
		return false;
	}
	// 停止时重新读：录制期间别人改了场景，按改过的插，不拿开始时的旧内容覆盖
	FString ParseError;
	const TSharedPtr<FJsonObject> Json = GameTestJson::ParseObjectFromFile(Absolute, ParseError);
	if (!Json.IsValid())
	{
		OutError = FString::Printf(TEXT("接着录的场景读不了：%s（%s）"), *InsertScenario, *ParseError);
		return false;
	}

	const TSharedPtr<FJsonValue> Segment = MakeShared<FJsonValueObject>(MakeSegment(InSteps));
	bool bPlaced = false;
	if (!InsertAfter.IsEmpty())
	{
		bPlaced = GameTestRecordSegmentDetail::InsertInObject(*Json, InsertAfter, Segment);
		if (bPlaced)
		{
			OutResult.SetStringField(TEXT("after"), InsertAfter);
		}
		else
		{
			OutResult.SetStringField(TEXT("warning"), FString::Printf(TEXT("场景里找不到 id 为 %s 的步骤了（录制期间被删或改了），录到的段放在了主流程最后"), *InsertAfter));
		}
	}
	if (!bPlaced)
	{
		TArray<TSharedPtr<FJsonValue>> Main;
		const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
		if (Json->TryGetArrayField(TEXT("steps"), Existing) && Existing != nullptr)
		{
			Main = *Existing;
		}
		Main.Add(Segment);
		Json->SetArrayField(TEXT("steps"), Main);
	}
	int32 Revision = 0;
	if (GameTestJson::TryGetInt(*Json, TEXT("revision"), Revision))
	{
		Json->SetNumberField(TEXT("revision"), Revision + 1);
	}

	if (!FFileHelper::SaveStringToFile(GameTestJson::ToPrettyString(Json), *Absolute, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("场景文件写不进去：%s（目录只读或被占用？）"), *Absolute);
		return false;
	}
	OutResult.SetStringField(TEXT("path"), InsertScenario);
	OutResult.SetStringField(TEXT("absolutePath"), Absolute);
	OutResult.SetBoolField(TEXT("inserted"), true);
	LastSaved = InsertScenario;
	UE_LOG(LogGameTest, Display, TEXT("录制「%s」：%d 步作为一段插进了 %s"), *Name, InSteps.Num(), *InsertScenario);
	return true;
}
