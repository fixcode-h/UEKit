// Copyright Epic Games, Inc. All Rights Reserved.

#include "Widgets/GameTestWidgetTreeReader.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/RichTextBlock.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace
{
	/** 返回最多这么多字（协议：读结构的最多 12000 字），超了截断、标 truncated。 */
	constexpr int32 MaxChars = 12000;
	/** 子类 WBP 继承父类的控件树时最多往上找几层。 */
	constexpr int32 MaxParentHops = 8;

	FString StripClassSuffix(FString InName)
	{
		InName.TrimStartAndEndInline();
		if (InName.EndsWith(TEXT("_C")))
		{
			InName.LeftChopInline(2);
		}
		return InName;
	}

	/** 子类 WBP 没有自己的控件树时用父 WBP 的（「继承父界面布局」的写法）。 */
	UWidgetTree* TreeOf(UWidgetBlueprint* InBlueprint, UWidgetBlueprint*& OutOwner)
	{
		UWidgetBlueprint* Current = InBlueprint;
		for (int32 Hop = 0; Current != nullptr && Hop < MaxParentHops; ++Hop)
		{
			if (Current->WidgetTree != nullptr && Current->WidgetTree->RootWidget != nullptr)
			{
				OutOwner = Current;
				return Current->WidgetTree;
			}
			UClass* Parent = Current->ParentClass;
			Current = Parent != nullptr ? Cast<UWidgetBlueprint>(Parent->ClassGeneratedBy) : nullptr;
		}
		OutOwner = InBlueprint;
		return nullptr;
	}

	FString NativeParentName(const UWidgetBlueprint& InBlueprint)
	{
		const UClass* Class = InBlueprint.GeneratedClass != nullptr ? InBlueprint.GeneratedClass.Get() : InBlueprint.ParentClass.Get();
		while (Class != nullptr && !Class->HasAnyClassFlags(CLASS_Native))
		{
			Class = Class->GetSuperClass();
		}
		return Class != nullptr ? Class->GetPrefixCPP() + Class->GetName() : FString();
	}

	/**
	 * 绑的 UnLua 模块：UnLua 的约定是实现 UnLuaInterface、GetModuleName 返回模块名。
	 * 不 include UnLua 的头（它是可选的，项目里还可能是改过的分支），按函数名反射调类默认对象。
	 */
	FString LuaModuleOf(const UWidgetBlueprint& InBlueprint)
	{
		UClass* Class = InBlueprint.GeneratedClass;
		UObject* Cdo = Class != nullptr ? Class->GetDefaultObject() : nullptr;
		UFunction* Function = Cdo != nullptr ? Cdo->FindFunction(TEXT("GetModuleName")) : nullptr;
		if (Function == nullptr || Function->NumParms != 1)
		{
			return FString();
		}
		FStrProperty* Ret = CastField<FStrProperty>(Function->GetReturnProperty());
		if (Ret == nullptr)
		{
			return FString();
		}
		TArray<uint8> Buffer;
		Buffer.SetNumZeroed(FMath::Max<int32>(Function->ParmsSize, 1));
		Ret->InitializeValue_InContainer(Buffer.GetData());
		Cdo->ProcessEvent(Function, Buffer.GetData());
		const FString Module = Ret->GetPropertyValue_InContainer(Buffer.GetData());
		Ret->DestroyValue_InContainer(Buffer.GetData());
		return Module;
	}

	/** 列表、动态条目框的条目类：按反射读 EntryWidgetClass，不依赖各容器各版本的访问函数。 */
	FString EntryClassOf(const UWidget& InWidget)
	{
		const FClassProperty* Prop = CastField<FClassProperty>(InWidget.GetClass()->FindPropertyByName(TEXT("EntryWidgetClass")));
		if (Prop == nullptr)
		{
			return FString();
		}
		const UClass* Entry = Cast<UClass>(Prop->GetObjectPropertyValue_InContainer(&InWidget));
		return Entry != nullptr ? StripClassSuffix(Entry->GetName()) : FString();
	}

	FString TextOf(const UWidget& InWidget)
	{
		if (const UTextBlock* Text = Cast<UTextBlock>(&InWidget))
		{
			return Text->GetText().ToString();
		}
		if (const URichTextBlock* Rich = Cast<URichTextBlock>(&InWidget))
		{
			return Rich->GetText().ToString();
		}
		return FString();
	}

	struct FWalk
	{
		FString ScreenName;
		FString Filter;
		TArray<TSharedPtr<FJsonObject>> Nodes;

		void Visit(UWidget* InWidget, const FString& InParentPath, int32 InDepth)
		{
			if (InWidget == nullptr)
			{
				return;
			}
			const FString Path = InParentPath.IsEmpty() ? InWidget->GetName() : InParentPath + TEXT("/") + InWidget->GetName();
			const FString ClassName = StripClassSuffix(InWidget->GetClass()->GetName());
			if (Filter.IsEmpty() || InWidget->GetName().Contains(Filter) || ClassName.Contains(Filter))
			{
				const TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
				Node->SetStringField(TEXT("name"), InWidget->GetName());
				Node->SetStringField(TEXT("class"), ClassName);
				Node->SetStringField(TEXT("selector"), ScreenName + TEXT(":") + Path);
				Node->SetNumberField(TEXT("depth"), InDepth);
				Node->SetBoolField(TEXT("isVariable"), InWidget->bIsVariable);
				const FString Text = TextOf(*InWidget);
				if (!Text.IsEmpty())
				{
					Node->SetStringField(TEXT("text"), Text);
				}
				if (const UEnum* Enum = StaticEnum<ESlateVisibility>())
				{
					Node->SetStringField(TEXT("visibility"), Enum->GetNameStringByValue((int64)InWidget->GetVisibility()));
				}
				const FString Entry = EntryClassOf(*InWidget);
				if (!Entry.IsEmpty())
				{
					const TSharedPtr<FJsonObject> Container = MakeShared<FJsonObject>();
					Container->SetStringField(TEXT("entryClass"), Entry);
					Node->SetObjectField(TEXT("container"), Container);
				}
				if (InWidget->IsA<UUserWidget>())
				{
					// 嵌套的 WBP：它里面的控件用 `>` 进去，再调一次 gt.widgets class=它 看
					Node->SetBoolField(TEXT("userWidget"), true);
				}
				Nodes.Add(Node);
			}
			if (const UPanelWidget* Panel = Cast<UPanelWidget>(InWidget))
			{
				for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
				{
					Visit(Panel->GetChildAt(Index), Path, InDepth + 1);
				}
			}
		}
	};
}

UWidgetBlueprint* GameTestWidgetTreeReader::FindWidgetBlueprint(const FGameTestWidgetTreeQuery& InQuery, FString& OutError)
{
	if (!InQuery.Asset.IsEmpty())
	{
		FString Path = InQuery.Asset;
		// 「/Game/UI/WBP_Bag」补成对象路径「/Game/UI/WBP_Bag.WBP_Bag」
		if (!Path.Contains(TEXT(".")))
		{
			Path += TEXT(".") + FPackageName::GetShortName(Path);
		}
		UWidgetBlueprint* Blueprint = LoadObject<UWidgetBlueprint>(nullptr, *Path);
		if (Blueprint == nullptr)
		{
			OutError = FString::Printf(TEXT("没有这个 WBP 资产：%s（路径写成 /Game/…/WBP_X）"), *InQuery.Asset);
		}
		return Blueprint;
	}

	const FString Name = StripClassSuffix(InQuery.Class);
	if (Name.IsEmpty())
	{
		OutError = TEXT("要给 asset（WBP 资产路径）或 class（WBP 类名）");
		return nullptr;
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	Registry.GetAssetsByClass(UWidgetBlueprint::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
	TArray<FAssetData> Hits;
	TArray<FString> Near;
	for (const FAssetData& Asset : Assets)
	{
		const FString AssetName = Asset.AssetName.ToString();
		if (AssetName.Equals(Name, ESearchCase::IgnoreCase))
		{
			Hits.Add(Asset);
		}
		else if (Near.Num() < 8 && AssetName.Contains(Name))
		{
			Near.Add(AssetName);
		}
	}
	if (Hits.Num() == 0)
	{
		OutError = FString::Printf(TEXT("没有叫 %s 的 WBP%s"), *Name,
			Near.Num() > 0 ? *FString::Printf(TEXT("，名字相近的：%s"), *FString::Join(Near, TEXT("、"))) : TEXT(""));
		return nullptr;
	}
	if (Hits.Num() > 1)
	{
		TArray<FString> Paths;
		for (const FAssetData& Hit : Hits)
		{
			Paths.Add(Hit.PackageName.ToString());
		}
		OutError = FString::Printf(TEXT("有 %d 个叫 %s 的 WBP，用 asset 写资产路径：%s"), Hits.Num(), *Name, *FString::Join(Paths, TEXT("、")));
		return nullptr;
	}
	UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Hits[0].GetAsset());
	if (Blueprint == nullptr)
	{
		OutError = FString::Printf(TEXT("%s 加载失败"), *Hits[0].PackageName.ToString());
	}
	return Blueprint;
}

bool GameTestWidgetTreeReader::Read(const FGameTestWidgetTreeQuery& InQuery, TSharedPtr<FJsonObject>& Out, FString& OutError)
{
	UWidgetBlueprint* Blueprint = FindWidgetBlueprint(InQuery, OutError);
	if (Blueprint == nullptr)
	{
		return false;
	}
	UWidgetBlueprint* TreeOwner = nullptr;
	UWidgetTree* Tree = TreeOf(Blueprint, TreeOwner);

	FWalk Walk;
	Walk.ScreenName = Blueprint->GetName();
	Walk.Filter = InQuery.Filter;
	if (Tree != nullptr)
	{
		Walk.Visit(Tree->RootWidget, FString(), 0);
	}

	Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset"), Blueprint->GetPackage()->GetName());
	Out->SetStringField(TEXT("class"), Blueprint->GetName());
	Out->SetStringField(TEXT("parent"), NativeParentName(*Blueprint));
	Out->SetStringField(TEXT("selector"), Blueprint->GetName() + TEXT(":."));
	if (TreeOwner != nullptr && TreeOwner != Blueprint)
	{
		// 控件树是从父 WBP 继承来的：选择器的屏名照样写子类名（运行时屏是子类的实例）
		Out->SetStringField(TEXT("treeFrom"), TreeOwner->GetPackage()->GetName());
	}
	const FString LuaModule = LuaModuleOf(*Blueprint);
	if (!LuaModule.IsEmpty())
	{
		Out->SetStringField(TEXT("luaModule"), LuaModule);
	}

	const int32 Offset = FMath::Max(0, InQuery.Offset);
	const int32 Limit = FMath::Clamp(InQuery.Limit, 1, 1000);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	int32 Chars = 0;
	bool bTruncated = false;
	for (int32 Index = Offset; Index < Walk.Nodes.Num() && Nodes.Num() < Limit; ++Index)
	{
		// 粗算长度：字段名和值加起来，够判断截不截断
		int32 Size = 80;
		for (const auto& Pair : Walk.Nodes[Index]->Values)
		{
			FString Value;
			Size += Pair.Value.IsValid() && Pair.Value->TryGetString(Value) ? Value.Len() : 8;
		}
		if (Chars + Size > MaxChars && Nodes.Num() > 0)
		{
			bTruncated = true;
			break;
		}
		Chars += Size;
		Nodes.Add(MakeShared<FJsonValueObject>(Walk.Nodes[Index]));
	}
	Out->SetArrayField(TEXT("nodes"), Nodes);
	Out->SetNumberField(TEXT("total"), Walk.Nodes.Num());
	const int32 Next = Offset + Nodes.Num();
	if (Next < Walk.Nodes.Num())
	{
		Out->SetBoolField(TEXT("hasMore"), true);
		Out->SetNumberField(TEXT("nextOffset"), Next);
	}
	if (bTruncated)
	{
		Out->SetBoolField(TEXT("truncated"), true);
	}
	if (Tree == nullptr)
	{
		Out->SetStringField(TEXT("note"), TEXT("这个 WBP 没有控件树（纯 C++ 父类建的界面，或还没编译过）"));
	}
	return true;
}
