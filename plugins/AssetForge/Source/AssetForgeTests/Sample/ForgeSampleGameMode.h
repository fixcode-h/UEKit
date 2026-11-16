// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ForgeSampleGameMode.generated.h"

class UUserWidget;

/**
 * 示例测试地图（`/Game/AgentTest/AssetForge/Map_ForgeSample`，配置 `Test/map/Map_ForgeSample.json`）的 GameMode：
 * 本地玩家一进来就把示例界面加进视口。
 *
 * 这样手动录制只要「打开这张地图 → PIE → 录制」，录出来的场景开头的 `pie.start` 回放时也会自己把界面带出来，
 * 不用再补一步 `widget.add`。没有 Pawn：示例界面盖满屏幕，不需要在场景里走动。
 */
UCLASS()
class AForgeSampleGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AForgeSampleGameMode();

	/** 要显示的界面；默认是示例配置导入生成的控件蓝图 */
	UPROPERTY(EditAnywhere, Category = "ForgeSample")
	TSoftClassPtr<UUserWidget> ScreenClass;

	/** 示例界面生成类的默认路径（控制台命令 AssetForge.Sample 也用它） */
	static const TCHAR* DefaultScreenClassPath;

protected:
	virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;
};
