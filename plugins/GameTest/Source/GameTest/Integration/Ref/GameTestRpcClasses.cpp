// Copyright Epic Games, Inc. All Rights Reserved.

// gt.classes（docs/game-test-ref.md 7.8）：类树，给面板「选对象」的类下拉用。
// UE 类 = 已加载的 UClass + 资产注册表里的蓝图（编辑器里，不加载资产）；lua: true 时是 Lua 类（package.loaded 的模块按元表继承）。

#include "Integration/Ref/GameTestRpcRef.h"

#include "Core/Script/GameTestFlowContext.h"
#include "Observe/Checks/GameTestWidgetProbe.h"
#include "Observe/Lua/GameTestLuaBridge.h"
#include "Observe/Lua/GameTestLuaObjects.h"

#include "AgentPanelBridge.h"
#include "Components/ActorComponent.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtrTemplates.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#endif

namespace GameTestRpcClassesImpl
{
	constexpr int32 DefaultLimit = 200;
	constexpr int32 MaxLimit = 500;
	/** 回包上限（ADR 0067 读结构的上限）：超了截断、给 hasMore。 */
	constexpr int32 MaxChars = 12000;
	/** 往上找父类最多走这么多层（防资产注册表里的坏数据绕圈）。 */
	constexpr int32 MaxChain = 64;

	struct FNode
	{
		FString Parent;
		/** 蓝图的资产（包名）；原生类为空。 */
		FString Asset;
		/** Lua 类时：模块文件。 */
		FString File;
		int32 Live = -1;
		int32 Kids = 0;
		bool bNative = false;
	};

	using FNodes = TMap<FString, FNode>;

	/**
	 * 上一次建好的类表（已数好 Kids）。面板逐层展开、再打开时定位到已选类会连着发好几次 gt.classes，
	 * 每次都重扫全部 UClass / 全部 Lua 模块（Lua 每个模块还要查文件时间）会很慢（用户 2026-11-21 嫌慢）；
	 * 几秒内的请求直接复用。fresh: true 强制重建（面板的「刷新」按钮）。
	 */
	struct FNodesCache
	{
		FNodes Nodes;
		TWeakObjectPtr<UWorld> World;
		bool bHasWorld = false;
		double At = -1.0;
	};
	constexpr double CacheSeconds = 15.0;

	FNodesCache& CacheFor(bool bInLua)
	{
		static FNodesCache UeCache;
		static FNodesCache LuaCache;
		return bInLua ? LuaCache : UeCache;
	}

	const FNodes* CachedNodes(bool bInLua, UWorld* InWorld)
	{
		const FNodesCache& Cache = CacheFor(bInLua);
		const bool bSameWorld = Cache.bHasWorld ? Cache.World.Get() == InWorld : InWorld == nullptr;
		if (Cache.At < 0.0 || !bSameWorld || FPlatformTime::Seconds() - Cache.At > CacheSeconds)
		{
			return nullptr;
		}
		return &Cache.Nodes;
	}

	void StoreNodes(bool bInLua, UWorld* InWorld, const FNodes& InNodes)
	{
		FNodesCache& Cache = CacheFor(bInLua);
		Cache.Nodes = InNodes;
		Cache.World = InWorld;
		Cache.bHasWorld = InWorld != nullptr;
		Cache.At = FPlatformTime::Seconds();
	}

	/** 类在树里的名字：蓝图生成类去掉 `_C`（和引用的 class 写法一致）。 */
	FString NameOf(const UClass* InClass)
	{
		FString Name = InClass->GetName();
		if (!InClass->HasAnyClassFlags(CLASS_Native) && Name.EndsWith(TEXT("_C")))
		{
			Name.LeftChopInline(2);
		}
		return Name;
	}

	/** 热重载、蓝图重编、占位留下的类不进树。 */
	bool IsStale(const UClass* InClass)
	{
		if (InClass == nullptr || InClass->HasAnyClassFlags(CLASS_NewerVersionExists))
		{
			return true;
		}
		const FString Name = InClass->GetName();
		return Name.StartsWith(TEXT("SKEL_")) || Name.StartsWith(TEXT("REINST_")) || Name.StartsWith(TEXT("TRASHCLASS_"))
			|| Name.StartsWith(TEXT("HOTRELOADED_")) || Name.StartsWith(TEXT("PLACEHOLDER-CLASS"));
	}

	void AddLoaded(FNodes& OutNodes, TMap<const UClass*, FString>& OutLoaded)
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			const UClass* Class = *It;
			if (IsStale(Class))
			{
				continue;
			}
			const FString Name = NameOf(Class);
			FNode* Existing = OutNodes.Find(Name);
			// 同名时原生类优先（和 FindClass 一致）
			if (Existing != nullptr && (Existing->bNative || !Class->HasAnyClassFlags(CLASS_Native)))
			{
				continue;
			}
			FNode Node;
			const UClass* Super = Class->GetSuperClass();
			Node.Parent = Super != nullptr ? NameOf(Super) : FString();
			Node.bNative = Class->HasAnyClassFlags(CLASS_Native);
			if (!Node.bNative)
			{
				Node.Asset = Class->GetOutermost()->GetName();
			}
			OutNodes.Add(Name, Node);
			OutLoaded.Add(Class, Name);
		}
	}

#if WITH_EDITOR
	/** 资产注册表的类标签（`/Script/CoreUObject.Class'/Game/UI/WBP_Base.WBP_Base_C'` 或对象路径）→ 树里的名字。 */
	FString NameOfTag(const FString& InTag)
	{
		FString Path = InTag;
		int32 Quote = INDEX_NONE;
		if (Path.FindChar(TEXT('\''), Quote))
		{
			Path.RightChopInline(Quote + 1);
			Path.RemoveFromEnd(TEXT("'"));
		}
		int32 Dot = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), Dot))
		{
			Path.RightChopInline(Dot + 1);
		}
		Path.RemoveFromEnd(TEXT("_C"));
		return Path;
	}

	/** 没加载的蓝图：只读资产注册表的标签，不加载资产。返回注册表是不是还在扫。 */
	bool AddBlueprintAssets(FNodes& OutNodes)
	{
		const IAssetRegistry* Registry = IAssetRegistry::Get();
		if (Registry == nullptr)
		{
			return false;
		}
		TArray<FAssetData> Assets;
		Registry->GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
		for (const FAssetData& Asset : Assets)
		{
			FString Generated;
			FString Parent;
			const FString Name = Asset.GetTagValue(FName(TEXT("GeneratedClass")), Generated) && !Generated.IsEmpty() ? NameOfTag(Generated) : Asset.AssetName.ToString();
			if (Name.IsEmpty() || OutNodes.Contains(Name) || !Asset.GetTagValue(FName(TEXT("ParentClass")), Parent) || Parent.IsEmpty())
			{
				continue;
			}
			FNode Node;
			Node.Parent = NameOfTag(Parent);
			Node.Asset = Asset.PackageName.ToString();
			OutNodes.Add(Name, Node);
		}
		return Registry->IsLoadingAssets();
	}
#endif

	/** 往上找到这几个基类之一就定种类；都没有是 object。 */
	FString KindOf(const FString& InName, const FNodes& InNodes)
	{
		FString Current = InName;
		for (int32 Step = 0; Step < MaxChain && !Current.IsEmpty(); ++Step)
		{
			if (Current == TEXT("Widget"))
			{
				return TEXT("umg");
			}
			if (Current == TEXT("Actor"))
			{
				return TEXT("actor");
			}
			if (Current == TEXT("ActorComponent"))
			{
				return TEXT("component");
			}
			const FNode* Node = InNodes.Find(Current);
			Current = Node != nullptr ? Node->Parent : FString();
		}
		return TEXT("object");
	}

	/** 游戏开着时：这个世界里每个类（不含子类）此刻有几个实例。一次遍历全部对象。 */
	void CountLive(UWorld* InWorld, const TMap<const UClass*, FString>& InLoaded, FNodes& InOutNodes)
	{
		if (InWorld == nullptr)
		{
			return;
		}
		TMap<const UClass*, int32> Counts;
		for (TObjectIterator<UObject> It; It; ++It)
		{
			const UObject* Object = *It;
			if (!IsValid(Object) || Object->IsTemplate() || Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || Object->GetWorld() != InWorld)
			{
				continue;
			}
			Counts.FindOrAdd(Object->GetClass())++;
		}
		for (const TPair<const UClass*, FString>& Pair : InLoaded)
		{
			if (FNode* Node = InOutNodes.Find(Pair.Value))
			{
				const int32* Count = Counts.Find(Pair.Key);
				Node->Live = Count != nullptr ? *Count : 0;
			}
		}
	}

	/** Lua 类：package.loaded 里的模块，父类 = 元表链上的下一层 Lua 模块。 */
	bool AddLuaModules(UWorld* InWorld, FNodes& OutNodes, FString& OutError)
	{
		if (!GameTestLuaBridge::IsAvailable())
		{
			OutError = TEXT("项目没有 UnLua（UELink 没编进 Lua 支持），没有 Lua 类");
			return false;
		}
		const TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetNumberField(TEXT("limit"), 5000);
		TSharedPtr<FJsonObject> Out;
		if (!FAgentPanelBridge::Get().CallOp(TEXT("lua.modules"), Args, Out, OutError))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Out.IsValid() || !Out->TryGetArrayField(TEXT("items"), Items) || Items == nullptr)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Items)
		{
			const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Name;
			if (!Item.IsValid() || !Item->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				continue;
			}
			FNode Node;
			Item->TryGetStringField(TEXT("file"), Node.File);
			double Objects = 0.0;
			if (Item->TryGetNumberField(TEXT("objects"), Objects))
			{
				Node.Live = FMath::RoundToInt(Objects);
			}
			TArray<FString> Chain;
			bool bLoaded = false;
			FString ChainError;
			if (GameTestLuaObjects::ModuleChain(InWorld, Name, Chain, bLoaded, ChainError) && Chain.Num() > 1)
			{
				Node.Parent = Chain[1];
			}
			OutNodes.Add(Name, Node);
		}
		return true;
	}

	/** Lua 模块名所在的目录：按 `/` 和 `.` 切段，去掉最后一段，用 `/` 连（`/Module/A/X`、`UI.Base.X` 都认）。 */
	FString LuaFolderOf(const FString& InName)
	{
		TArray<FString> Parts;
		InName.Replace(TEXT("."), TEXT("/")).ParseIntoArray(Parts, TEXT("/"), true);
		if (Parts.Num() > 0)
		{
			Parts.Pop();
		}
		return FString::Join(Parts, TEXT("/"));
	}

	/** 目录写法统一：`.` 当 `/`，去掉首尾的 `/`。 */
	FString NormalizeDir(const FString& InDir)
	{
		TArray<FString> Parts;
		InDir.Replace(TEXT("."), TEXT("/")).ParseIntoArray(Parts, TEXT("/"), true);
		return FString::Join(Parts, TEXT("/"));
	}

	/**
	 * Lua 类按目录列（用户 2026-11-21：Lua 也要树形，几千个模块平铺看不了）：OutNames = 正好在 InDir 下的模块，
	 * OutDirs = InDir 的下一层子目录和其下（含更深层）的模块数。
	 */
	void PickDir(const FNodes& InNodes, const FString& InDir, TArray<FString>& OutNames, TMap<FString, int32>& OutDirs)
	{
		const FString Prefix = InDir.IsEmpty() ? FString() : InDir + TEXT("/");
		for (const TPair<FString, FNode>& Pair : InNodes)
		{
			const FString Folder = LuaFolderOf(Pair.Key);
			if (Folder.Equals(InDir, ESearchCase::IgnoreCase))
			{
				OutNames.Add(Pair.Key);
				continue;
			}
			if (!Prefix.IsEmpty() && !Folder.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FString Rest = Folder.RightChop(Prefix.Len());
			FString Head = Rest;
			FString Tail;
			Rest.Split(TEXT("/"), &Head, &Tail);
			if (!Head.IsEmpty())
			{
				OutDirs.FindOrAdd(Prefix + Head) += 1;
			}
		}
		OutNames.Sort();
	}

	FString Serialize(const TSharedRef<FJsonObject>& InJson)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(InJson, Writer);
		return Text;
	}

	/** 搜索比对的那一段：Lua 模块名带目录（a.b.c），只比最后一段，不然搜一个目录名会把目录下全部模块都当命中（用户 2026-11-21）；q 里带 . 或 / 时按整条路径比 */
	FString SearchText(const FString& InName, const FString& InQuery)
	{
		if (InQuery.Contains(TEXT(".")) || InQuery.Contains(TEXT("/")))
		{
			return InName;
		}
		int32 Dot = INDEX_NONE;
		int32 Slash = INDEX_NONE;
		InName.FindLastChar(TEXT('.'), Dot);
		InName.FindLastChar(TEXT('/'), Slash);
		const int32 Cut = FMath::Max(Dot, Slash);
		return Cut == INDEX_NONE ? InName : InName.Mid(Cut + 1);
	}

	/** 要列哪些：有 q 时 = 名字里含 q 的（不分大小写，用户 2026-11-21：不要只按开头）+ 它们的祖先；否则 = parent 的直接子类。 */
	void Pick(const FNodes& InNodes, const FString& InParent, const FString& InQuery, TArray<FString>& OutNames)
	{
		if (InQuery.IsEmpty())
		{
			for (const TPair<FString, FNode>& Pair : InNodes)
			{
				if (Pair.Value.Parent.Equals(InParent, ESearchCase::IgnoreCase))
				{
					OutNames.Add(Pair.Key);
				}
			}
			OutNames.Sort();
			return;
		}
		TSet<FString> Picked;
		TArray<FString> Hits;
		for (const TPair<FString, FNode>& Pair : InNodes)
		{
			if (SearchText(Pair.Key, InQuery).Contains(InQuery, ESearchCase::IgnoreCase))
			{
				Hits.Add(Pair.Key);
			}
		}
		// 开头就对上的排前面：命中太多被分页截掉时，先留最像的
		Hits.Sort([&InQuery](const FString& A, const FString& B)
		{
			const bool bA = SearchText(A, InQuery).StartsWith(InQuery, ESearchCase::IgnoreCase);
			const bool bB = SearchText(B, InQuery).StartsWith(InQuery, ESearchCase::IgnoreCase);
			return bA != bB ? bA : A < B;
		});
		// 每个要列的名字记一个档次：开头对上的命中和它的祖先是 0，其余 1；最后先按档次排，分页截掉的是后面那档
		TMap<FString, int32> Rank;
		for (const FString& Hit : Hits)
		{
			const int32 HitRank = SearchText(Hit, InQuery).StartsWith(InQuery, ESearchCase::IgnoreCase) ? 0 : 1;
			FString Current = Hit;
			for (int32 Step = 0; Step < MaxChain && !Current.IsEmpty() && !Picked.Contains(Current); ++Step)
			{
				Picked.Add(Current);
				Rank.Add(Current, HitRank);
				const FNode* Node = InNodes.Find(Current);
				Current = Node != nullptr ? Node->Parent : FString();
			}
		}
		// 先按档次，同一档里祖先在前（按深度），同一层按名字：面板按 parent 拼树时父节点先到
		TMap<FString, int32> Depth;
		for (const FString& Name : Picked)
		{
			int32 Level = 0;
			const FNode* Node = InNodes.Find(Name);
			while (Node != nullptr && !Node->Parent.IsEmpty() && Level < MaxChain)
			{
				++Level;
				Node = InNodes.Find(Node->Parent);
			}
			Depth.Add(Name, Level);
		}
		OutNames = Picked.Array();
		OutNames.Sort([&Depth, &Rank](const FString& A, const FString& B)
		{
			const int32 RankA = Rank.FindRef(A);
			const int32 RankB = Rank.FindRef(B);
			if (RankA != RankB)
			{
				return RankA < RankB;
			}
			const int32 DepthA = Depth.FindRef(A);
			const int32 DepthB = Depth.FindRef(B);
			return DepthA != DepthB ? DepthA < DepthB : A < B;
		});
	}
}

bool GameTestRpcRef::Classes(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
{
	namespace Impl = GameTestRpcClassesImpl;
	const TSharedRef<FJsonObject> Args = InArgs.IsValid() ? InArgs.ToSharedRef() : MakeShared<FJsonObject>();
	bool bLua = false;
	Args->TryGetBoolField(TEXT("lua"), bLua);
	FString Query;
	Args->TryGetStringField(TEXT("q"), Query);
	Query.TrimStartAndEndInline();
	FString Parent;
	const bool bHasParent = Args->TryGetStringField(TEXT("parent"), Parent);
	Parent.TrimStartAndEndInline();
	if (!bHasParent)
	{
		// UE 类从 Object 往下；Lua 类没有公共根，省略时列最顶层的
		Parent = bLua ? FString() : FString(TEXT("Object"));
	}
	double Number = 0.0;
	const int32 Offset = Args->TryGetNumberField(TEXT("offset"), Number) ? FMath::Max(0, FMath::RoundToInt(Number)) : 0;
	const int32 Limit = Args->TryGetNumberField(TEXT("limit"), Number) ? FMath::Clamp(FMath::RoundToInt(Number), 1, Impl::MaxLimit) : Impl::DefaultLimit;

	FGameTestFlowContext Flow;
	MakeContext(Flow);
	UWorld* World = GameTestWidgetProbe::WorldOf(Flow);

	Impl::FNodes Nodes;
	bool bAssetsLoading = false;
	bool bFresh = false;
	Args->TryGetBoolField(TEXT("fresh"), bFresh);
	const Impl::FNodes* Cached = bFresh ? nullptr : Impl::CachedNodes(bLua, World);
	if (Cached != nullptr)
	{
		Nodes = *Cached;
	}
	else if (bLua)
	{
		if (!Impl::AddLuaModules(World, Nodes, OutError))
		{
			return false;
		}
	}
	else
	{
		TMap<const UClass*, FString> Loaded;
		Impl::AddLoaded(Nodes, Loaded);
#if WITH_EDITOR
		bAssetsLoading = Impl::AddBlueprintAssets(Nodes);
#endif
		Impl::CountLive(World, Loaded, Nodes);
	}
	if (Cached == nullptr)
	{
		for (const TPair<FString, Impl::FNode>& Pair : Nodes)
		{
			if (Impl::FNode* Up = Pair.Value.Parent.IsEmpty() ? nullptr : Nodes.Find(Pair.Value.Parent))
			{
				++Up->Kids;
			}
		}
		// 资产注册表还在扫时不缓存：扫完后的下一次要拿到完整的
		if (!bAssetsLoading)
		{
			Impl::StoreNodes(bLua, World, Nodes);
		}
	}
	if (Query.IsEmpty() && !bLua && !Parent.IsEmpty() && !Nodes.Contains(Parent))
	{
		OutError = FString::Printf(TEXT("没有类 %s（类名写错了，或它在没加载的模块里）；不写 parent 从 Object 开始列"), *Parent);
		return false;
	}

	// Lua 按目录列：给了 dir（空字符串是最顶层）且不搜时
	FString Dir;
	const bool bDirMode = bLua && Query.IsEmpty() && Args->TryGetStringField(TEXT("dir"), Dir);
	TArray<FString> Names;
	TMap<FString, int32> Dirs;
	if (bDirMode)
	{
		Impl::PickDir(Nodes, Impl::NormalizeDir(Dir), Names, Dirs);
	}
	else
	{
		Impl::Pick(Nodes, Parent, Query, Names);
	}

	// 子目录先占预算（最多一半），剩下的给模块；子目录只在第一页给，太多时截断并标 dirsMore
	TArray<TSharedPtr<FJsonValue>> DirItems;
	bool bDirsMore = false;
	int32 Chars = 0;
	if (bDirMode && Offset == 0)
	{
		Dirs.KeySort([](const FString& A, const FString& B) { return A < B; });
		for (const TPair<FString, int32>& Pair : Dirs)
		{
			const TSharedRef<FJsonObject> DirItem = MakeShared<FJsonObject>();
			DirItem->SetStringField(TEXT("name"), Pair.Key);
			DirItem->SetNumberField(TEXT("count"), Pair.Value);
			const int32 Size = Impl::Serialize(DirItem).Len() + 1;
			if (DirItems.Num() > 0 && Chars + Size > Impl::MaxChars / 2)
			{
				bDirsMore = true;
				break;
			}
			Chars += Size;
			DirItems.Add(MakeShared<FJsonValueObject>(DirItem));
		}
	}
	TArray<TSharedPtr<FJsonValue>> Items;
	int32 Index = Offset;
	for (; Index < Names.Num() && Items.Num() < Limit; ++Index)
	{
		const Impl::FNode& Node = Nodes.FindChecked(Names[Index]);
		const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Names[Index]);
		if (!Node.Parent.IsEmpty())
		{
			Item->SetStringField(TEXT("parent"), Node.Parent);
		}
		Item->SetStringField(TEXT("kind"), bLua ? FString(TEXT("lua")) : Impl::KindOf(Names[Index], Nodes));
		if (!Node.Asset.IsEmpty())
		{
			Item->SetStringField(TEXT("asset"), Node.Asset);
		}
		if (!Node.File.IsEmpty())
		{
			Item->SetStringField(TEXT("file"), Node.File);
		}
		Item->SetNumberField(TEXT("kids"), Node.Kids);
		if (Node.Live > 0)
		{
			Item->SetNumberField(TEXT("live"), Node.Live);
		}
		const int32 Size = Impl::Serialize(Item).Len() + 1;
		// 至少给一项，面板才能往下翻
		if (Items.Num() > 0 && Chars + Size > Impl::MaxChars)
		{
			break;
		}
		Chars += Size;
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}

	OutResult = MakeShared<FJsonObject>();
	OutResult->SetArrayField(TEXT("items"), Items);
	if (bDirMode && Offset == 0)
	{
		OutResult->SetArrayField(TEXT("dirs"), DirItems);
		if (bDirsMore)
		{
			OutResult->SetBoolField(TEXT("dirsMore"), true);
		}
	}
	OutResult->SetNumberField(TEXT("total"), Names.Num());
	OutResult->SetBoolField(TEXT("hasMore"), Index < Names.Num());
	if (Index < Names.Num())
	{
		OutResult->SetNumberField(TEXT("nextOffset"), Index);
	}
	if (bAssetsLoading)
	{
		OutResult->SetStringField(TEXT("note"), TEXT("资产注册表还在扫描，蓝图类可能不全；稍后再查一次"));
	}
	return true;
}
