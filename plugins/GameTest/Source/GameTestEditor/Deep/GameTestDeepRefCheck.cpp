// Copyright Epic Games, Inc. All Rights Reserved.

// 对象引用的深度检查（docs/game-test-ref.md 第 10 节的 part_missing / prop_missing / op_type）：
// 引用的类在不在、`part` 第一段在不在这个类的控件树里、`@self.X` 在类上有没有、op 和属性类型对不对得上。
// 读资产的 WidgetTree，不开游戏也能查；运行时才知道是谁的（`$变量`、Lua 字段、嵌套 WBP 里更深的段）不查。

#include "Deep/GameTestDeepRefCheck.h"

#include "Core/Flow/Deep/GameTestDeepCheck.h"
#include "Core/Flow/GameTestNode.h"
#include "Core/Script/GameTestDiag.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameTestCompat.h"
#include "Observe/Compare/GameTestCompareRegistry.h"
#include "Observe/Ref/GameTestPropKind.h"
#include "Observe/Ref/GameTestPropPath.h"
#include "Observe/Ref/GameTestRef.h"
#include "Observe/Ref/GameTestState.h"
#include "Observe/Sources/GameTestObjectRoots.h"
#include "Observe/Sources/GameTestReflectValue.h"
#include "Widgets/GameTestWidgetTreeReader.h"

#include "Blueprint/UserWidget.h"

namespace GameTestDeepRef
{
	const FName Key(TEXT("refs"));

	/** 写部件的字段（值是 `{obj, part}` 或一个引用）。 */
	const TCHAR* const TargetFields[] = { TEXT("target"), TEXT("toTarget"), TEXT("widget"), TEXT("each") };

	/** 一个界面的全部控件名（含父 WBP 继承来的）；读不到资产时为空。 */
	struct FTreeCache
	{
		TMap<FString, TOptional<TSet<FString>>> Names;

		const TSet<FString>* TreeOf(const FString& InClass)
		{
			if (const TOptional<TSet<FString>>* Cached = Names.Find(InClass))
			{
				return Cached->IsSet() ? &Cached->GetValue() : nullptr;
			}
			TOptional<TSet<FString>>& Slot = Names.Add(InClass);
			FGameTestWidgetTreeQuery Query;
			(InClass.StartsWith(TEXT("/")) ? Query.Asset : Query.Class) = InClass;
			Query.Limit = 1000;
			TSet<FString> Set;
			// 读控件树一页有字数上限：按 nextOffset 翻完，免得大界面后半截的控件被当成不存在
			for (int32 Page = 0; Page < 50; ++Page)
			{
				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				FString Error;
				if (!GameTestWidgetTreeReader::Read(Query, Out, Error) || !Out.IsValid())
				{
					if (Page == 0)
					{
						return nullptr;
					}
					break;
				}
				const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
				if (Out->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& Node : *Nodes)
					{
						const TSharedPtr<FJsonObject> Object = Node.IsValid() ? Node->AsObject() : nullptr;
						FString Name;
						if (Object.IsValid() && Object->TryGetStringField(TEXT("name"), Name))
						{
							Set.Add(Name);
						}
					}
				}
				int32 Next = 0;
				if (!Out->TryGetNumberField(TEXT("nextOffset"), Next) || Next <= Query.Offset)
				{
					break;
				}
				Query.Offset = Next;
			}
			Slot = MoveTemp(Set);
			return &Slot.GetValue();
		}
	};

	class FRefCheck final : public IGameTestDeepCheck
	{
	public:
		virtual FName GetKey() const override { return Key; }

		virtual void CheckScript(const FGameTestScript& InScript, FGameTestDeepEnv& InEnv) const override
		{
			Trees.Names.Reset();
			// objects 里每个引用的 ident
			if (!InScript.Objects.IsValid())
			{
				return;
			}
			for (const auto& Pair : InScript.Objects->Values)
			{
				const TSharedPtr<FJsonObject> Json = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
				if (Json.IsValid())
				{
					FGameTestDiagSink::FScope Scope(InEnv.Sink, TEXT("objects.") + GAMETEST_JSON_KEY(Pair.Key));
					CheckIdent(*Json, InEnv);
				}
			}
		}

		virtual void CheckObject(const FJsonObject& InJson, FGameTestDeepEnv& InEnv) const override
		{
			for (const TCHAR* Field : TargetFields)
			{
				FGameTestTarget Target;
				const TSharedPtr<FJsonValue> Value = InJson.TryGetField(Field);
				if (Value.IsValid() && Value->Type == EJson::Object && FGameTestTarget::FromJson(Value, Target))
				{
					FGameTestDiagSink::FScope Scope(InEnv.Sink, Field);
					CheckRefAndPart(Target.Obj, Target.Part, InEnv);
				}
			}
			// 条件里的对象判断、obj.* 动作：{obj, part?, prop?, op?}
			if (InJson.HasField(TEXT("obj")))
			{
				FString Part;
				InJson.TryGetStringField(TEXT("part"), Part);
				const UClass* Class = nullptr;
				{
					FGameTestDiagSink::FScope Scope(InEnv.Sink, TEXT("obj"));
					Class = CheckRefAndPart(InJson.TryGetField(TEXT("obj")), Part, InEnv);
				}
				if (Part.IsEmpty() && InJson.HasField(TEXT("prop")))
				{
					CheckItem(Class, FGameTestIdentItem::FromJson(InJson), InEnv);
				}
			}
			// 内联引用本身（`{class, ident}`）的 ident
			if (InJson.HasField(TEXT("class")) && InJson.HasField(TEXT("ident")))
			{
				CheckIdent(InJson, InEnv);
			}
		}

	private:
		/** 引用 → 类（只认内联和命名的，`$变量` 运行时才知道）。 */
		FString ClassNameOf(const TSharedPtr<FJsonValue>& InRef, const FGameTestDeepEnv& InEnv) const
		{
			FString Text;
			TSharedPtr<FJsonObject> Inline;
			switch (GameTestRefForm::Classify(InRef, Text, Inline))
			{
			case EGameTestRefForm::Inline:
				break;
			case EGameTestRefForm::Named:
			{
				const TSharedPtr<FJsonObject>* Def = nullptr;
				if (InEnv.Script.Objects.IsValid() && InEnv.Script.Objects->TryGetObjectField(Text, Def) && Def != nullptr)
				{
					Inline = *Def;
				}
				break;
			}
			default:
				return FString();
			}
			FString Class;
			if (Inline.IsValid())
			{
				Inline->TryGetStringField(TEXT("class"), Class);
			}
			return Class;
		}

		/** 查部件第一段在不在类的控件树里；返回已加载的类（查属性用，可能为空）。 */
		const UClass* CheckRefAndPart(const TSharedPtr<FJsonValue>& InRef, const FString& InPart, FGameTestDeepEnv& InEnv) const
		{
			const FString ClassName = ClassNameOf(InRef, InEnv);
			if (ClassName.IsEmpty())
			{
				return nullptr;
			}
			++InEnv.Stats.Refs;
			const UClass* Loaded = GameTestObjectRoots::FindClass(ClassName);
			if (InPart.IsEmpty())
			{
				return Loaded;
			}
			// Actor 的 part 是组件名：只查界面
			if (Loaded != nullptr && !Loaded->IsChildOf(UUserWidget::StaticClass()))
			{
				return Loaded;
			}
			const TSet<FString>* Tree = Trees.TreeOf(ClassName);
			if (Tree == nullptr)
			{
				if (Loaded == nullptr)
				{
					InEnv.Sink.Warning(TEXT("ref_bad_class"), FString::Printf(TEXT("找不到类 %s（写错了，或蓝图类没加载过；gt.tree 能看此刻界面的类名）"), *ClassName));
				}
				return Loaded;
			}
			FString First = InPart;
			InPart.Split(TEXT("."), &First, nullptr);
			if (!Tree->Contains(First))
			{
				FGameTestDiagSink::FScope Scope(InEnv.Sink, TEXT("part"));
				InEnv.Sink.Error(TEXT("part_missing"), FString::Printf(TEXT("%s 的控件树里没有 %s（改名了？用 gt.tree / gt.widgets 看现在的名字）"), *ClassName, *First));
			}
			return Loaded;
		}

		void CheckIdent(const FJsonObject& InRef, FGameTestDeepEnv& InEnv) const
		{
			FString ClassName;
			InRef.TryGetStringField(TEXT("class"), ClassName);
			const UClass* Class = ClassName.IsEmpty() ? nullptr : GameTestObjectRoots::FindClass(ClassName);
			const TArray<TSharedPtr<FJsonValue>>* Ident = nullptr;
			if (!InRef.TryGetArrayField(TEXT("ident"), Ident) || Ident == nullptr)
			{
				return;
			}
			for (int32 Index = 0; Index < Ident->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Item = (*Ident)[Index].IsValid() ? (*Ident)[Index]->AsObject() : nullptr;
				if (Item.IsValid())
				{
					FGameTestDiagSink::FScope Scope(InEnv.Sink, FString::Printf(TEXT("ident[%d]"), Index));
					CheckItem(Class, FGameTestIdentItem::FromJson(*Item), InEnv);
				}
			}
		}

		/** `@self.X` 在类上有没有；算出状态和 `@self.X` 的 op 能不能用在它的类型上。 */
		void CheckItem(const UClass* InClass, const FGameTestIdentItem& InItem, FGameTestDeepEnv& InEnv) const
		{
			FGameTestPropPath Path;
			FString Error;
			if (InItem.Prop.IsEmpty() || !FGameTestPropPath::Parse(InItem.Prop, Path, Error))
			{
				return;
			}
			EGameTestKind Kind = EGameTestKind::None;
			if (Path.Segments.Num() == 0 && Path.HasState())
			{
				if (const IGameTestState* State = FGameTestStateRegistry::Find(Path.State))
				{
					Kind = State->GetKind();
				}
			}
			else if (InClass != nullptr && Path.Segments.Num() == 2 && Path.Segments[0] == TEXT("self") && !Path.HasState())
			{
				++InEnv.Stats.Paths;
				// `Arr[0]` / `Map["k"]` 只查名字
				FString Name = Path.Segments[1];
				Name.Split(TEXT("["), &Name, nullptr);
				const FProperty* Prop = GameTestReflectValue::FindProperty(InClass, Name);
				if (Prop == nullptr)
				{
					if (GameTestReflectValue::FindFunction(InClass, Name) == nullptr)
					{
						FGameTestDiagSink::FScope Scope(InEnv.Sink, TEXT("prop"));
						InEnv.Sink.Error(TEXT("prop_missing"), FString::Printf(TEXT("%s 上没有属性 %s（Lua 字段写 @lua.%s；gt.props 能看全部属性）"), *InClass->GetName(), *Name, *Name));
					}
					return;
				}
				Kind = FGameTestPropKind::Of(Prop);
			}
			if (Kind == EGameTestKind::None || InItem.Op.IsEmpty() || FGameTestCompareRegistry::Find(InItem.Op) == nullptr)
			{
				return;
			}
			const TArray<FString> Ops = FGameTestCompareRegistry::OpsFor(Kind);
			if (!Ops.Contains(InItem.Op))
			{
				FGameTestDiagSink::FScope Scope(InEnv.Sink, TEXT("op"));
				InEnv.Sink.Error(TEXT("op_type"), FString::Printf(TEXT("%s 是 %s，不能用 %s（能用：%s）"), *InItem.Prop, FGameTestPropKind::Name(Kind), *InItem.Op, *FString::Join(Ops, TEXT("、"))));
			}
		}

		mutable FTreeCache Trees;
	};
}

namespace GameTestDeepRefCheck
{
	void Register()
	{
		GameTestDeepChecks::Get().Register(MakeShared<GameTestDeepRef::FRefCheck>());
	}

	void Unregister()
	{
		GameTestDeepChecks::Get().Unregister(GameTestDeepRef::Key);
	}
}
