// Copyright Epic Games, Inc. All Rights Reserved.

#include "Observe/Lua/GameTestLuaTable.h"

#include "Engine/World.h"
#include "UObject/Package.h"

namespace GameTestLuaTableDetail
{
	/**
	 * 替身表：键 = 世界 + luaref。替身挂根（AddToRoot）免得两次解析之间被回收、面板拿路径找不到；
	 * 太多时整个放掉（替身很小，下次解析再建）。不用静态的 TStrongObjectPtr：进程退出时它析构在 UObject 系统之后
	 */
	TMap<FString, TWeakObjectPtr<UGameTestLuaTable>>& ProxyCache()
	{
		static TMap<FString, TWeakObjectPtr<UGameTestLuaTable>> Map;
		return Map;
	}

	void ReleaseProxies()
	{
		for (TPair<FString, TWeakObjectPtr<UGameTestLuaTable>>& Pair : ProxyCache())
		{
			if (UGameTestLuaTable* Proxy = Pair.Value.Get())
			{
				Proxy->RemoveFromRoot();
			}
		}
		ProxyCache().Reset();
	}

	constexpr int32 MaxCached = 4096;
}

FString UGameTestLuaTable::Label() const
{
	if (!Info.Name.IsEmpty())
	{
		return Info.Name;
	}
	// 同一个类的多个实例要分得开（面板按名字选候选）：模块名最后一段 + luaref 编号
	TArray<FString> Parts;
	Info.Module.Replace(TEXT("."), TEXT("/")).ParseIntoArray(Parts, TEXT("/"), true);
	const FString Id = Info.Ref.Replace(TEXT("luaref:"), TEXT(""));
	return Parts.Num() > 0 ? FString::Printf(TEXT("%s#%s"), *Parts.Last(), *Id) : Info.Ref;
}

UGameTestLuaTable* UGameTestLuaTable::Obtain(UWorld* InWorld, const FGameTestLuaInstance& InInfo)
{
	using namespace GameTestLuaTableDetail;
	if (InInfo.Ref.IsEmpty())
	{
		return nullptr;
	}
	const FString Key = FString::Printf(TEXT("%llu|%s"), static_cast<uint64>(reinterpret_cast<UPTRINT>(InWorld)), *InInfo.Ref);
	if (const TWeakObjectPtr<UGameTestLuaTable>* Found = ProxyCache().Find(Key))
	{
		if (UGameTestLuaTable* Existing = Found->Get())
		{
			// 显示名字段、路径可能变了：每次按最新的
			Existing->Info = InInfo;
			return Existing;
		}
	}
	if (ProxyCache().Num() >= MaxCached)
	{
		ReleaseProxies();
	}
	// 名字带上 luaref 的编号，对象路径一眼看得出是哪张表
	const FString Base = FString::Printf(TEXT("LuaTable_%s"), *InInfo.Ref.Replace(TEXT("luaref:"), TEXT("")));
	UGameTestLuaTable* Proxy = NewObject<UGameTestLuaTable>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UGameTestLuaTable::StaticClass(), FName(*Base)), RF_Transient);
	if (Proxy == nullptr)
	{
		return nullptr;
	}
	Proxy->Info = InInfo;
	Proxy->World = InWorld;
	Proxy->AddToRoot();
	ProxyCache().Add(Key, Proxy);
	return Proxy;
}
