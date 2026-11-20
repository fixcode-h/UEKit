// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Script/GameTestBuiltins.h"

/**
 * 每个实现文件里定义的登记函数（`namespace GameTestBuiltin { void Register…(); }`）。
 * 加一个实现文件就在下面对应的段里各加一行声明和一行调用；段内按文档顺序排，
 * 这个顺序就是 `gt.actions` / `gt.api` / 面板节点面板里的顺序。
 */
namespace GameTestBuiltin
{
	//~ 块（Core/Flow/Blocks）
	void RegisterBlockGroup();
	void RegisterBlockIf();
	void RegisterBlockLoop();
	void RegisterBlockCall();
	void RegisterBlockSetVar();
	void RegisterBlockSignals();
	//~ 判断（Observe/Checks）
	void RegisterCheckObj();
	void RegisterCheckExpr();
	void RegisterCheckApi();
	void RegisterCheckAlias();
	void RegisterCheckActorNear();
	void RegisterCheckValue();
	//~ 算出状态（Observe/Ref/States）
	void RegisterStateExists();
	void RegisterStateOnScreen();
	void RegisterStateShown();
	void RegisterStateEnabled();
	void RegisterStateText();
	void RegisterStateTextKey();
	void RegisterStateCount();
	void RegisterStateIndex();
	void RegisterStateSelected();
	void RegisterStateChecked();
	void RegisterStateImage();
	void RegisterStateHovered();
	void RegisterStateFocused();
	void RegisterStatePos();
	void RegisterStateSize();
	void RegisterStateLocation();
	void RegisterStateDistance();
	void RegisterStateHiddenInGame();
	void RegisterStateClass();
	//~ 比较（Observe/Compare）
	void RegisterCompareEquality();
	void RegisterCompareOrder();
	void RegisterCompareMembership();
	void RegisterCompareText();
	void RegisterCompareVisibility();
	void RegisterCompareObject();
	void RegisterCompareNear();
	void RegisterCompareArray();

	// 关闭方式（obj.close 的 by，Operate/Close/）；pop 在 GameTestCommonUI
	void RegisterCloseCall();
	void RegisterCloseLua();
	void RegisterCloseClick();
	void RegisterCloseVisibility();
	void RegisterCloseRemove();
	void RegisterCloseKey();
	void RegisterCloseDestroy();
	//~ 取值来源与成员访问（Observe/Sources）
	void RegisterSourcePlayer();
	void RegisterSourceSubsystem();
	void RegisterSourceActor();
	void RegisterSourceObject();
	void RegisterSourceLua();
	void RegisterSourceCVar();
	void RegisterMemberReflect();
	void RegisterMemberLua();
	//~ 表达式函数（Core/Expr/Functions）
	void RegisterExprBasic();
	void RegisterExprProp();
	//~ 深度检查（Core/Flow/Deep）
	void RegisterDeepChecks();
	//~ 接口（Api/Builtin）
	void RegisterApiObject();
	void RegisterApiWidget();
	void RegisterApiPlayer();
	void RegisterApiWorld();
	void RegisterApiGlobal();
	void RegisterApiLua();
	void RegisterApiLog();
	void RegisterApiWait();
	void RegisterApiRuntime();
	void RegisterApiProject();
}

void GameTestBuiltins::EnsureRegistered()
{
	// 先置位再登记：登记函数里会取注册表，注册表的 Get() 又会调到这里，不置位就无限递归。
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}
	bRegistered = true;

	//~ 块
	GameTestBuiltin::RegisterBlockGroup();
	GameTestBuiltin::RegisterBlockIf();
	GameTestBuiltin::RegisterBlockLoop();
	GameTestBuiltin::RegisterBlockCall();
	GameTestBuiltin::RegisterBlockSetVar();
	GameTestBuiltin::RegisterBlockSignals();

	//~ 判断（更具体的先登记：FindFor 按登记顺序取第一个认的；value 认「只写了比较」，放最后兜底）
	GameTestBuiltin::RegisterCheckObj();
	GameTestBuiltin::RegisterCheckExpr();
	GameTestBuiltin::RegisterCheckApi();
	GameTestBuiltin::RegisterCheckAlias();
	GameTestBuiltin::RegisterCheckActorNear();
	GameTestBuiltin::RegisterCheckValue();

	//~ 算出状态（顺序即 gt.props 状态组的顺序）
	GameTestBuiltin::RegisterStateExists();
	GameTestBuiltin::RegisterStateOnScreen();
	GameTestBuiltin::RegisterStateShown();
	GameTestBuiltin::RegisterStateEnabled();
	GameTestBuiltin::RegisterStateText();
	GameTestBuiltin::RegisterStateTextKey();
	GameTestBuiltin::RegisterStateCount();
	GameTestBuiltin::RegisterStateIndex();
	GameTestBuiltin::RegisterStateSelected();
	GameTestBuiltin::RegisterStateChecked();
	GameTestBuiltin::RegisterStateImage();
	GameTestBuiltin::RegisterStateHovered();
	GameTestBuiltin::RegisterStateFocused();
	GameTestBuiltin::RegisterStatePos();
	GameTestBuiltin::RegisterStateSize();
	GameTestBuiltin::RegisterStateLocation();
	GameTestBuiltin::RegisterStateDistance();
	GameTestBuiltin::RegisterStateHiddenInGame();
	GameTestBuiltin::RegisterStateClass();

	//~ 比较（顺序即协议 3.4 表格的顺序，gt.props 的 ops 按它排）
	GameTestBuiltin::RegisterCompareEquality();
	GameTestBuiltin::RegisterCompareOrder();
	GameTestBuiltin::RegisterCompareMembership();
	GameTestBuiltin::RegisterCompareText();
	GameTestBuiltin::RegisterCompareVisibility();
	GameTestBuiltin::RegisterCompareObject();
	GameTestBuiltin::RegisterCompareNear();
	GameTestBuiltin::RegisterCompareArray();
	GameTestBuiltin::RegisterCloseCall();
	GameTestBuiltin::RegisterCloseLua();
	GameTestBuiltin::RegisterCloseClick();
	GameTestBuiltin::RegisterCloseVisibility();
	GameTestBuiltin::RegisterCloseRemove();
	GameTestBuiltin::RegisterCloseKey();
	GameTestBuiltin::RegisterCloseDestroy();

	//~ 取值来源与成员访问
	GameTestBuiltin::RegisterSourcePlayer();
	GameTestBuiltin::RegisterSourceSubsystem();
	GameTestBuiltin::RegisterSourceActor();
	GameTestBuiltin::RegisterSourceObject();
	GameTestBuiltin::RegisterSourceLua();
	GameTestBuiltin::RegisterSourceCVar();
	GameTestBuiltin::RegisterMemberReflect();
	GameTestBuiltin::RegisterMemberLua();

	//~ 表达式函数
	GameTestBuiltin::RegisterExprBasic();
	GameTestBuiltin::RegisterExprProp();

	//~ 接口
	GameTestBuiltin::RegisterApiObject();
	GameTestBuiltin::RegisterApiWidget();
	GameTestBuiltin::RegisterApiPlayer();
	GameTestBuiltin::RegisterApiWorld();
	GameTestBuiltin::RegisterApiGlobal();
	GameTestBuiltin::RegisterApiLua();
	GameTestBuiltin::RegisterApiLog();
	GameTestBuiltin::RegisterApiWait();
	GameTestBuiltin::RegisterApiRuntime();
	GameTestBuiltin::RegisterApiProject();

	//~ 深度检查
	GameTestBuiltin::RegisterDeepChecks();
}
