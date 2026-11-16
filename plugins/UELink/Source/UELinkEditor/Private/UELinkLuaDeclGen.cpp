// Lua 声明的文本生成：只读反射，不碰磁盘。调度、清单、事件在 UELinkLuaDecl.cpp。
// 类型名和 UnLua 的 IntelliSense 保持一致（AActor、BP_Hero_C、FVector、TArray<integer>），
// 这样项目里已有的 ---@type 注解和 UnLua 自带的 UnLua.lua（TArray / TMap 等泛型声明）照样对得上。
#include "UELinkLuaDecl.h"

#include "Engine/Blueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "StructUtils/UserDefinedStruct.h"
#include "ObjectEditorUtils.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace UELinkLuaDecl
{
	namespace
	{
		const FName NAME_ToolTip(TEXT("ToolTip"));
		const FName NAME_LatentInfo(TEXT("LatentInfo"));
		const TCHAR* Header = TEXT("-- 由 UELink 根据 UE 反射自动生成，会被覆盖，不要手改\n");

		const TCHAR* LuaKeywords[] = {
			TEXT("and"), TEXT("break"), TEXT("do"), TEXT("else"), TEXT("elseif"), TEXT("end"), TEXT("false"), TEXT("for"),
			TEXT("function"), TEXT("goto"), TEXT("if"), TEXT("in"), TEXT("local"), TEXT("nil"), TEXT("not"), TEXT("or"),
			TEXT("repeat"), TEXT("return"), TEXT("then"), TEXT("true"), TEXT("until"), TEXT("while")};

		bool IsKeyword(const FString& Name)
		{
			for (const TCHAR* K : LuaKeywords)
			{
				if (Name.Equals(K, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		/** 能直接写成 Lua 标识符（a.b、function A:b）的名字 */
		bool IsIdentifier(const FString& Name)
		{
			if (Name.IsEmpty() || IsKeyword(Name) || FChar::IsDigit(Name[0]))
			{
				return false;
			}
			for (const TCHAR C : Name)
			{
				if (!(FChar::IsAlnum(C) && C < 128) && C != TEXT('_'))
				{
					return false;
				}
			}
			return true;
		}

		/** 参数名、局部变量名：不合法的字符换成 _，撞关键字或数字开头加 _ */
		FString SafeIdent(const FString& In)
		{
			FString Name = In;
			for (int32 i = 0; i < Name.Len(); ++i)
			{
				const TCHAR C = Name[i];
				if (!(FChar::IsAlnum(C) && C < 128) && C != TEXT('_'))
				{
					Name[i] = TEXT('_');
				}
			}
			if (Name.IsEmpty() || IsKeyword(Name) || FChar::IsDigit(Name[0]))
			{
				Name = TEXT("_") + Name;
			}
			return Name;
		}

		FString Quote(const FString& S)
		{
			return TEXT("\"") + S.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
		}

		/** 注释：多行时每行加 ---；以 @ 开头的行（UE 文档里的 @param 之类）去掉，免得被当成注解 */
		FString DocBlock(const FString& ToolTip)
		{
			if (ToolTip.IsEmpty())
			{
				return FString();
			}
			TArray<FString> Lines;
			ToolTip.Replace(TEXT("\r"), TEXT("")).ParseIntoArray(Lines, TEXT("\n"), true);
			FString Out;
			for (FString& Line : Lines)
			{
				Line.TrimStartAndEndInline();
				if (Line.IsEmpty() || Line.StartsWith(TEXT("@")))
				{
					continue;
				}
				Out += TEXT("---") + Line + TEXT("\n");
			}
			return Out;
		}

		/** 行尾说明（---@field 后面）：压成一行 */
		FString InlineDoc(const FString& ToolTip)
		{
			if (ToolTip.IsEmpty())
			{
				return FString();
			}
			TArray<FString> Lines;
			ToolTip.Replace(TEXT("\r"), TEXT("")).ParseIntoArray(Lines, TEXT("\n"), true);
			FString Out;
			for (FString& Line : Lines)
			{
				Line.TrimStartAndEndInline();
				if (Line.IsEmpty() || Line.StartsWith(TEXT("@")))
				{
					continue;
				}
				Out += (Out.IsEmpty() ? TEXT("") : TEXT(" ")) + Line;
			}
			return Out.IsEmpty() ? Out : TEXT(" @") + Out;
		}

		FString MetaToolTip(const UField* Field)
		{
#if WITH_EDITORONLY_DATA
			return Field ? Field->GetMetaData(NAME_ToolTip) : FString();
#else
			return FString();
#endif
		}

		FString MetaToolTip(const FProperty* Property)
		{
#if WITH_EDITORONLY_DATA
			return Property ? Property->GetMetaData(NAME_ToolTip) : FString();
#else
			return FString();
#endif
		}

		/** 属性在 Lua 里的名字：用户定义结构体的成员内部名带 GUID 后缀，取作者起的名字 */
		FString PropertyName(const FProperty* Property)
		{
			if (Property && Cast<UUserDefinedStruct>(Property->GetOwnerStruct()))
			{
				return Property->GetAuthoredName();
			}
			return Property ? Property->GetName() : FString();
		}

		FString ClassRef(const UClass* Class)
		{
			return Class ? TypeName(Class) : FString(TEXT("UObject"));
		}

		FString PropertyType(const FProperty* Property)
		{
			if (!Property)
			{
				return TEXT("any");
			}
			if (const FByteProperty* Byte = CastField<FByteProperty>(Property))
			{
				return Byte->Enum ? TypeName(Byte->Enum) : FString(TEXT("integer"));
			}
			if (const FEnumProperty* Enum = CastField<FEnumProperty>(Property))
			{
				return Enum->GetEnum() ? TypeName(Enum->GetEnum()) : FString(TEXT("integer"));
			}
			if (CastField<FBoolProperty>(Property))
			{
				return TEXT("boolean");
			}
			if (CastField<FFloatProperty>(Property) || CastField<FDoubleProperty>(Property))
			{
				return TEXT("number");
			}
			if (Property->IsA<FNumericProperty>())
			{
				return TEXT("integer");
			}
			if (CastField<FNameProperty>(Property) || CastField<FStrProperty>(Property) || CastField<FTextProperty>(Property))
			{
				return TEXT("string");
			}
			// 顺序要紧：类引用、软类引用分别是对象引用、软对象引用的子类
			if (const FClassProperty* Cls = CastField<FClassProperty>(Property))
			{
				return FString::Printf(TEXT("TSubclassOf<%s>"), *ClassRef(Cls->MetaClass));
			}
			if (const FSoftClassProperty* SoftCls = CastField<FSoftClassProperty>(Property))
			{
				return FString::Printf(TEXT("TSoftClassPtr<%s>"), *ClassRef(SoftCls->MetaClass));
			}
			if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(Property))
			{
				return FString::Printf(TEXT("TSoftObjectPtr<%s>"), *ClassRef(Soft->PropertyClass));
			}
			if (const FWeakObjectProperty* Weak = CastField<FWeakObjectProperty>(Property))
			{
				return FString::Printf(TEXT("TWeakObjectPtr<%s>"), *ClassRef(Weak->PropertyClass));
			}
			if (const FLazyObjectProperty* Lazy = CastField<FLazyObjectProperty>(Property))
			{
				return FString::Printf(TEXT("TLazyObjectPtr<%s>"), *ClassRef(Lazy->PropertyClass));
			}
			if (const FObjectPropertyBase* Obj = CastField<FObjectPropertyBase>(Property))
			{
				return ClassRef(Obj->PropertyClass);
			}
			if (const FInterfaceProperty* Iface = CastField<FInterfaceProperty>(Property))
			{
				return FString::Printf(TEXT("TScriptInterface<%s>"), *ClassRef(Iface->InterfaceClass));
			}
			if (const FArrayProperty* Arr = CastField<FArrayProperty>(Property))
			{
				return FString::Printf(TEXT("TArray<%s>"), *PropertyType(Arr->Inner));
			}
			if (const FSetProperty* Set = CastField<FSetProperty>(Property))
			{
				return FString::Printf(TEXT("TSet<%s>"), *PropertyType(Set->ElementProp));
			}
			if (const FMapProperty* Map = CastField<FMapProperty>(Property))
			{
				return FString::Printf(TEXT("TMap<%s, %s>"), *PropertyType(Map->KeyProp), *PropertyType(Map->ValueProp));
			}
			if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
			{
				return Struct->Struct ? TypeName(Struct->Struct) : FString(TEXT("any"));
			}
			if (CastField<FDelegateProperty>(Property))
			{
				return TEXT("Delegate");
			}
			if (CastField<FMulticastDelegateProperty>(Property))
			{
				return TEXT("MulticastDelegate");
			}
			return TEXT("any");
		}

		void AppendField(FString& Out, const FProperty* Property)
		{
			const FString Name = PropertyName(Property);
			const FString Key = IsIdentifier(Name) ? Name : TEXT("[") + Quote(Name) + TEXT("]");
			Out += FString::Printf(TEXT("---@field %s %s%s\n"), *Key, *PropertyType(Property), *InlineDoc(MetaToolTip(Property)));
		}

		/** 一个 UFunction：UnLua 的调用约定是返回值在前、非 const 引用的输出参数依次跟在后面，输出参数和带默认值的参数可以不传 */
		void AppendFunction(FString& Out, const UFunction* Function, const FString& Owner)
		{
			Out += DocBlock(MetaToolTip(Function));
			FString Params;
			FString ReturnLines;
			FString OutLines;
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				const FProperty* P = *It;
				if (P->GetFName() == NAME_LatentInfo)
				{
					continue;
				}
				const FString Type = PropertyType(P);
				if (P->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					ReturnLines += FString::Printf(TEXT("---@return %s\n"), *Type);
					continue;
				}
				const FString Name = SafeIdent(P->GetName());
				const bool bOut = P->HasAnyPropertyFlags(CPF_OutParm) && !P->HasAnyPropertyFlags(CPF_ConstParm);
#if WITH_EDITORONLY_DATA
				const bool bDefault = Function->HasMetaData(*FString::Printf(TEXT("CPP_Default_%s"), *P->GetName()));
#else
				const bool bDefault = false;
#endif
				Out += FString::Printf(TEXT("---@param %s%s %s%s\n"), *Name, (bOut || bDefault) ? TEXT("?") : TEXT(""), *Type,
					bOut ? TEXT(" @[out]") : (bDefault ? TEXT(" @[opt]") : TEXT("")));
				if (bOut)
				{
					OutLines += FString::Printf(TEXT("---@return %s %s\n"), *Type, *Name);
				}
				Params += (Params.IsEmpty() ? TEXT("") : TEXT(", ")) + Name;
			}
			// 有输出参数但没有返回值时，第一个返回值就是第一个输出参数
			Out += ReturnLines + OutLines;

			const FString FuncName = Function->GetName();
			const bool bStatic = Function->HasAnyFunctionFlags(FUNC_Static);
			if (IsIdentifier(FuncName))
			{
				Out += FString::Printf(TEXT("function %s%s%s(%s) end\n\n"), *Owner, bStatic ? TEXT(".") : TEXT(":"), *FuncName, *Params);
			}
			else
			{
				const FString SelfParams = bStatic ? Params : (Params.IsEmpty() ? FString(TEXT("self")) : TEXT("self, ") + Params);
				Out += FString::Printf(TEXT("%s[%s] = function(%s) end\n\n"), *Owner, *Quote(FuncName), *SelfParams);
			}
		}

		bool ShouldEmitFunction(const UFunction* Function, const UClass* Class)
		{
			if (!Function || Function->HasAnyFunctionFlags(FUNC_UbergraphFunction | FUNC_Delegate))
			{
				return false;
			}
			const FString Name = Function->GetName();
			// UnLua 覆写蓝图函数时会在运行时加 __Overridden 副本，不是真接口
			if (Name.IsEmpty() || Name.EndsWith(TEXT("__Overridden")) || Name.StartsWith(TEXT("ExecuteUbergraph")))
			{
				return false;
			}
			return !FObjectEditorUtils::IsFunctionHiddenFromClass(Function, Class);
		}

		FString GenerateStruct(const UStruct* Struct)
		{
			const FString Name = TypeName(Struct);
			const FString Local = SafeIdent(Name);
			FString Out = Header;
			Out += DocBlock(MetaToolTip(Struct));
			Out += TEXT("---@class ") + Name;
			if (const UStruct* Super = Struct->GetSuperStruct())
			{
				Out += TEXT(" : ") + TypeName(Super);
			}
			Out += TEXT("\n");
			for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::ExcludeSuper, EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
			{
				AppendField(Out, *It);
			}
			Out += FString::Printf(TEXT("local %s = {}\n\n"), *Local);

			const UClass* Class = Cast<UClass>(Struct);
			if (!Class)
			{
				return Out;
			}
			TSet<FName> Emitted;
			for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper, EFieldIteratorFlags::ExcludeDeprecated, EFieldIteratorFlags::ExcludeInterfaces); It; ++It)
			{
				if (ShouldEmitFunction(*It, Class))
				{
					AppendFunction(Out, *It, Local);
					Emitted.Add(It->GetFName());
				}
			}
			// 实现的接口函数也挂在类上（父类已经声明过的不重复：父类的声明文件里有）
			for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper, EFieldIteratorFlags::ExcludeDeprecated, EFieldIteratorFlags::IncludeInterfaces); It; ++It)
			{
				const UFunction* Function = *It;
				const UClass* OwnerClass = Function ? Function->GetOwnerClass() : nullptr;
				if (!OwnerClass || !OwnerClass->IsChildOf(UInterface::StaticClass()) || Emitted.Contains(Function->GetFName()))
				{
					continue;
				}
				if (ShouldEmitFunction(Function, Class))
				{
					AppendFunction(Out, Function, Local);
					Emitted.Add(Function->GetFName());
				}
			}
			return Out;
		}

		FString GenerateEnum(const UEnum* Enum)
		{
			const FString Name = TypeName(Enum);
			FString Out = Header;
			Out += DocBlock(MetaToolTip(Enum));
			Out += TEXT("---@class ") + Name + TEXT("\n");
			const bool bUser = Cast<UUserDefinedEnum>(Enum) != nullptr;
			// 最后一项是引擎自动加的 _MAX，UnLua 也能取到，照样列出
			for (int32 i = 0; i < Enum->NumEnums(); ++i)
			{
				const FString Item = Enum->GetNameStringByIndex(i);
				const FString Key = IsIdentifier(Item) ? Item : TEXT("[") + Quote(Item) + TEXT("]");
				FString Doc;
				if (bUser)
				{
					// 用户定义枚举的内部名是 NewEnumerator0 这种，显示名写进说明
					Doc = TEXT(" @") + Enum->GetDisplayNameTextByIndex(i).ToString();
				}
				Out += FString::Printf(TEXT("---@field %s integer%s\n"), *Key, *Doc);
			}
			Out += FString::Printf(TEXT("local %s = {}\n"), *SafeIdent(Name));
			return Out;
		}
	}

	FString TypeName(const UField* Field)
	{
		if (!Field)
		{
			return TEXT("any");
		}
		if (const UClass* Class = Cast<UClass>(Field))
		{
			// 蓝图生成类在 Lua 里就叫 BP_Hero_C；原生类带 C++ 前缀（AActor、UObject）
			return Class->HasAnyClassFlags(CLASS_Native) ? FString(Class->GetPrefixCPP()) + Class->GetName() : Class->GetName();
		}
		if (const UScriptStruct* Struct = Cast<UScriptStruct>(Field))
		{
			return Struct->GetStructCPPName();
		}
		return Field->GetName();
	}

	bool IsTransientType(const UField* Field)
	{
		if (!Field)
		{
			return true;
		}
		const FString Name = Field->GetName();
		static const TCHAR* Prefixes[] = {TEXT("SKEL_"), TEXT("REINST_"), TEXT("TRASHCLASS_"), TEXT("HOTRELOADED_"),
			TEXT("PLACEHOLDER-CLASS"), TEXT("LIVECODING_"), TEXT("TRASHSTRUCT_"), TEXT("TRASHENUM_")};
		for (const TCHAR* P : Prefixes)
		{
			if (Name.StartsWith(P))
			{
				return true;
			}
		}
		if (const UClass* Class = Cast<UClass>(Field))
		{
			if (Class->HasAnyClassFlags(CLASS_NewerVersionExists))
			{
				return true;
			}
		}
		if (const UScriptStruct* Struct = Cast<UScriptStruct>(Field))
		{
			// 编辑结构体时留下的旧版本
			if (const UUserDefinedStruct* User = Cast<UUserDefinedStruct>(Struct))
			{
				if (User->Status == EUserDefinedStructureStatus::UDSS_Duplicate)
				{
					return true;
				}
			}
		}
		return Field->GetOutermost() == GetTransientPackage();
	}

	FString Generate(const UField* Field)
	{
		if (IsTransientType(Field))
		{
			return FString();
		}
		if (const UEnum* Enum = Cast<UEnum>(Field))
		{
			return GenerateEnum(Enum);
		}
		if (Cast<UFunction>(Field))
		{
			return FString();
		}
		if (const UStruct* Struct = Cast<UStruct>(Field))
		{
			return GenerateStruct(Struct);
		}
		return FString();
	}

	FString GenerateStub(const FString& Name, const FString& Parent, const FString& AssetPath)
	{
		FString Out = Header;
		Out += FString::Printf(TEXT("---%s\n---（这个资源在编辑器里还没加载过，先只有继承关系；打开、编译一次或运行 luadecl.run loadBlueprints 后补全成员）\n"), *AssetPath);
		Out += TEXT("---@class ") + Name;
		if (!Parent.IsEmpty())
		{
			Out += TEXT(" : ") + Parent;
		}
		Out += FString::Printf(TEXT("\nlocal %s = {}\n"), *SafeIdent(Name));
		return Out;
	}

	FString GenerateUETable(const TArray<FString>& NativeTypeNames)
	{
		FString Out = Header;
		Out += TEXT("---UnLua 的全局 UE 表：原生类、结构体、枚举都能按名字取（UE.AActor、UE.FVector、UE.ECollisionChannel）\n");
		Out += TEXT("UE = {\n");
		for (const FString& Name : NativeTypeNames)
		{
			if (!IsIdentifier(Name))
			{
				continue;
			}
			Out += FString::Printf(TEXT("    ---@type %s\n    %s = nil,\n"), *Name, *Name);
		}
		Out += TEXT("}\n");
		return Out;
	}
}
