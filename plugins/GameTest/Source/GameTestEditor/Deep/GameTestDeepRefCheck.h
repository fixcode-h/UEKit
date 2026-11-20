// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

/** 对象引用的深度检查（读 WBP 资产的控件树和类的反射，不用开游戏）：编辑器模块启动时登记。 */
namespace GameTestDeepRefCheck
{
	void Register();
	void Unregister();
}
