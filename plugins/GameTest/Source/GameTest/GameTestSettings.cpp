// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameTestSettings.h"

UGameTestSettings::UGameTestSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Game Test");
}

FName UGameTestSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}
