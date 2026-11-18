// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class FJsonObject;
class UWorld;

/**
 * 世界侧的最小观察：在游戏世界里按 `class` 或 `tag` 选一个 actor。
 *
 * **为什么只做最小集**（位置/朝向/速度），不做通用属性 dump：世界侧属性面巨大，`UObject` 的反射
 * dump 会一次带出几百个字段，而"走没走、朝哪、多快"这三样才是玩法验证真正要判的。要读别的属性
 * 再按需加，不要一次铺开。
 *
 * 选择语义与控件选择器同精神：**唯一命中**——0 个或超过 1 个都失败并给候选，不做"取第一个"。
 * 不引入第二套 `@句柄` 语义（控件句柄表是 `TWeakObjectPtr<UWidget>`，混用会让 `@name` 变含糊）。
 */
class GAMETEST_API GameTestActor
{
public:
	/**
	 * 类名匹配（沿继承链）：反射名会剥掉 `A`/`U` 前缀（`ATank` 的类名是 `Tank`），
	 * 两种写法都接受——场景作者写的是 C++ 类型名，世界里站着的常是蓝图实例。
	 */
	static bool MatchesClass(const AActor& InActor, const FString& InClassName);

	/**
	 * 按类名（沿继承链匹配）或标签选一个 actor；两者至少给一个。
	 * `InLocallyControlled` 有值时再筛"是不是本端玩家控制的那台"——多客户端会话里，
	 * 同一类 actor 在每端世界里各有本地/远端两份，这个筛子是把它们分开的唯一稳定判据。
	 */
	static AActor* ResolveOne(UWorld& InWorld, const FString& InClassName, const FString& InTag, const TOptional<bool>& InLocallyControlled, FString& OutError);

	/** actor → 对象路径、位置、朝向（Yaw 及完整 Rotator）、速度（大小与向量）。 */
	static TSharedPtr<FJsonObject> ToJson(const AActor& InActor);
};
