// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

/**
 * 录到的步骤 → schemaVersion 4（docs/game-test-ref.md 第 8、9 节）。
 *
 * 录制器内部仍用选择器文本记目标（挑标识、问人、改写方括号都按它做，录制期间不改），存盘前在这里统一改写：
 * - `target` / `toTarget` / `widget` → 部件写法 `{obj, part}`；屏 → 引用 `{class}`，去重写进 `objects`（名字按类名去前缀），步骤里写名字；
 *   容器条目 → `{class: 条目类, in: {obj, part: 容器}, ident: [{prop, op: "eq", value}]}`（标识值各不相同，内联不去重）。
 * - 断言、等待、skipIf 的旧判断（`visible` / `text` / `checked`…）→ `cond: {obj, part, prop, op, value}`。
 * 改写不准的地方（按序号认、屏名不确定、纯 Slate）在步骤上写 `todo: "说明"`，引用写 `many: "first"`（和面板迁移同一规则）。
 */
namespace GameTestRecordV4
{
	/**
	 * 改写一组步骤（先深拷贝，录制器手里的原步骤不动）。`InOutObjects` 是场景已有的 `objects`（接着录时），新引用加进去。
	 * 返回写了 todo 的步骤数。
	 */
	GAMETEST_API int32 Convert(TArray<TSharedPtr<FJsonValue>>& InOutSteps, FJsonObject& InOutObjects);

	/**
	 * 一个选择器 → 部件写法（单独用：录制暂停问标识时给面板看）。`OutTodo` 非空表示改写不准。
	 * `bInInline`：屏引用直接内联 `{class}`，不进 objects（录制时当场核对用，没有场景的 objects）。
	 */
	GAMETEST_API TSharedPtr<FJsonObject> TargetOf(const FString& InSelector, const TSharedPtr<FJsonObject>& InKeyHint, FJsonObject& InOutObjects, FString& OutTodo, bool bInInline = false);

	/** 录制挑的跳过条件（旧判断写法）→ v4 的 skipIf `{cond}`，引用内联：录制时用回放同一套判断当场核对。改写不了返回空。 */
	GAMETEST_API TSharedPtr<FJsonObject> SkipIfForCheck(const FJsonObject& InOldSkipIf, const TSharedPtr<FJsonObject>& InKeyHint);
}
