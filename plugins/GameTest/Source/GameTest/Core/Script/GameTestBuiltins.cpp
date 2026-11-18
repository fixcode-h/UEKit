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
	//~ 判断（Observe/Checks）
	//~ 取值来源与成员访问（Observe/Sources）
	//~ 表达式函数（Core/Expr/Functions）
	//~ 接口（Api/Builtin）
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

	//~ 判断（更具体的先登记：FindFor 按登记顺序取第一个认的）

	//~ 取值来源与成员访问

	//~ 表达式函数

	//~ 接口
}
