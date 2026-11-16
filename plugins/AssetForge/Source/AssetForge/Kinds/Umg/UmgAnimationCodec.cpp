// Copyright Epic Games, Inc. All Rights Reserved.

#include "Kinds/Umg/UmgAnimationCodec.h"
#include "AssetForgeCompat.h"

#include "Animation/MovieSceneMarginSection.h"
#include "Animation/MovieSceneMarginTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Kinds/Umg/UmgWidgetCodec.h"
#include "WidgetBlueprint.h"

namespace UmgAnimationCodec
{
namespace
{
	/**
	 * 受支持的通道白名单。
	 *
	 * 第一版只开这些——它们都落在 `UWidget` 自己或它的 slot 上，且各对应一类
	 * 现成的 Sequencer 轨道（float / margin）。要加通道时：先确认属性在
	 * `UWidget` 或 `UPanelSlot` 上、再确认值类型有对应的 Track 类。
	 */
	struct FChannelSpec
	{
		/** 配置里写的路径（slot 通道带 `slot.` 前缀）。 */
		const TCHAR* ConfigPath;

		/** 传给 Sequencer 的属性路径（相对被绑定对象）。 */
		const TCHAR* PropertyPath;

		/** 绑定对象是控件的 Slot 还是控件自身。 */
		bool bOnSlot;
	};

	const FChannelSpec SupportedChannels[] =
	{
		{ TEXT("RenderOpacity"), TEXT("RenderOpacity"), false },
		{ TEXT("RenderTransform.Translation.X"), TEXT("RenderTransform.Translation.X"), false },
		{ TEXT("RenderTransform.Translation.Y"), TEXT("RenderTransform.Translation.Y"), false },
		{ TEXT("RenderTransform.Scale.X"), TEXT("RenderTransform.Scale.X"), false },
		{ TEXT("RenderTransform.Scale.Y"), TEXT("RenderTransform.Scale.Y"), false },
		{ TEXT("RenderTransform.Shear.X"), TEXT("RenderTransform.Shear.X"), false },
		{ TEXT("RenderTransform.Shear.Y"), TEXT("RenderTransform.Shear.Y"), false },
		{ TEXT("RenderTransform.Angle"), TEXT("RenderTransform.Angle"), false },
		{ TEXT("slot.Padding.Left"), TEXT("Padding.Left"), true },
		{ TEXT("slot.Padding.Top"), TEXT("Padding.Top"), true },
		{ TEXT("slot.Padding.Right"), TEXT("Padding.Right"), true },
		{ TEXT("slot.Padding.Bottom"), TEXT("Padding.Bottom"), true },
	};

	const FChannelSpec* FindChannelSpec(const FString& InChannel)
	{
		for (const FChannelSpec& Spec : SupportedChannels)
		{
			if (InChannel == Spec.ConfigPath)
			{
				return &Spec;
			}
		}
		return nullptr;
	}

	const FChannelSpec* FindChannelSpecByPropertyPath(const FString& InPropertyPath, bool bInOnSlot)
	{
		for (const FChannelSpec& Spec : SupportedChannels)
		{
			if (Spec.bOnSlot == bInOnSlot && InPropertyPath == Spec.PropertyPath)
			{
				return &Spec;
			}
		}
		return nullptr;
	}

	const TCHAR* InterpToString(EUmgInterp InInterp)
	{
		switch (InInterp)
		{
		case EUmgInterp::Linear:
			return TEXT("linear");
		case EUmgInterp::Constant:
			return TEXT("constant");
		default:
			return TEXT("cubic");
		}
	}

	bool TryParseInterp(const FString& InText, EUmgInterp& OutInterp)
	{
		if (InText == TEXT("cubic"))
		{
			OutInterp = EUmgInterp::Cubic;
			return true;
		}
		if (InText == TEXT("linear"))
		{
			OutInterp = EUmgInterp::Linear;
			return true;
		}
		if (InText == TEXT("constant"))
		{
			OutInterp = EUmgInterp::Constant;
			return true;
		}
		return false;
	}

	//~ 控件树（配置侧）辅助 ---------------------------------------------------

	/** 按 `Root/Child/Grand` 路径在配置树里找节点；第一段必须是根的名字。 */
	const FForgeNode* ResolveNodePath(const FForgeNode& InRoot, const FString& InPath)
	{
		TArray<FString> Segments;
		InPath.ParseIntoArray(Segments, TEXT("/"), true);
		if (Segments.Num() == 0 || Segments[0] != InRoot.Name)
		{
			return nullptr;
		}

		const FForgeNode* Current = &InRoot;
		for (int32 Index = 1; Index < Segments.Num(); ++Index)
		{
			const FForgeNode* Next = nullptr;
			for (const FForgeNode& Child : Current->Children)
			{
				if (Child.Name == Segments[Index])
				{
					Next = &Child;
					break;
				}
			}
			if (Next == nullptr)
			{
				return nullptr;
			}
			Current = Next;
		}
		return Current;
	}

	/** 统计配置树里每个控件名出现的次数（引擎按名绑定，同名会让运行时绑错）。 */
	void CountNodeNames(const FForgeNode& InNode, TMap<FString, int32>& OutCounts)
	{
		OutCounts.FindOrAdd(InNode.Name)++;
		for (const FForgeNode& Child : InNode.Children)
		{
			CountNodeNames(Child, OutCounts);
		}
	}

	//~ 控件树（资产侧）辅助 ---------------------------------------------------

	/** 从根做 DFS，按名找到第一个匹配的控件（引擎的 FindWidget 也是这个语义）。 */
	UWidget* FindWidgetByNameInTree(const UWidget& InRoot, const FString& InName)
	{
		if (InRoot.GetName() == InName)
		{
			return const_cast<UWidget*>(&InRoot);
		}
		if (const UPanelWidget* Panel = Cast<UPanelWidget>(&InRoot))
		{
			for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
			{
				if (UWidget* Child = Panel->GetChildAt(Index))
				{
					if (UWidget* Found = FindWidgetByNameInTree(*Child, InName))
					{
						return Found;
					}
				}
			}
		}
		return nullptr;
	}

	/** 按 `Root/Child/Grand` 路径解析到控件。 */
	UWidget* ResolveWidgetPath(UWidget& InRoot, const FString& InPath)
	{
		TArray<FString> Segments;
		InPath.ParseIntoArray(Segments, TEXT("/"), true);
		if (Segments.Num() == 0 || Segments[0] != InRoot.GetName())
		{
			return nullptr;
		}

		UWidget* Current = &InRoot;
		for (int32 Index = 1; Index < Segments.Num(); ++Index)
		{
			UPanelWidget* Panel = Cast<UPanelWidget>(Current);
			if (Panel == nullptr)
			{
				return nullptr;
			}

			UWidget* Next = nullptr;
			for (int32 ChildIndex = 0; ChildIndex < Panel->GetChildrenCount(); ++ChildIndex)
			{
				UWidget* Child = Panel->GetChildAt(ChildIndex);
				if (Child != nullptr && Child->GetName() == Segments[Index])
				{
					Next = Child;
					break;
				}
			}
			if (Next == nullptr)
			{
				return nullptr;
			}
			Current = Next;
		}
		return Current;
	}

	/** 构造从根到目标的路径（导出的 target 形态）。 */
	bool BuildWidgetPath(const UWidget& InRoot, const UWidget& InTarget, const FString& InPath, FString& OutPath)
	{
		if (&InRoot == &InTarget)
		{
			OutPath = InPath;
			return true;
		}
		if (const UPanelWidget* Panel = Cast<UPanelWidget>(&InRoot))
		{
			for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
			{
				const UWidget* Child = Panel->GetChildAt(Index);
				if (Child == nullptr)
				{
					continue;
				}
				if (BuildWidgetPath(*Child, InTarget, InPath + TEXT("/") + Child->GetName(), OutPath))
				{
					return true;
				}
			}
		}
		return false;
	}

	//~ 关键帧与区间 -----------------------------------------------------------

	TRange<FFrameNumber> KeysToRange(const TArray<FUmgKey>& InKeys)
	{
		int32 First = InKeys[0].Frame;
		int32 Last = InKeys[0].Frame;
		for (const FUmgKey& Key : InKeys)
		{
			First = FMath::Min(First, Key.Frame);
			Last = FMath::Max(Last, Key.Frame);
		}
		// 上界排他：单个关键帧也占一帧的区间。
		return TRange<FFrameNumber>(FFrameNumber(First), FFrameNumber(Last + 1));
	}

	void AddKey(FMovieSceneFloatChannel& InChannel, const FUmgKey& InKey)
	{
		const FFrameNumber Frame(InKey.Frame);
		const float Value = static_cast<float>(InKey.Value);
		switch (InKey.Interp)
		{
		case EUmgInterp::Linear:
			InChannel.AddLinearKey(Frame, Value);
			break;
		case EUmgInterp::Constant:
			InChannel.AddConstantKey(Frame, Value);
			break;
		default:
			InChannel.AddCubicKey(Frame, Value);
			break;
		}
	}

	/** 叶子名与完整路径的拆分交给引擎自己的 FromPath——对结构体成员的语义它最权威。 */
	void ApplyPropertyPath(UMovieScenePropertyTrack& InTrack, const FString& InPropertyPath)
	{
		const FMovieScenePropertyBinding Binding = FMovieScenePropertyBinding::FromPath(InPropertyPath);
		InTrack.SetPropertyNameAndPath(Binding.PropertyName, InPropertyPath);
	}

	/** 按属性路径取 margin 段的对应通道。 */
	FMovieSceneFloatChannel* PickMarginChannel(UMovieSceneMarginSection& InSection, const FString& InPropertyPath)
	{
		// 实测点：UMovieSceneMarginSection 的四个通道成员名（LeftCurve/TopCurve/RightCurve/BottomCurve）。
		if (InPropertyPath.EndsWith(TEXT(".Left")))
		{
			return &InSection.LeftCurve;
		}
		if (InPropertyPath.EndsWith(TEXT(".Top")))
		{
			return &InSection.TopCurve;
		}
		if (InPropertyPath.EndsWith(TEXT(".Right")))
		{
			return &InSection.RightCurve;
		}
		if (InPropertyPath.EndsWith(TEXT(".Bottom")))
		{
			return &InSection.BottomCurve;
		}
		return nullptr;
	}

	void ReadKeys(FMovieSceneFloatChannel& InChannel, TArray<FUmgKey>& OutKeys)
	{
		// 通道的数据是「时间 + 值结构体」两张平行数组（引擎自己的 TMovieSceneChannelData）。
		TMovieSceneChannelData<FMovieSceneFloatValue> Data = InChannel.GetData();
		const TArrayView<FFrameNumber> Times = Data.GetTimes();
		const TArrayView<FMovieSceneFloatValue> Values = Data.GetValues();
		const int32 Count = FMath::Min(Times.Num(), Values.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FUmgKey Key;
			Key.Frame = Times[Index].Value;
			Key.Value = Values[Index].Value;
			switch (Values[Index].InterpMode)
			{
			case RCIM_Linear:
				Key.Interp = EUmgInterp::Linear;
				break;
			case RCIM_Constant:
				Key.Interp = EUmgInterp::Constant;
				break;
			default:
				Key.Interp = EUmgInterp::Cubic;
				break;
			}
			OutKeys.Add(MoveTemp(Key));
		}
	}

	//~ JSON 序列化（导出方向） ------------------------------------------------

	TSharedRef<FJsonObject> KeyToJson(const FUmgKey& InKey)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("time"), FrameToSeconds(InKey.Frame));
		Object->SetNumberField(TEXT("value"), InKey.Value);
		// 缺省即 cubic：默认值不写，配置保持干净（与属性 delta 同一个品味）。
		if (InKey.Interp != EUmgInterp::Cubic)
		{
			Object->SetStringField(TEXT("interp"), InterpToString(InKey.Interp));
		}
		return Object;
	}

	TSharedRef<FJsonObject> TrackToJson(const FUmgTrack& InTrack)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("target"), InTrack.Target);
		Object->SetStringField(TEXT("channel"), InTrack.Channel);

		TArray<TSharedPtr<FJsonValue>> KeyValues;
		KeyValues.Reserve(InTrack.Keys.Num());
		for (const FUmgKey& Key : InTrack.Keys)
		{
			KeyValues.Add(MakeShared<FJsonValueObject>(KeyToJson(Key)));
		}
		Object->SetArrayField(TEXT("keys"), KeyValues);
		return Object;
	}

	TSharedRef<FJsonObject> AnimationToJson(const FUmgAnimation& InAnimation)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("name"), InAnimation.Name);

		TArray<TSharedPtr<FJsonValue>> TrackValues;
		TrackValues.Reserve(InAnimation.Tracks.Num());
		for (const FUmgTrack& Track : InAnimation.Tracks)
		{
			TrackValues.Add(MakeShared<FJsonValueObject>(TrackToJson(Track)));
		}
		Object->SetArrayField(TEXT("tracks"), TrackValues);
		return Object;
	}

	//~ 结构比较（计划判据） ---------------------------------------------------

	bool AreKeysEqual(const FUmgKey& InLeft, const FUmgKey& InRight)
	{
		// 帧是整数，直接比。值按 float 比：通道是 FMovieSceneFloatChannel，配置写的
		// double 与资产读回的 float 之间必然差一个 float 级误差——按 double 比会让
		// 「配置 0.9 / 资产 0.8999999761581421」永远判为不等，计划永不收敛。
		return InLeft.Frame == InRight.Frame
			&& FMath::IsNearlyEqual(static_cast<float>(InLeft.Value), static_cast<float>(InRight.Value))
			&& InLeft.Interp == InRight.Interp;
	}

	bool AreTracksEqual(const FUmgTrack& InLeft, const FUmgTrack& InRight)
	{
		if (InLeft.Target != InRight.Target || InLeft.Channel != InRight.Channel || InLeft.Keys.Num() != InRight.Keys.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < InLeft.Keys.Num(); ++Index)
		{
			if (!AreKeysEqual(InLeft.Keys[Index], InRight.Keys[Index]))
			{
				return false;
			}
		}
		return true;
	}

	bool AreAnimationsEqual(const FUmgAnimation& InLeft, const FUmgAnimation& InRight)
	{
		if (InLeft.Name != InRight.Name || InLeft.Tracks.Num() != InRight.Tracks.Num())
		{
			return false;
		}

		// 轨道集合按 (target, channel) 匹配而不是按下标：资产里的轨道顺序由引擎的
		// possessable 顺序决定，与配置里写的先后无关——按下标比会让每次导入都报 Update。
		for (const FUmgTrack& LeftTrack : InLeft.Tracks)
		{
			const FUmgTrack* Matched = InRight.Tracks.FindByPredicate(
				[&LeftTrack](const FUmgTrack& Candidate)
				{
					return Candidate.Target == LeftTrack.Target && Candidate.Channel == LeftTrack.Channel;
				});
			if (Matched == nullptr || !AreTracksEqual(LeftTrack, *Matched))
			{
				return false;
			}
		}
		return true;
	}

	//~ 写入 -------------------------------------------------------------------

	/**
	 * 换一个新的 MovieScene 并清空绑定：轨道与 section 没有稳定名字，
	 * 整体重建比逐轨道增量更不易错（同 anim-blueprint 的状态机策略）。
	 */
	bool RebuildAnimation(UWidgetBlueprint& InBlueprint, UWidgetAnimation& InAnimation, const FUmgAnimation& InTarget, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		UWidgetTree* Tree = InBlueprint.WidgetTree;
		if (Tree == nullptr || Tree->RootWidget == nullptr)
		{
			OutDiagnostics.Emplace(EForgeSeverity::Error, TEXT("root"), TEXT("资产没有控件树，动画无从落地"));
			return false;
		}

		UMovieScene* Scene = NewObject<UMovieScene>(&InAnimation, NAME_None, RF_Transactional);
		// 显示帧率只影响编辑器时间轴刻度；求值基准是 tick 分辨率（保持引擎默认 60000/1）。
		Scene->SetDisplayRate(FFrameRate(20, 1));
		InAnimation.MovieScene = Scene;
		InAnimation.AnimationBindings.Reset();

		bool bOk = true;
		TOptional<int32> FirstFrame;
		TOptional<int32> LastFrame;

		for (int32 TrackIndex = 0; TrackIndex < InTarget.Tracks.Num(); ++TrackIndex)
		{
			const FUmgTrack& Track = InTarget.Tracks[TrackIndex];
			const FString TrackPath = FString::Printf(TEXT("animations/%s/tracks[%d]"), *InTarget.Name, TrackIndex);

			UWidget* Target = ResolveWidgetPath(*Tree->RootWidget, Track.Target);
			if (Target == nullptr)
			{
				OutDiagnostics.Emplace(EForgeSeverity::Error, TrackPath + TEXT(".target"), FString::Printf(TEXT("找不到控件：%s"), *Track.Target));
				bOk = false;
				continue;
			}

			const FChannelSpec* Spec = FindChannelSpec(Track.Channel);
			if (Spec == nullptr)
			{
				OutDiagnostics.Emplace(EForgeSeverity::Error, TrackPath + TEXT(".channel"), FString::Printf(TEXT("不支持的通道：%s"), *Track.Channel));
				bOk = false;
				continue;
			}

			// possessable + 绑定：字段语义照 UWidgetAnimation::BindPossessableObject
			// （唯一区别是它要预览实例，我们直接填）。
			FGuid PossessableGuid;
			if (Spec->bOnSlot)
			{
				UPanelSlot* Slot = Target->Slot;
				if (Slot == nullptr)
				{
					OutDiagnostics.Emplace(EForgeSeverity::Error, TrackPath + TEXT(".channel"), FString::Printf(TEXT("%s 没有插槽（它不在面板里？）"), *Track.Target));
					bOk = false;
					continue;
				}

				PossessableGuid = Scene->AddPossessable(Target->GetFName().ToString(), Slot->GetClass());

				FWidgetAnimationBinding Binding;
				Binding.AnimationGuid = PossessableGuid;
				Binding.WidgetName = Target->GetFName();
				Binding.SlotWidgetName = Slot->GetFName();
				Binding.bIsRootWidget = false;
				InAnimation.AnimationBindings.Add(Binding);
			}
			else
			{
				PossessableGuid = Scene->AddPossessable(Target->GetFName().ToString(), Target->GetClass());

				FWidgetAnimationBinding Binding;
				Binding.AnimationGuid = PossessableGuid;
				Binding.WidgetName = Target->GetFName();
				Binding.bIsRootWidget = (Target == Tree->RootWidget);
				InAnimation.AnimationBindings.Add(Binding);
			}

			const TRange<FFrameNumber> Range = KeysToRange(Track.Keys);
			if (Spec->bOnSlot)
			{
				UMovieSceneMarginTrack* TrackObject = Scene->AddTrack<UMovieSceneMarginTrack>(PossessableGuid);
				ApplyPropertyPath(*TrackObject, Spec->PropertyPath);
				UMovieSceneSection* Section = TrackObject->CreateNewSection();
				Section->SetRange(Range);
				TrackObject->AddSection(*Section);

				UMovieSceneMarginSection* MarginSection = Cast<UMovieSceneMarginSection>(Section);
				FMovieSceneFloatChannel* Channel = (MarginSection != nullptr) ? PickMarginChannel(*MarginSection, Spec->PropertyPath) : nullptr;
				if (Channel == nullptr)
				{
					OutDiagnostics.Emplace(EForgeSeverity::Error, TrackPath + TEXT(".channel"), TEXT("margin 通道创建失败（引擎 API 与预期不符，见实现里的实测点）"));
					bOk = false;
					continue;
				}
				for (const FUmgKey& Key : Track.Keys)
				{
					AddKey(*Channel, Key);
				}
			}
			else
			{
				UMovieSceneFloatTrack* TrackObject = Scene->AddTrack<UMovieSceneFloatTrack>(PossessableGuid);
				ApplyPropertyPath(*TrackObject, Spec->PropertyPath);

				UMovieSceneSection* Section = TrackObject->CreateNewSection();
				Section->SetRange(Range);
				TrackObject->AddSection(*Section);

				UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section);
				if (FloatSection == nullptr)
				{
					OutDiagnostics.Emplace(EForgeSeverity::Error, TrackPath + TEXT(".channel"), TEXT("float 段创建失败（引擎 API 与预期不符，见实现里的实测点）"));
					bOk = false;
					continue;
				}

				FMovieSceneFloatChannel& Channel = FloatSection->GetChannel();
				for (const FUmgKey& Key : Track.Keys)
				{
					AddKey(Channel, Key);
				}
			}

			for (const FUmgKey& Key : Track.Keys)
			{
				FirstFrame = FirstFrame.IsSet() ? FMath::Min(FirstFrame.GetValue(), Key.Frame) : Key.Frame;
				LastFrame = LastFrame.IsSet() ? FMath::Max(LastFrame.GetValue(), Key.Frame) : Key.Frame;
			}
		}

		if (FirstFrame.IsSet() && LastFrame.IsSet())
		{
			Scene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(FirstFrame.GetValue()), FFrameNumber(LastFrame.GetValue() + 1)));
		}

		return bOk;
	}

	bool DeleteAnimation(UWidgetBlueprint& InBlueprint, UWidgetAnimation& InAnimation)
	{
		const FName AnimationName = InAnimation.GetFName();
		InBlueprint.Animations.Remove(&InAnimation);
		ForgeWidgetVariableRemoved(InBlueprint, AnimationName);

		// 挪进瞬态包是引擎删动画的做法（AnimationTabSummoner.cpp:890-894）：
		// 直接删对象会让还引用它的 Sequencer 标签页悬空。
		InAnimation.Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
		return true;
	}
} // namespace

int32 SecondsToFrame(double InSeconds)
{
	return FMath::RoundToInt(InSeconds * static_cast<double>(TicksPerSecond));
}

double FrameToSeconds(int32 InFrame)
{
	return static_cast<double>(InFrame) / static_cast<double>(TicksPerSecond);
}

bool ParseAnimations(const FForgeDocument& InDocument, TArray<FUmgAnimation>& OutAnimations, FForgeReport& OutReport)
{
	OutAnimations.Reset();

	const TArray<TSharedPtr<FJsonValue>>* AnimationValues = nullptr;
	if (InDocument.Extra.IsValid())
	{
		InDocument.Extra->TryGetArrayField(TEXT("animations"), AnimationValues);
	}
	if (AnimationValues == nullptr)
	{
		// 没写 `animations`：空集——语义是「资产里不该有动画」。
		return true;
	}

	TMap<FString, int32> NameCounts;
	CountNodeNames(InDocument.Root, NameCounts);

	TSet<FString> SeenAnimationNames;
	bool bOk = true;

	for (int32 AnimationIndex = 0; AnimationIndex < AnimationValues->Num(); ++AnimationIndex)
	{
		const FString AnimationPath = FString::Printf(TEXT("animations[%d]"), AnimationIndex);

		const TSharedPtr<FJsonObject>* AnimationObject = nullptr;
		if (!(*AnimationValues)[AnimationIndex]->TryGetObject(AnimationObject) || AnimationObject == nullptr)
		{
			OutReport.AddError(AnimationPath, TEXT("每条动画必须是 JSON 对象"));
			bOk = false;
			continue;
		}

		FUmgAnimation Animation;
		if (!(*AnimationObject)->TryGetStringField(TEXT("name"), Animation.Name) || Animation.Name.IsEmpty())
		{
			OutReport.AddError(AnimationPath + TEXT(".name"), TEXT("动画名不能为空（导入与导出都按它匹配）"));
			bOk = false;
			continue;
		}

		bool bDuplicateName = false;
		SeenAnimationNames.Add(Animation.Name, &bDuplicateName);
		if (bDuplicateName)
		{
			OutReport.AddError(AnimationPath + TEXT(".name"), FString::Printf(TEXT("动画名 `%s` 重复（资产内必须唯一）"), *Animation.Name));
			bOk = false;
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* TrackValues = nullptr;
		if (!(*AnimationObject)->TryGetArrayField(TEXT("tracks"), TrackValues) || TrackValues == nullptr)
		{
			// 没有轨道等于空动画：合法（动画对象在、但什么都不动）。
			OutAnimations.Add(MoveTemp(Animation));
			continue;
		}

		for (int32 TrackIndex = 0; TrackIndex < TrackValues->Num(); ++TrackIndex)
		{
			const FString TrackPath = FString::Printf(TEXT("%s.tracks[%d]"), *AnimationPath, TrackIndex);

			const TSharedPtr<FJsonObject>* TrackObject = nullptr;
			if (!(*TrackValues)[TrackIndex]->TryGetObject(TrackObject) || TrackObject == nullptr)
			{
				OutReport.AddError(TrackPath, TEXT("每条轨道必须是 JSON 对象"));
				bOk = false;
				continue;
			}

			FUmgTrack Track;
			if (!(*TrackObject)->TryGetStringField(TEXT("target"), Track.Target) || Track.Target.IsEmpty())
			{
				OutReport.AddError(TrackPath + TEXT(".target"), TEXT("target 不能为空（从根控件起的完整路径）"));
				bOk = false;
				continue;
			}

			const FForgeNode* TargetNode = ResolveNodePath(InDocument.Root, Track.Target);
			if (TargetNode == nullptr)
			{
				OutReport.AddError(TrackPath + TEXT(".target"), FString::Printf(TEXT("配置里找不到这个控件路径：%s"), *Track.Target));
				bOk = false;
				continue;
			}

			// 引擎按控件名绑定，同名控件会让运行时绑错——这里直接拒绝，不留到运行时。
			if (NameCounts.FindRef(TargetNode->Name) > 1)
			{
				OutReport.AddError(
					TrackPath + TEXT(".target"),
					FString::Printf(TEXT("控件名 `%s` 在树里出现多次；动画按名绑定，无法区分"), *TargetNode->Name));
				bOk = false;
				continue;
			}

			if (!(*TrackObject)->TryGetStringField(TEXT("channel"), Track.Channel) || Track.Channel.IsEmpty())
			{
				OutReport.AddError(TrackPath + TEXT(".channel"), TEXT("channel 不能为空"));
				bOk = false;
				continue;
			}
			if (FindChannelSpec(Track.Channel) == nullptr)
			{
				OutReport.AddError(
					TrackPath + TEXT(".channel"),
					FString::Printf(TEXT("不支持的通道 `%s`（第一版：RenderOpacity、RenderTransform 各分量、slot.Padding 四边）"), *Track.Channel));
				bOk = false;
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* KeyValues = nullptr;
			if (!(*TrackObject)->TryGetArrayField(TEXT("keys"), KeyValues) || KeyValues == nullptr || KeyValues->Num() == 0)
			{
				OutReport.AddError(TrackPath + TEXT(".keys"), TEXT("keys 不能为空（至少一个关键帧）"));
				bOk = false;
				continue;
			}

			for (int32 KeyIndex = 0; KeyIndex < KeyValues->Num(); ++KeyIndex)
			{
				const FString KeyPath = FString::Printf(TEXT("%s.keys[%d]"), *TrackPath, KeyIndex);

				const TSharedPtr<FJsonObject>* KeyObject = nullptr;
				if (!(*KeyValues)[KeyIndex]->TryGetObject(KeyObject) || KeyObject == nullptr)
				{
					OutReport.AddError(KeyPath, TEXT("每个关键帧必须是 JSON 对象"));
					bOk = false;
					continue;
				}

				double Time = 0.0;
				double Value = 0.0;
				if (!(*KeyObject)->TryGetNumberField(TEXT("time"), Time))
				{
					OutReport.AddError(KeyPath + TEXT(".time"), TEXT("time 缺失（单位：秒）"));
					bOk = false;
					continue;
				}
				if (!(*KeyObject)->TryGetNumberField(TEXT("value"), Value))
				{
					OutReport.AddError(KeyPath + TEXT(".value"), TEXT("value 缺失"));
					bOk = false;
					continue;
				}

				FUmgKey Key;
				Key.Frame = SecondsToFrame(Time);
				Key.Value = Value;

				FString InterpText;
				if ((*KeyObject)->TryGetStringField(TEXT("interp"), InterpText) && !InterpText.IsEmpty())
				{
					if (!TryParseInterp(InterpText, Key.Interp))
					{
						OutReport.AddError(KeyPath + TEXT(".interp"), FString::Printf(TEXT("interp 只能是 cubic / linear / constant，收到 `%s`"), *InterpText));
						bOk = false;
						continue;
					}
				}

				Track.Keys.Add(MoveTemp(Key));
			}

			Animation.Tracks.Add(MoveTemp(Track));
		}

		OutAnimations.Add(MoveTemp(Animation));
	}

	return bOk;
}

void ComputePlan(const TArray<FUmgAnimation>& InTarget, const TArray<FUmgAnimation>& InCurrent, TArray<FForgePlanEntry>& OutPlan)
{
	TMap<FString, const FUmgAnimation*> CurrentByName;
	for (const FUmgAnimation& Animation : InCurrent)
	{
		CurrentByName.Add(Animation.Name, &Animation);
	}

	for (const FUmgAnimation& Target : InTarget)
	{
		const FUmgAnimation* const* Found = CurrentByName.Find(Target.Name);
		if (Found == nullptr)
		{
			FForgePlanEntry Entry;
			Entry.Action = EForgeAction::Create;
			Entry.NodeName = Target.Name;
			Entry.NodePath = FString::Printf(TEXT("animations/%s"), *Target.Name);
			Entry.Detail = TEXT("新建动画");
			OutPlan.Add(MoveTemp(Entry));
			continue;
		}

		if (!AreAnimationsEqual(Target, **Found))
		{
			FForgePlanEntry Entry;
			Entry.Action = EForgeAction::Update;
			Entry.NodeName = Target.Name;
			Entry.NodePath = FString::Printf(TEXT("animations/%s"), *Target.Name);
			Entry.Detail = TEXT("重建轨道与关键帧");
			OutPlan.Add(MoveTemp(Entry));
		}
	}

	for (const FUmgAnimation& Current : InCurrent)
	{
		const bool bStillConfigured = InTarget.ContainsByPredicate(
			[&Current](const FUmgAnimation& Candidate) { return Candidate.Name == Current.Name; });
		if (bStillConfigured)
		{
			continue;
		}

		FForgePlanEntry Entry;
		Entry.Action = EForgeAction::Delete;
		Entry.NodeName = Current.Name;
		Entry.NodePath = FString::Printf(TEXT("animations/%s"), *Current.Name);
		Entry.Detail = TEXT("配置里没有，删除");
		OutPlan.Add(MoveTemp(Entry));
	}
}

void ReadAnimations(UWidgetBlueprint& InBlueprint, TArray<FUmgAnimation>& OutAnimations, TArray<FForgeDiagnostic>& OutDiagnostics)
{
	OutAnimations.Reset();

	UWidgetTree* Tree = InBlueprint.WidgetTree;

	for (UWidgetAnimation* Animation : InBlueprint.Animations)
	{
		if (Animation == nullptr || Animation->MovieScene == nullptr)
		{
			continue;
		}

		FUmgAnimation Out;
		Out.Name = Animation->GetFName().ToString();

		TMap<FGuid, const FWidgetAnimationBinding*> BindingByGuid;
		for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
		{
			BindingByGuid.Add(Binding.AnimationGuid, &Binding);
		}

		for (const FMovieSceneBinding& SceneBinding : Animation->MovieScene->GetBindings())
		{
			const FGuid PossessableGuid = SceneBinding.GetObjectGuid();
			const FWidgetAnimationBinding* const* FoundBinding = BindingByGuid.Find(PossessableGuid);

			FString TargetPath;
			bool bOnSlot = false;
			if (FoundBinding != nullptr && Tree != nullptr && Tree->RootWidget != nullptr)
			{
				bOnSlot = !(*FoundBinding)->SlotWidgetName.IsNone();
				if (UWidget* Target = FindWidgetByNameInTree(*Tree->RootWidget, (*FoundBinding)->WidgetName.ToString()))
				{
					BuildWidgetPath(*Tree->RootWidget, *Target, Tree->RootWidget->GetName(), TargetPath);
				}
			}

			if (TargetPath.IsEmpty())
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Warning,
					FString::Printf(TEXT("animations/%s"), *Out.Name),
					FString::Printf(TEXT("有一条轨道绑定的控件不在控件树里（绑定 Guid %s），导出时跳过"), *PossessableGuid.ToString()));
				continue;
			}

			for (UMovieSceneTrack* SceneTrack : SceneBinding.GetTracks())
			{
				UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(SceneTrack);
				if (PropertyTrack == nullptr)
				{
					continue;
				}

				// 实测点：从 property track 读回属性路径的接口名。
				const FString PropertyPath = PropertyTrack->GetPropertyPath().ToString();
				const FChannelSpec* Spec = FindChannelSpecByPropertyPath(PropertyPath, /*bInOnSlot=*/Cast<UMovieSceneMarginTrack>(SceneTrack) != nullptr);
				if (Spec == nullptr)
				{
					// 引擎侧存在的、配置不支持的通道：如实报出来，别静默丢。
					OutDiagnostics.Emplace(
						EForgeSeverity::Warning,
						FString::Printf(TEXT("animations/%s"), *Out.Name),
						FString::Printf(TEXT("轨道 `%s` 不在支持范围内（导出会漏掉它）"), *PropertyPath));
					continue;
				}

				FUmgTrack Track;
				Track.Target = TargetPath;
				Track.Channel = Spec->ConfigPath;

				if (UMovieSceneMarginTrack* MarginTrack = Cast<UMovieSceneMarginTrack>(SceneTrack))
				{
					if (UMovieSceneSection* Section = MarginTrack->GetAllSections().Num() > 0 ? MarginTrack->GetAllSections()[0] : nullptr)
					{
						if (UMovieSceneMarginSection* MarginSection = Cast<UMovieSceneMarginSection>(Section))
						{
							if (FMovieSceneFloatChannel* Channel = PickMarginChannel(*MarginSection, PropertyPath))
							{
								ReadKeys(*Channel, Track.Keys);
							}
						}
					}
				}
				else if (UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(SceneTrack))
				{
					if (UMovieSceneSection* Section = FloatTrack->GetAllSections().Num() > 0 ? FloatTrack->GetAllSections()[0] : nullptr)
					{
						if (UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
						{
							ReadKeys(FloatSection->GetChannel(), Track.Keys);
						}
					}
				}

				if (Track.Keys.Num() > 0)
				{
					Out.Tracks.Add(MoveTemp(Track));
				}
			}
		}

		OutAnimations.Add(MoveTemp(Out));
	}
}

void ExportToDocument(UWidgetBlueprint& InBlueprint, FForgeDocument& OutDocument, TArray<FForgeDiagnostic>& OutDiagnostics)
{
	TArray<FUmgAnimation> Animations;
	ReadAnimations(InBlueprint, Animations, OutDiagnostics);
	if (Animations.Num() == 0)
	{
		// 没动画就不写字段：省略即空集，配置保持干净。
		return;
	}

	if (!OutDocument.Extra.IsValid())
	{
		OutDocument.Extra = MakeShared<FJsonObject>();
	}

	TArray<TSharedPtr<FJsonValue>> AnimationValues;
	AnimationValues.Reserve(Animations.Num());
	for (const FUmgAnimation& Animation : Animations)
	{
		AnimationValues.Add(MakeShared<FJsonValueObject>(AnimationToJson(Animation)));
	}
	OutDocument.Extra->SetArrayField(TEXT("animations"), AnimationValues);
}

bool Sync(UWidgetBlueprint& InBlueprint, const TArray<FUmgAnimation>& InTarget, TArray<FForgeDiagnostic>& OutDiagnostics)
{
	TMap<FString, UWidgetAnimation*> CurrentByName;
	for (UWidgetAnimation* Animation : InBlueprint.Animations)
	{
		if (Animation != nullptr)
		{
			CurrentByName.Add(Animation->GetFName().ToString(), Animation);
		}
	}

	bool bOk = true;
	TSet<FString> TargetNames;

	for (const FUmgAnimation& Target : InTarget)
	{
		TargetNames.Add(Target.Name);

		UWidgetAnimation* Animation = nullptr;
		if (UWidgetAnimation** Found = CurrentByName.Find(Target.Name))
		{
			Animation = *Found;
		}

		if (Animation == nullptr)
		{
			// 创建序列照抄 AnimationTabSummoner.cpp:612-660（全是公开 API）。
			Animation = NewObject<UWidgetAnimation>(&InBlueprint, FName(*Target.Name), RF_Transactional);
			Animation->SetDisplayLabel(Target.Name);
			InBlueprint.Animations.Add(Animation);
			ForgeWidgetVariableAdded(InBlueprint, FName(*Target.Name));
		}

		if (!RebuildAnimation(InBlueprint, *Animation, Target, OutDiagnostics))
		{
			bOk = false;
		}
	}

	for (auto It = CurrentByName.CreateIterator(); It; ++It)
	{
		if (TargetNames.Contains(It.Key()))
		{
			continue;
		}
		DeleteAnimation(InBlueprint, *It.Value());
	}

	return bOk;
}
} // namespace UmgAnimationCodec
