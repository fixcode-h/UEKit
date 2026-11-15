// Copyright Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"

// 缺这一行时症状极具误导性：模块 DLL 能编出来、能链接，但加载阶段直接失败，
// 于是**整个插件的所有测试都不会出现在结果里**（不会报"测试失败"，什么都不会有）。
IMPLEMENT_MODULE(FDefaultModuleImpl, GameTestTests)
