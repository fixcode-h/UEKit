#include "CQTest.h"

#include "Core/ForgeClassResolver.h"

#include "Animation/AnimInstance.h"
#include "Blueprint/UserWidget.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"

#if WITH_AUTOMATION_WORKER

/**
 * 类引用解析的测试：全路径 / 短名 / UnrealSharp 的 C# 类（按名兜底）/ 失败诊断。
 *
 * 背景：UnrealSharp 的 C# 类不是包资产（运行时由托管程序集创建），`LoadObject` 对它们的命中
 * 取决于加载时机；而配置里写的正是这类路径。解析器必须有"按名兜底"，失败时给可操作诊断。
 */
TEST_CLASS_WITH_FLAGS(ForgeClassResolverTest, "AssetForge.ClassResolver",
                      EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
{
	TEST_METHOD(ResolvesNativeClassByFullPath)
	{
		FString Error;
		UClass* Resolved = FForgeClassResolver::Resolve(TEXT("/Script/UMG.UserWidget"), UWidget::StaticClass(), Error);
		ASSERT_THAT(IsTrue(Resolved == UUserWidget::StaticClass(), TEXT("应解析到 UUserWidget")));
		ASSERT_THAT(IsTrue(Error.IsEmpty(), TEXT("成功时不应有诊断")));
	}

	TEST_METHOD(ResolvesUnrealSharpClassByLoadedName)
	{
		// 项目里所有界面 WBP 的父类都是 `UUIWidgetBase`（ui-framework 的契约），
		// 它正是"运行时创建、不在包资产里"的那类类——解析器靠按名兜底命中。
		FString Error;
		UClass* Resolved = FForgeClassResolver::Resolve(
			TEXT("/Script/UnrealSharp.UIWidgetBase_C"), UUserWidget::StaticClass(), Error);

		ASSERT_THAT(IsTrue(Resolved != nullptr,
			TEXT("应解析到 UnrealSharp 的 C# 类；若失败，先确认 C# 程序集已编译并加载")));
		if (Resolved != nullptr)
		{
			ASSERT_THAT(IsTrue(Resolved->IsChildOf(UUserWidget::StaticClass()), TEXT("它应是 UUserWidget 的子类")));
		}
	}

	TEST_METHOD(FailsWithActionableErrorForMissingUnrealSharpClass)
	{
		FString Error;
		UClass* Resolved = FForgeClassResolver::Resolve(
			TEXT("/Script/UnrealSharp.NoSuchClass_C"), UUserWidget::StaticClass(), Error);

		ASSERT_THAT(IsTrue(Resolved == nullptr, TEXT("不存在的类不应解析成功")));
		ASSERT_THAT(IsTrue(Error.Contains(TEXT("UnrealSharp")), TEXT("诊断应指出这是 C# 类、以及先编译程序集")));
	}

	TEST_METHOD(FailsWhenRequiredBaseDoesNotMatch)
	{
		FString Error;
		UClass* Resolved = FForgeClassResolver::Resolve(
			TEXT("/Script/UMG.UserWidget"), UAnimInstance::StaticClass(), Error);

		ASSERT_THAT(IsTrue(Resolved == nullptr, TEXT("类型不符不应解析成功")));
		ASSERT_THAT(IsTrue(Error.Contains(TEXT("子类")), TEXT("诊断应说明类型不符")));
	}

	TEST_METHOD(ResolvesShortNameThroughPrefixes)
	{
		const TArray<FString> Prefixes = { TEXT("/Script/UMG.") };
		FString Error;
		UClass* Resolved = FForgeClassResolver::ResolveShortName(
			TEXT("TextBlock"), Prefixes, UWidget::StaticClass(), Error);

		ASSERT_THAT(IsTrue(Resolved == UTextBlock::StaticClass(), TEXT("短名应在前缀表里解析到 UTextBlock")));
	}
};

#endif
