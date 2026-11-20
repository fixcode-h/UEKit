// 引擎版本差异只写在这里（UEKit 约定）：业务代码只用这里定义的宏，不直接判断 ENGINE_*_VERSION。
// GameTest 以 UE 5.4 为基准开发：默认写 5.4 的写法，5.5+ / 5.8+ 才有的接口在这里用版本判断包住，调用处只用宏。
#pragma once

#include "Misc/EngineVersionComparison.h"

#if UE_VERSION_OLDER_THAN(5, 4, 0)
	#error "GameTest 需要 UE 5.4 或更新的版本"
#endif

// FJsonObject::Values 的键：5.4 是 FString，5.8 换成了 UE::FSharedString（不能隐式转 FString）。
// 两者的 operator* 都返回 const TCHAR*，按这个取键在各版本都能编过；遍历时用 const auto& 接 Pair。
#define GAMETEST_JSON_KEY(Key) FString(*(Key))

// FStringOutputDevice：5.4 声明在 Containers/UnrealString.h（Core 头里已带上），之后单独搬进 Misc/StringOutputDevice.h。
// 按头文件在不在来 include，不猜具体是哪一版搬的（5.4 上 include 新路径会 C1083）。
#if __has_include("Misc/StringOutputDevice.h")
	#include "Misc/StringOutputDevice.h"
#else
	#include "Containers/UnrealString.h"
#endif

// UEnhancedInputLocalPlayerSubsystem::HasContinuousInputInjectionForAction：5.4 没有（C2039）。
// 只在确认编过的 5.8 用引擎的查询，更早的版本由 GameTestInjection 自己记哪些动作在注入。
#define GAMETEST_HAS_INJECTION_QUERY UE_VERSION_NEWER_THAN(5, 7, 99)

// FText 的本地化键（`:textKey`、比较 `key`，GameTest 2.0.0）：FTextInspector::GetNamespace / GetKey。
// 各版本的返回类型没在本地核对过（5.4 是 TOptional<FString>，之后的版本可能换成别的），
// 这里按两种写法都接住：返回 TOptional<FString> 或 FString 都能编过，调用处只用 GameTestCompat::TextKeyOf。
// 需要在本地 5.4 / 5.8 上确认能编（CHANGELOG 2.0.0「需要本地确认的引擎接口」）。
#include "Internationalization/Text.h"

namespace GameTestCompat
{
	inline bool TextPieceOf(const TOptional<FString>& InPiece, FString& OutText)
	{
		if (!InPiece.IsSet())
		{
			return false;
		}
		OutText = InPiece.GetValue();
		return true;
	}

	inline bool TextPieceOf(const FString& InPiece, FString& OutText)
	{
		OutText = InPiece;
		return true;
	}

	/** FText 的命名空间和键；没有键（运行时拼出来的文字、FText::FromString）返回 false。 */
	inline bool TextKeyOf(const FText& InText, FString& OutNamespace, FString& OutKey)
	{
		OutNamespace.Reset();
		OutKey.Reset();
		if (!TextPieceOf(FTextInspector::GetKey(InText), OutKey) || OutKey.IsEmpty())
		{
			return false;
		}
		TextPieceOf(FTextInspector::GetNamespace(InText), OutNamespace);
		return true;
	}
}
