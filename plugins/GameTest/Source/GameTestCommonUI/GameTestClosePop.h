// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** obj.close 的 `by: "pop"`：模块启动时登记进关闭方式注册表，关闭时摘掉。 */
namespace GameTestClosePop
{
	void Register();
	void Unregister();
}
