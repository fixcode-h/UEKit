// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FGameTestFlowContext;
class FJsonObject;
class FJsonValue;
class UObject;
class UWidget;

/**
 * 对象引用的 `/rpc` 操作（docs/game-test-ref.md 第 7 节）：gt.tree、gt.highlight、gt.props、gt.refFor、gt.classes，以及 gt.resolve 的 refs。
 * 一个操作一个文件（Integration/Ref/）。
 */
namespace GameTestRpcRef
{
	bool Tree(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	bool Highlight(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	bool Props(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	bool RefFor(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	/** gt.classes：类树（UE 类或 Lua 类），面板选对象的类下拉用。 */
	bool Classes(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);

	/** gt.resolve 带了 refs。 */
	bool HasRefs(const TSharedPtr<FJsonObject>& InArgs);
	/** gt.resolve 的 refs：`[{id?, ref, show?}]` → `[{id, count, candidates: [{name, ident: [{prop, actual}]}]}]`。 */
	bool ResolveRefs(const TSharedPtr<FJsonObject>& InArgs, TSharedPtr<FJsonObject>& OutResult, FString& OutError);

	/** 停掉描边（模块关闭时）。 */
	void Shutdown();

	//~ 共用 --------------------------------------------------------------------

	/** gt.tree 的临时 id → 控件（这次会话里最近一次 gt.tree 编的号）；没有或控件没了返回空。 */
	UWidget* FindNode(const FString& InId);

	/** 控件在最近一次 gt.tree 里的 id；没编过号返回空串。 */
	FString NodeIdOf(const UWidget* InWidget);

	/** 读 RPC 用的临时脚本上下文：游戏世界、句柄表，引用只能写内联的（名字、$var 没有场景可查）。 */
	void MakeContext(FGameTestFlowContext& OutContext);

	/** `node` 或 `target`（第 2 节写法）→ 对象（target 时是 part 落到的控件或组件）。 */
	UObject* ObjectFromArgs(const FJsonObject& InArgs, FGameTestFlowContext& InContext, FString& OutError);

	/** gt.props 的 `class: "lua:模块"`：Lua 类（模块表）的字段和函数，路径写 `@#字段`。 */
	bool PropsLua(const FString& InModule, TSharedPtr<FJsonObject>& OutResult, FString& OutError);

	/** 从游戏里点到的控件出 `{chain, ref, target?, matches, ask}`（gt.refFor、gt.pick at、gt.inspect、录制问标识共用）。 */
	TSharedRef<FJsonObject> RefJsonFor(UWidget* InWidget, FGameTestFlowContext& InContext, int32 InLevel = 0);
}
