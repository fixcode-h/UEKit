// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class AActor;
class UClass;
class UObject;
class UWorld;

/**
 * 取值、调函数、对象定位共用的「从世界里拿到一个对象」（协议第 4.3、7 节）：
 * 玩家相关的单例（@gi @gs @gm @ps @pc @pawn @hud）、子系统（@sub.类名）、按标签 / 类 / 名字唯一命中的 Actor。
 *
 * 只认 UObject，不碰值的表示：取值来源、接口、定位器拿到对象后各自往下读。
 */
class GAMETEST_API GameTestObjectRoots
{
public:
	/**
	 * 类名匹配，子类也算：`BP_Npc_C` / `BP_Npc`、`ANpc` / `Npc`、类路径 `/Game/…/BP_Npc.BP_Npc_C` 都认，忽略大小写。
	 */
	static bool ClassMatches(const UClass* InClass, const FString& InName);

	/** 按类名或路径找已加载的类；找不到返回空（不主动加载资产：测试不该因为读值而改变加载状态）。 */
	static UClass* FindClass(const FString& InNameOrPath);

	/** 能当 `@xx` 的玩家相关名字：gi gs gm ps pc pawn hud。 */
	static const TArray<FString>& PlayerRootNames();

	/**
	 * 本地玩家那一份：gi、gs、gm（只有服务器 / 单机有）、ps、pc、pawn、hud。
	 * 拿不到时返回空并写原因（「GameMode 只在服务器上有」「还没有 Pawn」），给 unresolved 用。
	 */
	static UObject* Player(UWorld& InWorld, const FString& InWhich, FString& OutError);

	/** 这个世界能看到的全部子系统：GameInstance、World、本地玩家（LocalPlayer）、Engine 四类。 */
	static void Subsystems(UWorld& InWorld, TArray<UObject*>& OutSubsystems);

	/** 按类名找子系统（`U` 前缀可省、子类也算）；多个同类（不同本地玩家）时取第一个本地玩家的。 */
	static UObject* Subsystem(UWorld& InWorld, const FString& InClassName, FString& OutError);

	/**
	 * `@actor[tag=X]` / `[class=BP_X]` / `[name=大纲名]`：世界里**唯一**命中的 Actor，0 个或多个都报错（多个时列出前几个）。
	 * InBy 是 tag / class / name。
	 */
	static AActor* UniqueActor(UWorld& InWorld, const FString& InBy, const FString& InValue, FString& OutError);

	/** 给人看的名字：编辑器里用大纲名（Actor 标签），游戏进程里用对象名。 */
	static FString DisplayName(const UObject* InObject);
};
