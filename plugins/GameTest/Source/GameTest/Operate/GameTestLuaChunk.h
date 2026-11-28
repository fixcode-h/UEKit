// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Lua 代码节点（`lua`、`luafn`，2.9.0）拼给 UELink lua.exec 跑的代码。
 *
 * 用户的代码不落成 .lua 文件：原文作为字符串放进外壳，外壳用 load(代码, chunkname, "t", 环境) 编译，
 * 所以报错行号就是节点里的行号；环境表的 __index 指向 _G（读得到项目的模块和全局），写全局只写进环境表，不碰 _G。
 * 函数库（面板展开时写进 lib）同样按函数体编译进 fn 表，跑完整个环境丢掉，不进 package.loaded。
 * 返回值在 Lua 里编成 JSON 字符串带回来，C++ 再解析成脚本的值（lua.exec 的 results 只给显示用的描述）。
 */
namespace GameTestLuaChunk
{
	/** JSON 值 → Lua 字面量（null → nil、数组 → {…}、对象 → {["键"] = 值}）。 */
	FString ToLuaLiteral(const TSharedPtr<FJsonValue>& InValue);

	/** 字符串 → Lua 字符串字面量（转义引号、反斜杠、控制字符）。 */
	FString QuoteLua(const FString& InText);

	/**
	 * 拼外壳代码。
	 * @param InCode       节点里的代码（luafn 时是生成的 `return fn["名字"](参数…)`）
	 * @param InChunkName  chunkname（`=GameTest:#步骤id`），报错里显示它
	 * @param InArgs       参数：名字 → 已求值的 JSON 值，代码里直接当变量用
	 * @param InLib        函数库：名字 → {params: [名字…], body: 函数体}
	 */
	FString BuildWrapper(const FString& InCode, const FString& InChunkName, const TMap<FString, TSharedPtr<FJsonValue>>& InArgs, const TSharedPtr<FJsonObject>& InLib);

	/** luafn：按函数库里的参数表生成调用代码。函数不在 lib 里返回空，并写原因。 */
	FString BuildFnCall(const FString& InName, const TSharedPtr<FJsonObject>& InLib, FString& OutError);
}
