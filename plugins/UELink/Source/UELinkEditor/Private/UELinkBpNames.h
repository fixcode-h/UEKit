// bp.edit 一条操作作用到多个成员（2.14.0）：name / node 给数组或通配符，组件还能按类筛，
// 展开成多条同样的操作，免得改十个组件的同一个属性要写十条 set_component
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;

namespace AgentPanel
{
	/**
	 * 展开一条操作：不需要展开（name 是普通的单个名字）时 Out 只有它自己。
	 * 能展开的：set_variable、remove_variable、remove_function、set_component、remove_component 的 name，remove_node 的 node；
	 * 组件的两个操作另认 componentClass（含子类），name 不写时就是这个类的全部组件。一个都没匹配上返回 false 并写原因
	 */
	bool ExpandBlueprintOp(UBlueprint* BP, const TSharedPtr<FJsonObject>& Op, TArray<TSharedPtr<FJsonObject>>& Out, FString& Error);
}
