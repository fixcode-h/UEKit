// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * 「加载完了没有」的唯一判断：World.IsLoaded、World.Open（等新地图）、Wait.Loading 共用，
 * 免得一个说加载完了、另一个还在等。
 */
namespace GameTestApiLoading
{
	/** 世界能玩、该加载的流送关卡都加载并显示了、没有异步加载中的包。没好时 `OutWhy` 写还在等什么。 */
	bool IsDone(const UWorld* InWorld, FString& OutWhy);
}
