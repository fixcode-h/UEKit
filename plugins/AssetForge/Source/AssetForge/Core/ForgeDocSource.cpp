// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ForgeDocSource.h"
#include "AssetForgeCompat.h"

#include "Core/ForgeJsonText.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

namespace ForgeDocSource
{

namespace
{
	const TCHAR* const FieldInclude = TEXT("include");
	const TCHAR* const FieldFragment = TEXT("fragment");
	const TCHAR* const FieldKind = TEXT("kind");
	const TCHAR* const FieldRoot = TEXT("root");
	const TCHAR* const FieldChildren = TEXT("children");
	const TCHAR* const FieldName = TEXT("name");
	const TCHAR* const FieldComment = TEXT("//");
	const TCHAR* const FieldSchemaRef = TEXT("$schema");
	/** 节点片段的 fragment 值，内容放在同名字段 `node` 里。 */
	const TCHAR* const FragmentNode = TEXT("node");

	/** 头字段不能 include（它们短，而且 kind / asset 决定这份文件是什么，必须在主文件里一眼可见）。 */
	bool IsHeaderField(const FString& InKey)
	{
		return InKey == FieldKind
			|| InKey == FieldComment
			|| InKey == FieldSchemaRef
			|| InKey == TEXT("schemaVersion")
			|| InKey == TEXT("revision")
			|| InKey == TEXT("asset")
			|| InKey == TEXT("parent");
	}

	FString NormalizePath(const FString& InPath)
	{
		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Path;
	}

	TSharedPtr<FJsonObject> ObjectOf(const TSharedPtr<FJsonValue>& InValue)
	{
		if (!InValue.IsValid() || InValue->Type != EJson::Object)
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!InValue->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
		{
			return nullptr;
		}
		return *Object;
	}

	TSharedPtr<FJsonValue> ValueOf(const TSharedPtr<FJsonObject>& InObject, const FString& InKey)
	{
		if (!InObject.IsValid())
		{
			return nullptr;
		}
		const TSharedPtr<FJsonValue>* Found = InObject->Values.Find(FForgeJsonKey(InKey));
		return Found != nullptr ? *Found : nullptr;
	}

	FString NameOf(const TSharedPtr<FJsonObject>& InNode)
	{
		FString Name;
		if (InNode.IsValid())
		{
			InNode->TryGetStringField(FieldName, Name);
		}
		return Name;
	}

	/** 节点片段的内容：一个节点对象，或节点数组。 */
	bool ToNodeList(const TSharedPtr<FJsonValue>& InContent, TArray<TSharedPtr<FJsonValue>>& OutItems)
	{
		if (!InContent.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (InContent->Type == EJson::Array && InContent->TryGetArray(Items) && Items != nullptr)
		{
			OutItems = *Items;
			return true;
		}
		if (InContent->Type == EJson::Object)
		{
			OutItems.Add(InContent);
			return true;
		}
		return false;
	}

	enum class EIncludeForm : uint8
	{
		/** 不是 include。 */
		None,
		Valid,
		/** 是 include 但写错了（已报错）。 */
		Invalid,
	};

	/** `{ "include": "路径" }`：只许再带 `//` 注释——片段的内容整个替换这个位置，别的字段没地方放。 */
	EIncludeForm ReadInclude(const TSharedPtr<FJsonValue>& InValue, const FString& InPath, FString& OutRef, TArray<FForgeDiagnostic>& OutDiagnostics)
	{
		const TSharedPtr<FJsonObject> Object = ObjectOf(InValue);
		if (!Object.IsValid() || !Object->HasField(FieldInclude))
		{
			return EIncludeForm::None;
		}
		if (!Object->TryGetStringField(FieldInclude, OutRef) || OutRef.IsEmpty())
		{
			OutDiagnostics.Emplace(EForgeSeverity::Error, InPath, TEXT("include 写片段文件的路径（字符串，相对本文件）"));
			return EIncludeForm::Invalid;
		}
		for (const auto& Pair : Object->Values)
		{
			const FString Key(ASSETFORGE_JSON_KEY(Pair.Key));
			if (Key != FieldInclude && Key != FieldComment)
			{
				OutDiagnostics.Emplace(
					EForgeSeverity::Error,
					InPath,
					FString::Printf(TEXT("include 不能和别的字段（%s）写在一起：片段里的内容整个替换这个位置，要改的写进片段"), *Key));
				return EIncludeForm::Invalid;
			}
		}
		return EIncludeForm::Valid;
	}

	// ---------------------------------------------------------------- 展开

	struct FExpandContext
	{
		const FLoader& Loader;
		FString MainPath;
		FString MainDir;
		/** 正在展开的文件链（防环）。 */
		TArray<FString> Stack;
		/** 片段在这份配置里被引用的次数。 */
		TMap<FString, int32> Uses;
		FExpanded& Out;
		TArray<FForgeDiagnostic>& Diagnostics;

		FExpandContext(const FLoader& InLoader, const FString& InMainPath, FExpanded& InOut, TArray<FForgeDiagnostic>& InDiagnostics)
			: Loader(InLoader)
			, MainPath(InMainPath)
			, MainDir(FPaths::GetPath(InMainPath))
			, Out(InOut)
			, Diagnostics(InDiagnostics)
		{
		}

		/** 片段相对主文件目录的写法：诊断和面板里显示它。 */
		FString Label(const FString& InPath) const
		{
			FString Relative = InPath;
			FPaths::MakePathRelativeTo(Relative, *(MainDir + TEXT("/")));
			return Relative;
		}

		void Error(const FString& InPath, const FString& InMessage)
		{
			Diagnostics.Emplace(EForgeSeverity::Error, InPath, InMessage);
		}
	};

	/** 读片段并核对：是片段、类型对得上、内容字段在。成功时 OutContent 是片段内容（还没展开里面的 include）。 */
	bool LoadFragment(FExpandContext& Ctx, const FString& InRef, const FString& InFromFile, const FString& InExpected, const FString& InPath, FString& OutPath, TSharedPtr<FJsonValue>& OutContent)
	{
		OutPath = ResolveIncludePath(InRef, InFromFile);
		if (Ctx.Stack.Contains(OutPath))
		{
			TArray<FString> Chain;
			for (const FString& File : Ctx.Stack)
			{
				Chain.Add(Ctx.Label(File));
			}
			Chain.Add(Ctx.Label(OutPath));
			Ctx.Error(InPath, FString::Printf(TEXT("include 成环：%s"), *FString::Join(Chain, TEXT(" → "))));
			return false;
		}

		int32& UseCount = Ctx.Uses.FindOrAdd(OutPath);
		++UseCount;
		if (UseCount == 2)
		{
			Ctx.Diagnostics.Emplace(
				EForgeSeverity::Warning,
				InPath,
				FString::Printf(TEXT("片段 %s 在这份配置里被引用了不止一次：内容会展开成几份（控件重名）。要复用请用 styles 或 ref 子界面"), *Ctx.Label(OutPath)));
		}

		TSharedPtr<FJsonObject> Json;
		FString LoadError;
		if (!Ctx.Loader(OutPath, Json, LoadError) || !Json.IsValid())
		{
			Ctx.Error(InPath, FString::Printf(TEXT("读不了片段 %s：%s"), *Ctx.Label(OutPath), *LoadError));
			return false;
		}
		if (Json->HasField(FieldKind))
		{
			Ctx.Error(InPath, FString::Printf(TEXT("%s 是一份资源配置（有 kind），不是片段：嵌套另一个资产用 ref，include 只引用片段（顶层写 fragment）"), *Ctx.Label(OutPath)));
			return false;
		}
		FString Fragment;
		if (!Json->TryGetStringField(FieldFragment, Fragment) || Fragment.IsEmpty())
		{
			Ctx.Error(InPath, FString::Printf(TEXT("%s 不是片段：片段顶层要写 fragment（node 或段名）和同名字段"), *Ctx.Label(OutPath)));
			return false;
		}
		if (!Fragment.Equals(InExpected, ESearchCase::CaseSensitive))
		{
			Ctx.Error(InPath, FString::Printf(TEXT("%s 是「%s」片段，不能 include 到这里（这里要的是「%s」片段）"), *Ctx.Label(OutPath), *Fragment, *InExpected));
			return false;
		}
		for (const auto& Pair : Json->Values)
		{
			const FString Key(ASSETFORGE_JSON_KEY(Pair.Key));
			if (Key != FieldFragment && Key != FieldComment && Key != FieldSchemaRef && !Key.Equals(Fragment, ESearchCase::CaseSensitive))
			{
				Ctx.Error(InPath, FString::Printf(TEXT("片段 %s 只能写 fragment 和 %s 两个字段，多了 %s"), *Ctx.Label(OutPath), *Fragment, *Key));
				return false;
			}
		}
		OutContent = ValueOf(Json, Fragment);
		if (!OutContent.IsValid())
		{
			Ctx.Error(InPath, FString::Printf(TEXT("片段 %s 缺少 %s 字段（内容放在和 fragment 同名的字段里）"), *Ctx.Label(OutPath), *Fragment));
			return false;
		}
		Ctx.Out.Fragments.AddUnique(OutPath);
		return true;
	}

	bool ExpandNode(FExpandContext& Ctx, const TSharedPtr<FJsonObject>& InNode, const FString& InPath, const FString& InFromFile, TSharedPtr<FJsonObject>& OutNode);

	/** 展开一组子节点：include 处换成片段里的节点（一个或一组），下标按展开后的位置算。 */
	bool ExpandList(FExpandContext& Ctx, const TArray<TSharedPtr<FJsonValue>>& InItems, const FString& InParentPath, const FString& InFromFile, TArray<TSharedPtr<FJsonValue>>& OutItems)
	{
		bool bOk = true;
		for (const TSharedPtr<FJsonValue>& Item : InItems)
		{
			const FString ItemPath = FString::Printf(TEXT("%s.children[%d]"), *InParentPath, OutItems.Num());

			FString Ref;
			const EIncludeForm Form = ReadInclude(Item, ItemPath, Ref, Ctx.Diagnostics);
			if (Form == EIncludeForm::Invalid)
			{
				bOk = false;
				continue;
			}
			if (Form == EIncludeForm::Valid)
			{
				FString FragmentPath;
				TSharedPtr<FJsonValue> Content;
				if (!LoadFragment(Ctx, Ref, InFromFile, FragmentNode, ItemPath, FragmentPath, Content))
				{
					bOk = false;
					continue;
				}
				TArray<TSharedPtr<FJsonValue>> FragmentItems;
				if (!ToNodeList(Content, FragmentItems))
				{
					Ctx.Error(ItemPath, FString::Printf(TEXT("片段 %s 的 node 要写一个节点对象或节点数组"), *Ctx.Label(FragmentPath)));
					bOk = false;
					continue;
				}
				Ctx.Stack.Push(FragmentPath);
				bOk &= ExpandList(Ctx, FragmentItems, InParentPath, FragmentPath, OutItems);
				Ctx.Stack.Pop();
				continue;
			}

			const TSharedPtr<FJsonObject> NodeObject = ObjectOf(Item);
			if (!NodeObject.IsValid())
			{
				// 不是对象：原样放着，文档解析会报「子节点必须是 JSON 对象」，那里的路径更准。
				OutItems.Add(Item);
				continue;
			}
			if (InFromFile != Ctx.MainPath)
			{
				Ctx.Out.Origins.Add(ItemPath, Ctx.Label(InFromFile));
			}
			TSharedPtr<FJsonObject> Expanded;
			bOk &= ExpandNode(Ctx, NodeObject, ItemPath, InFromFile, Expanded);
			OutItems.Add(MakeShared<FJsonValueObject>(Expanded));
		}
		return bOk;
	}

	bool ExpandNode(FExpandContext& Ctx, const TSharedPtr<FJsonObject>& InNode, const FString& InPath, const FString& InFromFile, TSharedPtr<FJsonObject>& OutNode)
	{
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (!InNode->TryGetArrayField(FieldChildren, Children) || Children == nullptr)
		{
			OutNode = InNode;
			return true;
		}

		// 复制一层再换 children：InNode 是加载器给的原件，不在原件上改。
		OutNode = MakeShared<FJsonObject>();
		for (const auto& Pair : InNode->Values)
		{
			OutNode->SetField(ASSETFORGE_JSON_KEY(Pair.Key), Pair.Value);
		}
		TArray<TSharedPtr<FJsonValue>> ExpandedChildren;
		const bool bOk = ExpandList(Ctx, *Children, InPath, InFromFile, ExpandedChildren);
		OutNode->SetArrayField(FieldChildren, ExpandedChildren);
		return bOk;
	}

	/** 一个顶层段：整段 include 时换成片段内容（片段还能再 include 同一段的片段）；root 段再展开里面的节点 include。 */
	bool ExpandSection(FExpandContext& Ctx, const FString& InSection, const TSharedPtr<FJsonValue>& InValue, const FString& InFromFile, TSharedPtr<FJsonValue>& OutValue)
	{
		FString Ref;
		const EIncludeForm Form = ReadInclude(InValue, InSection, Ref, Ctx.Diagnostics);
		if (Form == EIncludeForm::Invalid)
		{
			return false;
		}
		if (Form == EIncludeForm::Valid)
		{
			FString FragmentPath;
			TSharedPtr<FJsonValue> Content;
			if (!LoadFragment(Ctx, Ref, InFromFile, InSection, InSection, FragmentPath, Content))
			{
				return false;
			}
			Ctx.Out.Origins.Add(InSection, Ctx.Label(FragmentPath));
			Ctx.Stack.Push(FragmentPath);
			const bool bSectionOk = ExpandSection(Ctx, InSection, Content, FragmentPath, OutValue);
			Ctx.Stack.Pop();
			return bSectionOk;
		}

		if (InSection == FieldRoot)
		{
			const TSharedPtr<FJsonObject> RootObject = ObjectOf(InValue);
			if (RootObject.IsValid())
			{
				TSharedPtr<FJsonObject> Expanded;
				const bool bRootOk = ExpandNode(Ctx, RootObject, FieldRoot, InFromFile, Expanded);
				OutValue = MakeShared<FJsonValueObject>(Expanded);
				return bRootOk;
			}
		}
		OutValue = InValue;
		return true;
	}

	// ---------------------------------------------------------------- 写回

	struct FCollapseContext
	{
		const FLoader& Loader;
		/** 正在拆的文件链（防环）。 */
		TArray<FString> Stack;
		TArray<FFileWrite>& Writes;
		TArray<FForgeDiagnostic>& Diagnostics;
		/** 遇到环：放弃按片段拆，整份写主文件。 */
		bool bCycle = false;

		FCollapseContext(const FLoader& InLoader, TArray<FFileWrite>& InWrites, TArray<FForgeDiagnostic>& InDiagnostics)
			: Loader(InLoader)
			, Writes(InWrites)
			, Diagnostics(InDiagnostics)
		{
		}

		void AddWrite(const FString& InPath, const TSharedPtr<FJsonObject>& InJson, const bool bInFragment)
		{
			FFileWrite* Existing = Writes.FindByPredicate([&InPath](const FFileWrite& Candidate) { return Candidate.Path == InPath; });
			FFileWrite& Write = Existing != nullptr ? *Existing : Writes.AddDefaulted_GetRef();
			Write.Path = InPath;
			Write.Json = InJson;
			Write.bFragment = bInFragment;
		}
	};

	/** 读片段原件（写回时对照它的结构）。读不了、不是这种片段时返回 false；成环时另外记下 bCycle。 */
	bool ReadRawFragment(FCollapseContext& Ctx, const FString& InRef, const FString& InFromFile, const FString& InExpected, FString& OutPath, TSharedPtr<FJsonObject>& OutFragment, TSharedPtr<FJsonValue>& OutContent)
	{
		OutPath = ResolveIncludePath(InRef, InFromFile);
		if (Ctx.Stack.Contains(OutPath))
		{
			Ctx.bCycle = true;
			return false;
		}
		FString LoadError;
		FString Fragment;
		if (!Ctx.Loader(OutPath, OutFragment, LoadError)
			|| !OutFragment.IsValid()
			|| OutFragment->HasField(FieldKind)
			|| !OutFragment->TryGetStringField(FieldFragment, Fragment)
			|| !Fragment.Equals(InExpected, ESearchCase::CaseSensitive))
		{
			return false;
		}
		OutContent = ValueOf(OutFragment, Fragment);
		return OutContent.IsValid();
	}

	/** 片段文件的新内容：保留原来的 `//` 注释，`$schema` 由写盘时补。 */
	TSharedPtr<FJsonObject> MakeFragmentJson(const TSharedPtr<FJsonObject>& InRawFragment, const FString& InFragment, const TSharedPtr<FJsonValue>& InContent)
	{
		const TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		if (const TSharedPtr<FJsonValue> Comment = ValueOf(InRawFragment, FieldComment); Comment.IsValid())
		{
			Json->SetField(FieldComment, Comment);
		}
		Json->SetStringField(FieldFragment, InFragment);
		Json->SetField(InFragment, InContent);
		return Json;
	}

	/** 一组原始子节点（可能含 include）展开后会有哪些节点名：写回时按名字把新节点认回原来的片段。 */
	void CollectNames(FCollapseContext& Ctx, const TArray<TSharedPtr<FJsonValue>>& InRawItems, const FString& InFromFile, TSet<FString>& OutNames)
	{
		for (const TSharedPtr<FJsonValue>& Item : InRawItems)
		{
			FString Ref;
			TArray<FForgeDiagnostic> Ignored;
			if (ReadInclude(Item, FString(), Ref, Ignored) == EIncludeForm::Valid)
			{
				FString FragmentPath;
				TSharedPtr<FJsonObject> RawFragment;
				TSharedPtr<FJsonValue> Content;
				TArray<TSharedPtr<FJsonValue>> FragmentItems;
				if (ReadRawFragment(Ctx, Ref, InFromFile, FragmentNode, FragmentPath, RawFragment, Content) && ToNodeList(Content, FragmentItems))
				{
					Ctx.Stack.Push(FragmentPath);
					CollectNames(Ctx, FragmentItems, FragmentPath, OutNames);
					Ctx.Stack.Pop();
				}
				continue;
			}
			const FString Name = NameOf(ObjectOf(Item));
			if (!Name.IsEmpty())
			{
				OutNames.Add(Name);
			}
		}
	}

	TSharedPtr<FJsonObject> CollapseNode(FCollapseContext& Ctx, const TSharedPtr<FJsonObject>& InRawNode, const TSharedPtr<FJsonObject>& InNewNode, const FString& InFromFile);

	/** 原文件里一个节点 include 的位置，和新文档里认回它的那些节点。 */
	struct FIncludeSlot
	{
		TSharedPtr<FJsonValue> RawItem;
		FString Path;
		TSharedPtr<FJsonObject> RawFragment;
		TArray<TSharedPtr<FJsonValue>> RawItems;
		/** 原来写的是单个节点对象（不是数组）。 */
		bool bSingle = false;
		TSet<FString> Names;
		TArray<TSharedPtr<FJsonValue>> Owned;
		bool bEmitted = false;
	};

	/**
	 * 一组子节点拆回文件：新节点名字在某个 include 的片段里出现过，就归那个片段（include 留在它第一个节点的位置）；
	 * 否则按名字对上原文件里的节点继续往下拆，对不上的是新增节点，写在当前文件。
	 */
	TArray<TSharedPtr<FJsonValue>> CollapseList(FCollapseContext& Ctx, const TArray<TSharedPtr<FJsonValue>>& InRawItems, const TArray<TSharedPtr<FJsonValue>>& InNewItems, const FString& InFromFile)
	{
		TArray<FIncludeSlot> Slots;
		TMap<FString, TSharedPtr<FJsonObject>> RawByName;
		for (const TSharedPtr<FJsonValue>& Item : InRawItems)
		{
			FString Ref;
			TArray<FForgeDiagnostic> Ignored;
			if (ReadInclude(Item, FString(), Ref, Ignored) == EIncludeForm::Valid)
			{
				FIncludeSlot Slot;
				Slot.RawItem = Item;
				TSharedPtr<FJsonValue> Content;
				if (ReadRawFragment(Ctx, Ref, InFromFile, FragmentNode, Slot.Path, Slot.RawFragment, Content) && ToNodeList(Content, Slot.RawItems))
				{
					Slot.bSingle = Content->Type == EJson::Object;
					Ctx.Stack.Push(Slot.Path);
					CollectNames(Ctx, Slot.RawItems, Slot.Path, Slot.Names);
					Ctx.Stack.Pop();
					Slots.Add(MoveTemp(Slot));
				}
				else if (!Ctx.bCycle)
				{
					Ctx.Diagnostics.Emplace(
						EForgeSeverity::Warning,
						FString(),
						FString::Printf(TEXT("片段 %s 读不了或不是节点片段：它原来的节点写回了 %s"), *Ref, *FPaths::GetCleanFilename(InFromFile)));
				}
				continue;
			}
			const TSharedPtr<FJsonObject> RawObject = ObjectOf(Item);
			const FString Name = NameOf(RawObject);
			if (!Name.IsEmpty() && !RawByName.Contains(Name))
			{
				RawByName.Add(Name, RawObject);
			}
		}

		TArray<TSharedPtr<FJsonValue>> Out;
		TSet<FString> Claimed;
		for (const TSharedPtr<FJsonValue>& NewItem : InNewItems)
		{
			const TSharedPtr<FJsonObject> NewObject = ObjectOf(NewItem);
			const FString Name = NameOf(NewObject);
			if (!NewObject.IsValid() || Name.IsEmpty())
			{
				Out.Add(NewItem);
				continue;
			}

			FIncludeSlot* Owner = nullptr;
			if (!Claimed.Contains(Name))
			{
				Owner = Slots.FindByPredicate([&Name](const FIncludeSlot& Candidate) { return Candidate.Names.Contains(Name); });
			}
			if (Owner != nullptr)
			{
				Claimed.Add(Name);
				Owner->Owned.Add(NewItem);
				if (!Owner->bEmitted)
				{
					Out.Add(Owner->RawItem);
					Owner->bEmitted = true;
				}
				continue;
			}

			TSharedPtr<FJsonObject> RawNode;
			if (const TSharedPtr<FJsonObject>* RawMatch = RawByName.Find(Name))
			{
				RawNode = *RawMatch;
			}
			Out.Add(MakeShared<FJsonValueObject>(CollapseNode(Ctx, RawNode, NewObject, InFromFile)));
		}

		for (FIncludeSlot& Slot : Slots)
		{
			// 片段里的节点都没了：include 还留着（结构不变），片段写成空的。
			if (!Slot.bEmitted)
			{
				Out.Add(Slot.RawItem);
			}
			Ctx.Stack.Push(Slot.Path);
			const TArray<TSharedPtr<FJsonValue>> FragmentItems = CollapseList(Ctx, Slot.RawItems, Slot.Owned, Slot.Path);
			Ctx.Stack.Pop();
			const TSharedPtr<FJsonValue> Content = (Slot.bSingle && FragmentItems.Num() == 1)
				? FragmentItems[0]
				: TSharedPtr<FJsonValue>(MakeShared<FJsonValueArray>(FragmentItems));
			Ctx.AddWrite(Slot.Path, MakeFragmentJson(Slot.RawFragment, FragmentNode, Content), /*bInFragment=*/true);
		}
		return Out;
	}

	TSharedPtr<FJsonObject> CollapseNode(FCollapseContext& Ctx, const TSharedPtr<FJsonObject>& InRawNode, const TSharedPtr<FJsonObject>& InNewNode, const FString& InFromFile)
	{
		const TArray<TSharedPtr<FJsonValue>>* NewChildren = nullptr;
		if (!InNewNode->TryGetArrayField(FieldChildren, NewChildren) || NewChildren == nullptr)
		{
			return InNewNode;
		}
		TArray<TSharedPtr<FJsonValue>> NoRawChildren;
		const TArray<TSharedPtr<FJsonValue>>* RawChildren = nullptr;
		if (!InRawNode.IsValid() || !InRawNode->TryGetArrayField(FieldChildren, RawChildren) || RawChildren == nullptr)
		{
			RawChildren = &NoRawChildren;
		}

		const TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const auto& Pair : InNewNode->Values)
		{
			Out->SetField(ASSETFORGE_JSON_KEY(Pair.Key), Pair.Value);
		}
		Out->SetArrayField(FieldChildren, CollapseList(Ctx, *RawChildren, *NewChildren, InFromFile));
		return Out;
	}

	/** 一个顶层段拆回文件：原来整段 include 的，新内容写回片段，主文件里还是那行 include。 */
	TSharedPtr<FJsonValue> CollapseSection(FCollapseContext& Ctx, const FString& InSection, const TSharedPtr<FJsonValue>& InNew, const TSharedPtr<FJsonValue>& InRaw, const FString& InFromFile)
	{
		FString Ref;
		TArray<FForgeDiagnostic> Ignored;
		if (ReadInclude(InRaw, InSection, Ref, Ignored) == EIncludeForm::Valid)
		{
			FString FragmentPath;
			TSharedPtr<FJsonObject> RawFragment;
			TSharedPtr<FJsonValue> RawContent;
			if (ReadRawFragment(Ctx, Ref, InFromFile, InSection, FragmentPath, RawFragment, RawContent))
			{
				Ctx.Stack.Push(FragmentPath);
				const TSharedPtr<FJsonValue> Content = CollapseSection(Ctx, InSection, InNew, RawContent, FragmentPath);
				Ctx.Stack.Pop();
				Ctx.AddWrite(FragmentPath, MakeFragmentJson(RawFragment, InSection, Content), /*bInFragment=*/true);
				return InRaw;
			}
			if (!Ctx.bCycle)
			{
				Ctx.Diagnostics.Emplace(
					EForgeSeverity::Warning,
					InSection,
					FString::Printf(TEXT("片段 %s 读不了或不是「%s」片段：这一段写回了 %s"), *Ref, *InSection, *FPaths::GetCleanFilename(InFromFile)));
			}
		}

		if (InSection == FieldRoot)
		{
			const TSharedPtr<FJsonObject> NewRoot = ObjectOf(InNew);
			if (NewRoot.IsValid())
			{
				return MakeShared<FJsonValueObject>(CollapseNode(Ctx, ObjectOf(InRaw), NewRoot, InFromFile));
			}
		}
		return InNew;
	}
}

FLoader DiskLoader()
{
	return [](const FString& InPath, TSharedPtr<FJsonObject>& OutJson, FString& OutError)
	{
		return ForgeJsonText::LoadObject(InPath, OutJson, OutError);
	};
}

FString ResolveIncludePath(const FString& InInclude, const FString& InFromFile)
{
	return NormalizePath(FPaths::IsRelative(InInclude) ? FPaths::Combine(FPaths::GetPath(InFromFile), InInclude) : InInclude);
}

bool IsFragment(const TSharedPtr<FJsonObject>& InJson)
{
	return InJson.IsValid() && InJson->HasField(FieldFragment) && !InJson->HasField(FieldKind);
}

bool IsFragmentFile(const FString& InPath)
{
	TSharedPtr<FJsonObject> Json;
	FString Error;
	return ForgeJsonText::LoadObject(InPath, Json, Error) && IsFragment(Json);
}

bool Expand(const TSharedPtr<FJsonObject>& InMain, const FString& InMainPath, const FLoader& InLoader, FExpanded& OutExpanded, TArray<FForgeDiagnostic>& OutDiagnostics)
{
	OutExpanded = FExpanded();
	if (!InMain.IsValid())
	{
		OutDiagnostics.Emplace(EForgeSeverity::Error, FString(), TEXT("配置文件的顶层必须是 JSON 对象"));
		return false;
	}
	if (InMain->HasField(FieldFragment))
	{
		OutDiagnostics.Emplace(
			EForgeSeverity::Error,
			FieldFragment,
			InMain->HasField(FieldKind)
				? TEXT("顶层同时有 kind 和 fragment：资源配置写 kind，被 include 的片段写 fragment，两者只能有一个")
				: TEXT("这是片段文件（fragment），不是资源配置：它的内容由 include 它的那份配置导入，不能单独导入"));
		return false;
	}

	const FString MainPath = NormalizePath(InMainPath);
	FExpandContext Ctx(InLoader, MainPath, OutExpanded, OutDiagnostics);
	Ctx.Stack.Add(MainPath);

	OutExpanded.Json = MakeShared<FJsonObject>();
	bool bOk = true;
	for (const auto& Pair : InMain->Values)
	{
		const FString Key(ASSETFORGE_JSON_KEY(Pair.Key));
		if (IsHeaderField(Key))
		{
			OutExpanded.Json->SetField(Key, Pair.Value);
			continue;
		}
		TSharedPtr<FJsonValue> Value;
		bOk &= ExpandSection(Ctx, Key, Pair.Value, MainPath, Value);
		OutExpanded.Json->SetField(Key, Value.IsValid() ? Value : Pair.Value);
	}
	return bOk;
}

bool LoadFile(const FString& InMainPath, FExpanded& OutExpanded, TArray<FForgeDiagnostic>& OutDiagnostics)
{
	TSharedPtr<FJsonObject> Json;
	FString Error;
	if (!ForgeJsonText::LoadObject(InMainPath, Json, Error))
	{
		OutDiagnostics.Emplace(EForgeSeverity::Error, FString(), Error);
		return false;
	}
	return Expand(Json, InMainPath, DiskLoader(), OutExpanded, OutDiagnostics);
}

bool LoadText(const FString& InText, const FString& InMainPath, FExpanded& OutExpanded, TArray<FForgeDiagnostic>& OutDiagnostics)
{
	TSharedPtr<FJsonObject> Json;
	FString Error;
	if (!ForgeJsonText::ParseObject(ForgeJsonText::StripComments(InText), Json, Error))
	{
		OutDiagnostics.Emplace(EForgeSeverity::Error, FString(), Error);
		return false;
	}
	return Expand(Json, InMainPath, DiskLoader(), OutExpanded, OutDiagnostics);
}

void LabelDiagnostics(const TMap<FString, FString>& InOrigins, TArray<FForgeDiagnostic>& InOutDiagnostics, const int32 InStartIndex)
{
	if (InOrigins.Num() == 0)
	{
		return;
	}
	for (int32 Index = FMath::Max(0, InStartIndex); Index < InOutDiagnostics.Num(); ++Index)
	{
		FForgeDiagnostic& Diagnostic = InOutDiagnostics[Index];
		const FString* BestLabel = nullptr;
		int32 BestLength = -1;
		for (const TPair<FString, FString>& Origin : InOrigins)
		{
			const FString& Prefix = Origin.Key;
			if (Prefix.Len() <= BestLength || !Diagnostic.FieldPath.StartsWith(Prefix, ESearchCase::CaseSensitive))
			{
				continue;
			}
			// `root.children[1]` 不能认成 `root.children[10]` 的前缀：前缀后面只能是结尾、`.` 或 `[`。
			if (Diagnostic.FieldPath.Len() > Prefix.Len())
			{
				const TCHAR Next = Diagnostic.FieldPath[Prefix.Len()];
				if (Next != TEXT('.') && Next != TEXT('['))
				{
					continue;
				}
			}
			BestLabel = &Origin.Value;
			BestLength = Prefix.Len();
		}
		if (BestLabel != nullptr)
		{
			Diagnostic.FieldPath = FString::Printf(TEXT("%s: %s"), **BestLabel, *Diagnostic.FieldPath);
		}
	}
}

bool Collapse(const TSharedPtr<FJsonObject>& InDocument, const FString& InMainPath, const FLoader& InLoader, TArray<FFileWrite>& OutWrites, TArray<FForgeDiagnostic>& OutDiagnostics)
{
	OutWrites.Reset();
	if (!InDocument.IsValid())
	{
		OutDiagnostics.Emplace(EForgeSeverity::Error, FString(), TEXT("待写入的文档为空"));
		return false;
	}

	const FString MainPath = NormalizePath(InMainPath);
	FFileWrite WholeFile;
	WholeFile.Path = MainPath;
	WholeFile.Json = InDocument;

	TSharedPtr<FJsonObject> RawMain;
	FString LoadError;
	if (!InLoader(MainPath, RawMain, LoadError) || !RawMain.IsValid() || IsFragment(RawMain))
	{
		// 新配置或原文件坏了：没有可保留的结构，整份写主文件（和以前一样）。
		OutWrites.Add(MoveTemp(WholeFile));
		return true;
	}

	FCollapseContext Ctx(InLoader, OutWrites, OutDiagnostics);
	Ctx.Stack.Add(MainPath);

	const TSharedPtr<FJsonObject> Main = MakeShared<FJsonObject>();
	for (const auto& Pair : InDocument->Values)
	{
		const FString Key(ASSETFORGE_JSON_KEY(Pair.Key));
		const TSharedPtr<FJsonValue> RawValue = ValueOf(RawMain, Key);
		if (IsHeaderField(Key) || !RawValue.IsValid())
		{
			Main->SetField(Key, Pair.Value);
			continue;
		}
		Main->SetField(Key, CollapseSection(Ctx, Key, Pair.Value, RawValue, MainPath));
	}

	if (Ctx.bCycle)
	{
		OutWrites.Reset();
		OutWrites.Add(MoveTemp(WholeFile));
		OutDiagnostics.Emplace(EForgeSeverity::Warning, FString(), TEXT("片段的 include 成环，没法按片段拆：这次整份写回了主文件，片段文件没动"));
		return true;
	}

	FFileWrite MainWrite;
	MainWrite.Path = MainPath;
	MainWrite.Json = Main;
	OutWrites.Insert(MoveTemp(MainWrite), 0);
	return true;
}

bool WriteIfChanged(const FString& InPath, const TSharedPtr<FJsonObject>& InJson, bool& bOutWritten, FString& OutError)
{
	bOutWritten = false;
	TSharedPtr<FJsonObject> Existing;
	FString LoadError;
	if (ForgeJsonText::LoadObject(InPath, Existing, LoadError)
		&& ForgeJsonText::ToCanonicalString(Existing) == ForgeJsonText::ToCanonicalString(InJson))
	{
		return true;
	}
	if (!ForgeJsonText::SaveObject(InPath, InJson, OutError))
	{
		return false;
	}
	bOutWritten = true;
	return true;
}

} // namespace ForgeDocSource
