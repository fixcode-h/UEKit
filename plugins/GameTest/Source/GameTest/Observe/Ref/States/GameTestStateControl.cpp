// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Ref/GameTestState.h"

#include "Components/ComboBoxString.h"
#include "Components/ExpandableArea.h"
#include "Components/InputKeySelector.h"
#include "Components/ListView.h"
#include "Components/MenuAnchor.h"
#include "Components/ScrollBox.h"
#include "Components/WidgetSwitcher.h"

/**
 * 控件自己的运行时状态（没有 UPROPERTY 可读、只能调 getter 的那几种）：
 * `:selectedIndex`（ComboBoxString）、`:selectedCount`（ListView 族）、`:scrollOffset`（ScrollBox）、
 * `:open`（MenuAnchor）、`:selectedKey`（InputKeySelector）、`:activeIndex`（WidgetSwitcher）、`:expanded`（ExpandableArea）。
 * v3 的 stateKey 迁移过来就落在这些名字上；getter 和旧版快照（GameTestSnapshot）用的是同一批。
 * 每种都很短，用一张表注册，不各开一个文件。
 */
namespace GameTestStateControlImpl
{
	using FApplies = bool (*)(const UObject*);
	using FRead = FGameTestValue (*)(const UObject*);

	class FState final : public IGameTestState
	{
	public:
		FState(FName InKey, EGameTestKind InKind, const TCHAR* InSummary, bool bInStable, FApplies InApplies, FRead InRead)
			: Key(InKey), Kind(InKind), Summary(InSummary), bStable(bInStable), Applies(InApplies), ReadFn(InRead)
		{
		}

		virtual FName GetKey() const override { return Key; }
		virtual EGameTestStateScope GetScope() const override { return EGameTestStateScope::Widget; }
		virtual EGameTestKind GetKind() const override { return Kind; }
		virtual FString GetSummary() const override { return Summary; }
		virtual bool IsStable() const override { return bStable; }
		virtual bool AppliesTo(const UObject* InObject) const override { return InObject != nullptr && Applies(InObject); }

		virtual FGameTestStateValue Read(UObject* InObject, FGameTestStateContext& InContext) const override
		{
			(void)InContext;
			if (InObject == nullptr || !Applies(InObject))
			{
				return GameTestStateUtil::NotApplicable(InObject, *Key.ToString());
			}
			return GameTestStateUtil::Of(ReadFn(InObject));
		}

	private:
		FName Key;
		EGameTestKind Kind;
		FString Summary;
		bool bStable;
		FApplies Applies;
		FRead ReadFn;
	};

	template <typename T>
	bool Is(const UObject* InObject)
	{
		return InObject->IsA<T>();
	}

	FGameTestValue ComboIndex(const UObject* InObject) { return FGameTestValue::FromNumber(CastChecked<UComboBoxString>(InObject)->GetSelectedIndex()); }
	FGameTestValue ListSelected(const UObject* InObject) { return FGameTestValue::FromNumber(CastChecked<UListView>(InObject)->GetNumItemsSelected()); }
	FGameTestValue ScrollOffset(const UObject* InObject) { return FGameTestValue::FromNumber(CastChecked<UScrollBox>(InObject)->GetScrollOffset()); }
	FGameTestValue MenuOpen(const UObject* InObject) { return FGameTestValue::FromBool(CastChecked<UMenuAnchor>(InObject)->IsOpen()); }
	FGameTestValue SelectedKey(const UObject* InObject) { return FGameTestValue::FromString(CastChecked<UInputKeySelector>(InObject)->GetSelectedKey().Key.GetFName().ToString()); }
	FGameTestValue ActiveIndex(const UObject* InObject) { return FGameTestValue::FromNumber(CastChecked<UWidgetSwitcher>(InObject)->GetActiveWidgetIndex()); }
	FGameTestValue Expanded(const UObject* InObject) { return FGameTestValue::FromBool(CastChecked<UExpandableArea>(InObject)->GetIsExpanded()); }
}

namespace GameTestBuiltin
{
	void RegisterStateControl()
	{
		using namespace GameTestStateControlImpl;
		FGameTestStateRegistry& Registry = FGameTestStateRegistry::Get();
		Registry.Register(MakeShared<FState>(TEXT("selectedIndex"), EGameTestKind::Int, TEXT("下拉框选中第几项（从 0 起，没选是 -1）"), true, &Is<UComboBoxString>, &ComboIndex));
		Registry.Register(MakeShared<FState>(TEXT("selectedCount"), EGameTestKind::Int, TEXT("列表选中了几条"), true, &Is<UListView>, &ListSelected));
		// 滚动在下一帧布局时才生效：读回要等一帧，所以不稳定、不建议当标识
		Registry.Register(MakeShared<FState>(TEXT("scrollOffset"), EGameTestKind::Float, TEXT("滚动框当前滚到的位置（0 = 最上面）"), false, &Is<UScrollBox>, &ScrollOffset));
		Registry.Register(MakeShared<FState>(TEXT("open"), EGameTestKind::Bool, TEXT("菜单锚点的菜单开着没有"), true, &Is<UMenuAnchor>, &MenuOpen));
		Registry.Register(MakeShared<FState>(TEXT("selectedKey"), EGameTestKind::String, TEXT("按键选择器选的键名（如 SpaceBar）"), true, &Is<UInputKeySelector>, &SelectedKey));
		Registry.Register(MakeShared<FState>(TEXT("activeIndex"), EGameTestKind::Int, TEXT("切换器当前显示第几页（从 0 起）"), true, &Is<UWidgetSwitcher>, &ActiveIndex));
		Registry.Register(MakeShared<FState>(TEXT("expanded"), EGameTestKind::Bool, TEXT("折叠区展开没有"), true, &Is<UExpandableArea>, &Expanded));
	}
}
