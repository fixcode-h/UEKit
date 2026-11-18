// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

/** 能力系统接口（协议第 8.1 节 Ability.*）：模块启动时登记，关闭时按模块名摘掉。 */
namespace GameTestGASApi
{
	void Register();
	void Unregister();
}
