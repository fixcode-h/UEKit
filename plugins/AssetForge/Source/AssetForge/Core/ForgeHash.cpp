// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ForgeHash.h"

#include "Misc/Crc.h"

namespace ForgeHash
{

FString HashText(const FString& InText)
{
	const uint32 Crc = FCrc::StrCrc32(*InText);
	return FString::Printf(TEXT("%08x"), Crc);
}

} // namespace ForgeHash
