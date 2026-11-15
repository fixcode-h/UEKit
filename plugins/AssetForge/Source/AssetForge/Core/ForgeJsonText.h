// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 算法层：JSON 文本的读写。
 *
 * 为什么需要 StripComments：UE 5.8 自带的 JSON 解析器是**严格 JSON**——不支持 `//` 与
 * `/* *​/` 注释（证据：Runtime/Json/Public/Serialization/JsonReader.h 的 IsWhitespace 只认
 * 空白，NextToken 的 switch 没有 `/` 分支）。而布局配置恰恰最需要注释，所以我们在交给引擎
 * 解析之前先剥一层，剥完的文本才是引擎看到的输入。
 */
namespace ForgeJsonText
{
	/** 剥离行注释与块注释；字符串字面量内的 `//` 不受影响；换行数保持不变以便对照。 */
	ASSETFORGE_API FString StripComments(const FString& InText);

	/** 解析一段 JSON 文本为对象。 */
	ASSETFORGE_API bool ParseObject(const FString& InJson, TSharedPtr<FJsonObject>& OutObject, FString& OutError);

	/** 读文件 → 剥注释 → 解析为对象。 */
	ASSETFORGE_API bool LoadObject(const FString& InFilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutError);

	/** 以 Pretty 策略写回磁盘（父目录不存在时自动创建）。 */
	ASSETFORGE_API bool SaveObject(const FString& InFilePath, const TSharedPtr<FJsonObject>& InObject, FString& OutError);

	/** 序列化成紧凑字符串。注意它**保留字段顺序**，因此不适合用于相等判断。 */
	ASSETFORGE_API FString ToCondensedString(const TSharedPtr<FJsonObject>& InObject);

	/**
	 * 字段名的规范化：大小写任意、`b` 前缀可省。
	 *
	 * 导入按 UE 的属性查找走（`FindFProperty` 本就大小写不敏感，另加「`b` 前缀可省」），
	 * 所以 `defaultValue` 能写进 `DefaultValue`。比较与诊断若按原样比，这份**能导入成功**
	 * 的配置就会被判成「每次都不同」：表现成每次导入都报同一个 update、资产怎么都不收敛。
	 */
	ASSETFORGE_API FString NormalizeKey(const FString& InKey);

	/**
	 * 序列化成「按键递归排序」的规范形式，用于判断两个 JSON 对象是否等价。
	 *
	 * 为什么不能直接用 ToCondensedString 比字符串：`FJsonObject::Values` 是 TMap，
	 * 迭代顺序取决于**插入顺序**。配置由人/AI 手写、引擎导出按属性声明顺序生成，
	 * 两者对同一个对象几乎不可能给出同一个顺序——不比规范形式的话，
	 * 「内容相同但字段顺序不同」会被判成发生了变化，每次导入都误报一遍。
	 *
	 * 键在排序前先过 `NormalizeKey`：字段顺序与写法都不该影响等价判断。
	 * 字符串值先按**对象引用**归一（`/Game/X.Y` 与 `/Script/Engine.Texture2D'/Game/X.Y'`
	 * 是同一个引用）——两者导入都收，比较也就必须都认。
	 */
	ASSETFORGE_API FString ToCanonicalString(const TSharedPtr<FJsonObject>& InObject);

	/** 序列化成 Pretty 字符串。 */
	ASSETFORGE_API FString ToPrettyString(const TSharedPtr<FJsonObject>& InObject);
}
