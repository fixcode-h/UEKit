// Copyright Epic Games, Inc. All Rights Reserved.

#include "Registry/ForgeKindSchema.h"

#include "Core/ForgeDoc.h"
#include "Registry/ForgeKindRegistry.h"

#include "Kinds/Actor/ActorBlueprintForge.h"
#include "Kinds/Anim/AnimBlueprintForge.h"
#include "Kinds/Anim/AnimSequenceForge.h"
#include "Kinds/BehaviorTree/BehaviorTreeForge.h"
#include "Kinds/Data/DataAssetForge.h"
#include "Kinds/Font/FontForge.h"
#include "Kinds/Map/MapForge.h"
#include "Kinds/Material/MaterialForge.h"
#include "Kinds/Niagara/NiagaraForge.h"
#include "Kinds/Object/ObjectBlueprintForge.h"
#include "Kinds/Physics/PhysicsAssetForge.h"
#include "Kinds/SkeletalMesh/SkeletalMeshForge.h"
#include "Kinds/Skeleton/SkeletonForge.h"
#if ASSETFORGE_WITH_STATETREE
#include "Kinds/StateTree/StateTreeForge.h"
#endif
#include "Kinds/Table/DataTableForge.h"
#include "Kinds/Texture/TextureForge.h"
#include "Kinds/Umg/UmgForge.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ForgeKindSchema
{
namespace
{
	/** 节点或文档上的一个扩展字段（管道不认识、由各 kind 自己解释的那些）。 */
	struct FField
	{
		const TCHAR* Name;
		/** JSON Schema 的 type：string / number / boolean / array / object。 */
		const TCHAR* Type;
		const TCHAR* Description;
	};

	/**
	 * 一种 kind 的配置形状。照着各 kind 头文件开头的「配置形状」注释抄：那里是真实导入代码的说明，
	 * 改导入时两边一起改。
	 */
	struct FShape
	{
		const TCHAR* Kind = nullptr;
		/** `root.class` 固定取值（空 = 由 RootClassHint 说明怎么写）。 */
		const TCHAR* RootClass = nullptr;
		const TCHAR* RootClassHint = nullptr;
		/** `root.props` 的属性来自哪个类（固定时客户端可直接取属性表；空 = 看 PropsHint）。 */
		const TCHAR* PropsClass = nullptr;
		const TCHAR* PropsHint = nullptr;
		/** 子节点说明；空 = `children` 必须为空。 */
		const TCHAR* ChildrenHint = nullptr;
		/** `parent` 说明；空 = 这个 kind 不用 `parent`。 */
		const TCHAR* ParentHint = nullptr;
		bool bParentRequired = false;
		/** 文档顶层的扩展字段。 */
		TArray<FField> DocFields;
		/** 节点（根和子节点）上的扩展字段。 */
		TArray<FField> NodeFields;
		/** 写配置时最容易踩的坑，一两句。 */
		const TCHAR* Notes = nullptr;
	};

	const TCHAR* const PropsByClass = TEXT("属性名用 UE 属性名原样（只写想改的，结构体可只写部分字段）；属性表用 forge.kinds class=<类> 取");

	const TArray<FShape>& Shapes()
	{
		static const TArray<FShape> Table = []()
		{
			TArray<FShape> Out;

			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = UmgForge::Kind;
				S.RootClassHint = TEXT("根控件类：短名按 /Script/UMG.、/Script/CommonUI. 解析（CanvasPanel），也可写全路径；嵌套别的 WBP 时用 ref 指向那份 umg 配置、省掉 class");
				S.PropsHint = PropsByClass;
				S.ChildrenHint = TEXT("子控件，结构同根节点；按 name 匹配（缺的建、有的改、多的删、顺序变的重排），整棵树里 name 不能重名；slot 写父面板槽的属性（CanvasPanelSlot 的 LayoutData 等）");
				S.ParentHint = TEXT("控件蓝图父类，缺省 /Script/UMG.UserWidget");
				S.DocFields = {{TEXT("animations"), TEXT("array"), TEXT("控件动画；没写等于资产里不该有动画")}};
				S.Notes = TEXT("先 plan 演练再 import；属性和默认值相同时会被归一化删掉");
			}
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = DataAssetForge::Kind;
				S.RootClassHint = TEXT("资产的类（UDataAsset 派生，全路径 /Script/<模块>.<类> 或蓝图类），新建资产也用它");
				S.PropsHint = TEXT("资产的全部可配置属性（取属性表：forge.kinds class=<root.class>）；没写的属性导入时重置成默认值");
			}
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = DataTableForge::Kind;
				S.RootClass = DataTableForge::DataTableClassPath;
				S.PropsHint = TEXT("只放 rowStruct：行结构体路径（/Script/<模块>.<结构体名，不带 F>）");
				S.ChildrenHint = TEXT("每行一个节点：class 写 Row（占位，不解析），name 是 RowName，props 是这一行的数据（字段表：forge.kinds class=<rowStruct>）；按 RowName 匹配");
			}
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = ActorBlueprintForge::Kind;
				S.RootClass = ActorBlueprintForge::ComponentRootClass;
				S.PropsHint = TEXT("占位根，不写 props");
				S.ChildrenHint = TEXT("组件树：class 是组件类全路径（/Script/Engine.StaticMeshComponent），name 是组件变量名，props 是组件模板属性（forge.kinds class=<组件类>），children 是挂在它下面的组件；按 name 匹配");
				S.ParentHint = TEXT("蓝图父类（C++ 类或另一个蓝图），必须是 Actor 派生");
				S.bParentRequired = true;
				S.Notes = TEXT("只管组件树和父类，事件图表、变量、函数不进配置");
			}
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = BehaviorTreeForge::Kind;
				S.RootClassHint = TEXT("根节点类，必须是 composite（短名按 /Script/AIModule. 解析，如 BTComposite_Selector）");
				S.PropsHint = TEXT("节点属性（forge.kinds class=<节点类>）");
				S.ChildrenHint = TEXT("子节点（composite 或 task），按 name（NodeName）和位置匹配，同级可以重名");
				S.DocFields = {{TEXT("blackboard"), TEXT("string"), TEXT("黑板资产路径")}};
				S.NodeFields = {
					{TEXT("decorators"), TEXT("array"), TEXT("挂在这个节点上的装饰器：[{class, name, props}]")},
					{TEXT("services"), TEXT("array"), TEXT("这个节点上的服务：[{class, name, props}]")}};
			}
#if ASSETFORGE_WITH_STATETREE
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = StateTreeForge::Kind;
				S.RootClassHint = TEXT("根状态（照导出的写）");
				S.PropsHint = TEXT("状态没有 props，字段写在节点的扩展字段里");
				S.ChildrenHint = TEXT("子状态，按 name 匹配（改名 = 删旧建新）");
				S.DocFields = {
					{TEXT("schema"), TEXT("string"), TEXT("状态树 schema 类")},
					{TEXT("parameters"), TEXT("object"), TEXT("全局参数")},
					{TEXT("evaluators"), TEXT("array"), TEXT("评估器：[{type, name, props}]")},
					{TEXT("globalTasks"), TEXT("array"), TEXT("全局任务：[{type, name, props}]")},
					{TEXT("bindings"), TEXT("array"), TEXT("属性绑定：[{source, target}]，引用写 StatePath/Name.属性")}};
				S.NodeFields = {
					{TEXT("type"), TEXT("string"), TEXT("状态类型")},
					{TEXT("selection"), TEXT("string"), TEXT("子状态选择方式")},
					{TEXT("tasks"), TEXT("array"), TEXT("任务：[{type, name, props}]")},
					{TEXT("enterConditions"), TEXT("array"), TEXT("进入条件：[{type, name, props}]")},
					{TEXT("considerations"), TEXT("array"), TEXT("考量：[{type, name, props}]")},
					{TEXT("transitions"), TEXT("array"), TEXT("转移：[{trigger, to, event?, priority?, delay?, conditions?}]")}};
				S.Notes = TEXT("字段多，最好先导出一份现有状态树照着改；绑定和参数每次导入全量重建");
			}
#endif
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = AnimBlueprintForge::Kind;
				S.RootClass = AnimBlueprintForge::AnimGraphRootClass;
				S.PropsHint = TEXT("占位根，不写 props");
				S.ChildrenHint = TEXT("状态机：class = StateMachine，name = 状态机图名，children 是状态（class = State，props.sequence / loop / playRate），extra 里 entry 写入口状态、transitions 写转移");
				S.ParentHint = TEXT("动画实例父类，缺省 /Script/Engine.AnimInstance");
				S.DocFields = {{TEXT("skeleton"), TEXT("string"), TEXT("目标骨架资产路径（必写，没有它编译不过）")}};
				S.NodeFields = {
					{TEXT("entry"), TEXT("string"), TEXT("状态机节点：入口状态名，缺省取第一个状态")},
					{TEXT("transitions"), TEXT("array"), TEXT("状态机节点：[{from, to, crossfade?, autoRule?}]")}};
				S.Notes = TEXT("只管状态机，混合、IK 等别的节点不进配置");
			}
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = AnimSequenceForge::Kind;
				S.RootClassHint = TEXT("资产的类（AnimSequence / AnimMontage），照导出的写");
				S.PropsHint = TEXT("只写要改的字段，没写的保持资产原值（forge.kinds class=<root.class>）");
				S.Notes = TEXT("纳管型：资产必须已存在（先由 FBX 导入），配置只做微调");
			}
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = PhysicsAssetForge::Kind;
				S.RootClass = TEXT("/Script/Engine.PhysicsAsset");
				S.PropsClass = TEXT("/Script/Engine.PhysicsAsset");
				S.PropsHint = TEXT("只写要改的字段，没写的保持资产原值");
				S.Notes = TEXT("纳管型：资产必须已存在，碰撞体增删不进配置");
			}
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = SkeletonForge::Kind;
				S.RootClass = TEXT("/Script/Engine.Skeleton");
				S.PropsClass = TEXT("/Script/Engine.Skeleton");
				S.PropsHint = TEXT("只写要改的字段，没写的保持资产原值");
				S.Notes = TEXT("纳管型：资产必须已存在，插槽增删不进配置");
			}
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = SkeletalMeshForge::Kind;
				S.RootClass = TEXT("/Script/Engine.SkeletalMesh");
				S.PropsClass = TEXT("/Script/Engine.SkeletalMesh");
				S.PropsHint = TEXT("只写要改的字段，没写的保持资产原值");
				S.Notes = TEXT("纳管型：资产必须已存在，SourceModels、Sockets 不进配置");
			}
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = MaterialForge::Kind;
				S.RootClass = TEXT("Material");
				S.PropsClass = TEXT("/Script/Engine.Material");
				S.PropsHint = TEXT("材质自身的属性（BlendMode、TwoSided……）");
				S.NodeFields = {
					{TEXT("inputs"), TEXT("object"), TEXT("根上：材质属性输入 → 节点名，或 {from, output}；节点上：节点输入 → 来源")},
					{TEXT("nodes"), TEXT("array"), TEXT("根上：表达式节点 [{name, class（VectorParameter、TextureSample……）, props, inputs?}]")}};
				S.Notes = TEXT("节点坐标不进配置，导入后自动排版");
			}
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = MapForge::Kind;
				S.RootClassHint = TEXT("世界设置的类：/Script/Engine.WorldSettings 或地图实际用的派生类");
				S.PropsClass = TEXT("/Script/Engine.WorldSettings");
				S.PropsHint = TEXT("世界设置的属性；没写的导入时重置成默认值");
				S.Notes = TEXT("只管世界设置，关卡里摆的东西不进配置；root.name 固定 WorldSettings");
			}
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = ObjectBlueprintForge::Kind;
				S.RootClass = ObjectBlueprintForge::ObjectRootClass;
				S.PropsHint = TEXT("类默认值（CDO）上的可编辑属性，相对父类（forge.kinds class=<parent>）");
				S.ParentHint = TEXT("父类：非 Actor 派生、可蓝图化、非抽象（如 GameplayAbility）");
				S.bParentRequired = true;
			}
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = NiagaraForge::Kind;
				S.RootClass = NiagaraForge::SystemRootClass;
				S.PropsHint = TEXT("系统层用户参数的默认值，键是参数全名（User.SpriteSize）");
				S.ChildrenHint = TEXT("发射器：class 是模板 Emitter 的资产路径（复制源），name 是发射器名（匹配键），props 必须为空；换模板要换名字");
				S.Notes = TEXT("不改模块图，发射器的形状只能来自模板");
			}
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = TextureForge::Kind;
				S.RootClass = TEXT("Texture2D");
				S.PropsClass = TEXT("/Script/Engine.Texture2D");
				S.PropsHint = TEXT("导入设置（CompressionSettings、SRGB、LODGroup……）");
				S.NodeFields = {{TEXT("source"), TEXT("string"), TEXT("根上：源图路径（相对项目根或绝对路径），新纹理必写")}};
			}
			{
				FShape& S = Out.AddDefaulted_GetRef();
				S.Kind = FontForge::Kind;
				S.RootClass = TEXT("Font");
				S.PropsClass = TEXT("/Script/Engine.Font");
				S.PropsHint = TEXT("UFont 的属性，FontCacheType 必须显式写 Runtime");
				S.ChildrenHint = TEXT("字体面：class = FontFace，name 是 FontFace 资产名（和字体同目录），props 是 UFontFace 属性；第一个约定放 Regular");
				S.NodeFields = {
					{TEXT("source"), TEXT("string"), TEXT("字体面：源 ttf 路径（相对项目根），新面必写")},
					{TEXT("typeface"), TEXT("string"), TEXT("字体面：在 UFont 里的 typeface 名（Regular、Bold……），全局唯一")}};
			}

			return Out;
		}();
		return Table;
	}

	const FShape* FindShape(const FString& InKind)
	{
		return Shapes().FindByPredicate([&InKind](const FShape& S) { return InKind == S.Kind; });
	}

	TSharedRef<FJsonObject> Prop(const TCHAR* InType, const FString& InDescription)
	{
		const TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), InType);
		if (!InDescription.IsEmpty())
		{
			P->SetStringField(TEXT("description"), InDescription);
		}
		return P;
	}

	TSharedRef<FJsonObject> ConstProp(const FString& InValue, const FString& InDescription)
	{
		const TSharedRef<FJsonObject> P = Prop(TEXT("string"), InDescription);
		P->SetStringField(TEXT("const"), InValue);
		return P;
	}

	TSharedRef<FJsonObject> Ref(const TCHAR* InDef)
	{
		const TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("$ref"), FString::Printf(TEXT("#/$defs/%s"), InDef));
		return P;
	}

	void AddFields(const TSharedRef<FJsonObject>& InProps, const TArray<FField>& InFields)
	{
		for (const FField& F : InFields)
		{
			InProps->SetObjectField(F.Name, Prop(F.Type, F.Description));
		}
	}

	TArray<TSharedPtr<FJsonValue>> Names(std::initializer_list<const TCHAR*> InNames)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const TCHAR* N : InNames)
		{
			Out.Add(MakeShared<FJsonValueString>(N));
		}
		return Out;
	}

	/** 节点 schema：根和子节点结构一样，区别只在 class 约束、children 许不许有。 */
	TSharedRef<FJsonObject> NodeSchema(const FShape* InShape, const bool bRoot)
	{
		const TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("type"), TEXT("object"));
		const TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();

		if (bRoot && InShape != nullptr && InShape->RootClass != nullptr)
		{
			Props->SetObjectField(TEXT("class"), ConstProp(InShape->RootClass, TEXT("固定取值")));
		}
		else
		{
			const TCHAR* Hint = bRoot && InShape != nullptr ? InShape->RootClassHint : nullptr;
			Props->SetObjectField(TEXT("class"), Prop(TEXT("string"), Hint != nullptr ? Hint : TEXT("节点的类")));
		}
		Props->SetObjectField(TEXT("name"), Prop(TEXT("string"), bRoot ? TEXT("根节点名（一般和资产同名，仅作对照）") : TEXT("节点名")));
		Props->SetObjectField(TEXT("ref"), Prop(TEXT("string"), TEXT("引用另一份配置（嵌套资产时用），给了可以省 class")));

		FString PropsHint = InShape != nullptr && InShape->PropsHint != nullptr ? InShape->PropsHint : PropsByClass;
		if (!bRoot)
		{
			PropsHint = PropsByClass;
		}
		Props->SetObjectField(TEXT("props"), Prop(TEXT("object"), PropsHint));

		const bool bChildren = InShape == nullptr || InShape->ChildrenHint != nullptr;
		if (bChildren)
		{
			Props->SetObjectField(TEXT("slot"), Prop(TEXT("object"), TEXT("父容器槽的属性（UMG 用）")));
			const TSharedRef<FJsonObject> Children = Prop(
				TEXT("array"), InShape != nullptr && InShape->ChildrenHint != nullptr ? InShape->ChildrenHint : TEXT("子节点"));
			Children->SetObjectField(TEXT("items"), Ref(TEXT("node")));
			Props->SetObjectField(TEXT("children"), Children);
		}
		else if (bRoot)
		{
			const TSharedRef<FJsonObject> Children = Prop(TEXT("array"), TEXT("这个 kind 没有子节点，必须为空"));
			Children->SetNumberField(TEXT("maxItems"), 0);
			Props->SetObjectField(TEXT("children"), Children);
		}

		if (InShape != nullptr)
		{
			AddFields(Props, InShape->NodeFields);
		}
		Node->SetObjectField(TEXT("properties"), Props);
		Node->SetArrayField(TEXT("required"), Names({TEXT("name")}));
		return Node;
	}
}

bool HasShape(const FString& InKind)
{
	return FindShape(InKind) != nullptr;
}

TSharedPtr<FJsonObject> Build(const FString& InKind)
{
	const ForgeKindRegistry::FForgeKindInfo* Info = ForgeKindRegistry::Find(InKind);
	if (Info == nullptr)
	{
		return nullptr;
	}
	const FShape* Shape = FindShape(InKind);

	const TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("$schema"), TEXT("https://json-schema.org/draft/2020-12/schema"));
	Schema->SetStringField(TEXT("title"), FString::Printf(TEXT("AssetForge %s 配置"), Info->DisplayName));
	Schema->SetStringField(
		TEXT("description"),
		FString::Printf(TEXT("放在配置根的 %s/ 下，路径和资产路径对应（/Game/A/B ↔ %s/A/B.json）。允许 // 注释。"),
			Info->ConfigDirName, Info->ConfigDirName));
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	const TSharedRef<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetObjectField(TEXT("kind"), ConstProp(Info->Kind, TEXT("")));
	{
		const TSharedRef<FJsonObject> Version = Prop(TEXT("integer"), TEXT("配置格式版本"));
		Version->SetNumberField(TEXT("const"), ForgeDoc::CurrentSchemaVersion);
		Props->SetObjectField(TEXT("schemaVersion"), Version);
	}
	Props->SetObjectField(TEXT("revision"), Prop(TEXT("integer"), TEXT("改一次 +1，新配置写 0")));
	{
		const TSharedRef<FJsonObject> Asset = Prop(TEXT("string"), TEXT("目标资产路径"));
		Asset->SetStringField(TEXT("pattern"), TEXT("^/"));
		Props->SetObjectField(TEXT("asset"), Asset);
	}
	if (Shape == nullptr || Shape->ParentHint != nullptr)
	{
		Props->SetObjectField(TEXT("parent"), Prop(TEXT("string"), Shape != nullptr ? Shape->ParentHint : TEXT("父类")));
	}
	Props->SetObjectField(TEXT("root"), Ref(TEXT("root")));
	if (Shape != nullptr)
	{
		AddFields(Props, Shape->DocFields);
	}
	Schema->SetObjectField(TEXT("properties"), Props);

	TArray<TSharedPtr<FJsonValue>> Required = Names({TEXT("kind"), TEXT("schemaVersion"), TEXT("asset"), TEXT("root")});
	if (Shape != nullptr && Shape->bParentRequired)
	{
		Required.Add(MakeShared<FJsonValueString>(TEXT("parent")));
	}
	Schema->SetArrayField(TEXT("required"), Required);

	const TSharedRef<FJsonObject> Defs = MakeShared<FJsonObject>();
	Defs->SetObjectField(TEXT("root"), NodeSchema(Shape, /*bRoot=*/true));
	if (Shape == nullptr || Shape->ChildrenHint != nullptr)
	{
		Defs->SetObjectField(TEXT("node"), NodeSchema(Shape, /*bRoot=*/false));
	}
	Schema->SetObjectField(TEXT("$defs"), Defs);

	// x-forge：不是 JSON Schema 的标准字段，给写配置的人（和 Agent）的提示
	const TSharedRef<FJsonObject> Hints = MakeShared<FJsonObject>();
	if (Shape != nullptr && Shape->PropsClass != nullptr)
	{
		Hints->SetStringField(TEXT("propsClass"), Shape->PropsClass);
	}
	if (Shape != nullptr && Shape->Notes != nullptr)
	{
		Hints->SetStringField(TEXT("notes"), Shape->Notes);
	}
	Hints->SetStringField(TEXT("workflow"), TEXT("写完先 forge.plan（演练）看诊断和动作，再 forge.import"));
	Schema->SetObjectField(TEXT("x-forge"), Hints);
	return Schema;
}
}
