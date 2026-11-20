// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"
#include "GameTestCompat.h"
#include "Observe/GameTestCondition.h"
#include "Observe/GameTestSnapshot.h"
#include "Observe/GameTestWidgetNode.h"

#include "Components/RichTextBlock.h"
#include "Components/Widget.h"
#include "Internationalization/Regex.h"

namespace GameTestStateTextImpl
{
	/** `:text`：显示的字（富文本去标记；按钮取里面的字）。 */
	class FState final : public IGameTestState
	{
	public:
		virtual FName GetKey() const override
		{
			return TEXT("text");
		}

		virtual EGameTestStateScope GetScope() const override
		{
			return EGameTestStateScope::Widget;
		}

		virtual EGameTestKind GetKind() const override
		{
			return EGameTestKind::Text;
		}

		virtual FString GetSummary() const override
		{
			return TEXT("显示的字（富文本去标记；按钮取里面的字）");
		}

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			UWidget* Widget = GameTestStateUtil::WidgetOf(InObject);
			if (Widget == nullptr)
			{
				return GameTestStateUtil::NotApplicable(InObject, TEXT("text"));
			}
			FGameTestStateValue Out;
			FText Text;
			if (GameTestStateUtil::TextWidgetText(Widget, Text))
			{
				FString Plain = Text.ToString();
				if (Widget->IsA<URichTextBlock>())
				{
					// 富文本去掉 <tag>…</> 标记，只留显示的字
					const FRegexPattern Pattern(TEXT("<[^<>]*>"));
					FRegexMatcher Matcher(Pattern, Plain);
					FString Stripped;
					int32 Last = 0;
					while (Matcher.FindNext())
					{
						Stripped += Plain.Mid(Last, Matcher.GetMatchBeginning() - Last);
						Last = Matcher.GetMatchEnding();
					}
					Plain = Stripped + Plain.Mid(Last);
				}
				Out.Value = FGameTestValue::FromString(Plain);
				FString Namespace;
				FString Key;
				if (GameTestCompat::TextKeyOf(Text, Namespace, Key))
				{
					Out.TextKey = Namespace + TEXT(",") + Key;
				}
				return Out;
			}
			// 没有文字控件（按钮里是嵌套 WBP 之类）：按快照的取字规则从 Slate 子树收
			const TSharedPtr<FGameTestWidgetNode> Node = GameTestSnapshot::CaptureSubtree(Widget, Widget->GetName(), false, true);
			Out.Value = FGameTestValue::FromString(Node.IsValid() ? GameTestCondition::ReadText(*Node) : FString());
			return Out;
		}
	};
}

namespace GameTestBuiltin
{
	void RegisterStateText()
	{
		FGameTestStateRegistry::Get().Register(MakeShared<GameTestStateTextImpl::FState>());
	}
}
