// Copyright Epic Games, Inc. All Rights Reserved.

#include "Operate/GameTestLuaChunk.h"
#include "GameTestCompat.h"

namespace GameTestLuaChunkLocal
{
	/**
	 * 外壳的固定部分：JSON 编码器、gt 帮手、编译函数库和节点代码、调用、把返回值编成 JSON。
	 * 变量名都带 __gt 前缀，不和用户代码里的名字撞。
	 */
	const TCHAR* const WrapperBody = TEXT(R"LUA(
local __gtEnv = setmetatable({}, { __index = _G })
local function __gtEnc(v, d)
  local t = type(v)
  if t == "nil" then return "null" end
  if t == "boolean" then return v and "true" or "false" end
  if t == "number" then
    if v ~= v or v == math.huge or v == -math.huge then return "null" end
    return string.format("%.14g", v)
  end
  if t == "string" then
    local s = string.gsub(v, '[%c"\\]', function(c)
      local m = { ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
      return m[c] or string.format("\\u%04x", string.byte(c))
    end)
    return '"' .. s .. '"'
  end
  if t == "table" and d < 6 then
    local n = #v
    local count = 0
    for _ in pairs(v) do count = count + 1 end
    local parts = {}
    if n > 0 and count == n then
      for i = 1, n do parts[i] = __gtEnc(v[i], d + 1) end
      return "[" .. table.concat(parts, ",") .. "]"
    end
    for k, x in pairs(v) do
      parts[#parts + 1] = __gtEnc(tostring(k), d + 1) .. ":" .. __gtEnc(x, d + 1)
      if #parts >= 200 then break end
    end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  return __gtEnc(tostring(v), d)
end
__gtEnv.gt = {
  log = print,
  fail = function(msg) error(tostring(msg), 0) end,
}
local __gtFn = {}
__gtEnv.fn = __gtFn
for __gtName, __gtDef in pairs(__gtLib) do
  local __gtHead = ""
  if #__gtDef.p > 0 then __gtHead = "local " .. table.concat(__gtDef.p, ", ") .. " = ...; " end
  local __gtF, __gtErr = load(__gtHead .. __gtDef.b, "=GameTest:库." .. __gtName, "t", __gtEnv)
  if not __gtF then error("函数库 " .. __gtName .. " 编译不过：" .. tostring(__gtErr), 0) end
  __gtFn[__gtName] = __gtF
end
for __gtK, __gtV in pairs(__gtArgs) do __gtEnv[__gtK] = __gtV end
local __gtMain, __gtErr = load(__gtSrc, __gtChunk, "t", __gtEnv)
if not __gtMain then error(tostring(__gtErr), 0) end
local __gtR = table.pack(__gtMain())
if __gtR.n == 0 then return "null" end
if __gtR.n == 1 then return __gtEnc(__gtR[1], 0) end
local __gtAll = {}
for __gtI = 1, __gtR.n do __gtAll[__gtI] = __gtEnc(__gtR[__gtI], 0) end
return "[" .. table.concat(__gtAll, ",") .. "]"
)LUA");

	bool IsLuaName(const FString& InName)
	{
		if (InName.IsEmpty() || FChar::IsDigit(InName[0]))
		{
			return false;
		}
		for (const TCHAR C : InName)
		{
			if (!(FChar::IsAlnum(C) || C == TEXT('_')))
			{
				return false;
			}
		}
		return true;
	}
}

FString GameTestLuaChunk::QuoteLua(const FString& InText)
{
	FString Out;
	Out.Reserve(InText.Len() + 2);
	Out.AppendChar(TEXT('"'));
	for (const TCHAR C : InText)
	{
		switch (C)
		{
		case TEXT('"'): Out += TEXT("\\\""); break;
		case TEXT('\\'): Out += TEXT("\\\\"); break;
		case TEXT('\n'): Out += TEXT("\\n"); break;
		case TEXT('\r'): Out += TEXT("\\r"); break;
		case TEXT('\t'): Out += TEXT("\\t"); break;
		default:
			if (C < 32)
			{
				Out += FString::Printf(TEXT("\\%03d"), (int32)C);
			}
			else
			{
				Out.AppendChar(C);
			}
		}
	}
	Out.AppendChar(TEXT('"'));
	return Out;
}

FString GameTestLuaChunk::ToLuaLiteral(const TSharedPtr<FJsonValue>& InValue)
{
	if (!InValue.IsValid())
	{
		return TEXT("nil");
	}
	switch (InValue->Type)
	{
	case EJson::Boolean:
		return InValue->AsBool() ? TEXT("true") : TEXT("false");
	case EJson::Number:
	{
		const double D = InValue->AsNumber();
		if (!FMath::IsFinite(D))
		{
			return TEXT("nil");
		}
		if (FMath::IsNearlyEqual(D, FMath::RoundToDouble(D)) && FMath::Abs(D) < 9007199254740992.0)
		{
			return FString::Printf(TEXT("%lld"), (long long)FMath::RoundToDouble(D));
		}
		return FString::Printf(TEXT("%.17g"), D);
	}
	case EJson::String:
		return QuoteLua(InValue->AsString());
	case EJson::Array:
	{
		TArray<FString> Items;
		for (const TSharedPtr<FJsonValue>& Item : InValue->AsArray())
		{
			Items.Add(ToLuaLiteral(Item));
		}
		return TEXT("{") + FString::Join(Items, TEXT(", ")) + TEXT("}");
	}
	case EJson::Object:
	{
		const TSharedPtr<FJsonObject> Object = InValue->AsObject();
		if (!Object.IsValid())
		{
			return TEXT("{}");
		}
		TArray<FString> Items;
		for (const auto& Pair : Object->Values)
		{
			Items.Add(FString::Printf(TEXT("[%s] = %s"), *QuoteLua(GAMETEST_JSON_KEY(Pair.Key)), *ToLuaLiteral(Pair.Value)));
		}
		return TEXT("{") + FString::Join(Items, TEXT(", ")) + TEXT("}");
	}
	default:
		return TEXT("nil");
	}
}

FString GameTestLuaChunk::BuildWrapper(const FString& InCode, const FString& InChunkName, const TMap<FString, TSharedPtr<FJsonValue>>& InArgs, const TSharedPtr<FJsonObject>& InLib)
{
	TArray<FString> Args;
	for (const auto& Pair : InArgs)
	{
		Args.Add(FString::Printf(TEXT("[%s] = %s"), *QuoteLua(Pair.Key), *ToLuaLiteral(Pair.Value)));
	}
	TArray<FString> Lib;
	if (InLib.IsValid())
	{
		for (const auto& Pair : InLib->Values)
		{
			const TSharedPtr<FJsonObject> Def = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
			if (!Def.IsValid())
			{
				continue;
			}
			TArray<FString> Params;
			const TArray<TSharedPtr<FJsonValue>>* ParamList = nullptr;
			if (Def->TryGetArrayField(TEXT("params"), ParamList) && ParamList != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& P : *ParamList)
				{
					FString Name;
					if (P.IsValid() && P->TryGetString(Name) && GameTestLuaChunkLocal::IsLuaName(Name))
					{
						Params.Add(QuoteLua(Name));
					}
				}
			}
			FString Body;
			Def->TryGetStringField(TEXT("body"), Body);
			Lib.Add(FString::Printf(TEXT("[%s] = { p = { %s }, b = %s }"), *QuoteLua(GAMETEST_JSON_KEY(Pair.Key)), *FString::Join(Params, TEXT(", ")), *QuoteLua(Body)));
		}
	}
	// 头几行把数据放进局部变量；用户代码由外壳 load，行号不受这几行影响
	return FString::Printf(TEXT("local __gtSrc, __gtChunk = %s, %s\nlocal __gtArgs = { %s }\nlocal __gtLib = { %s }"),
		*QuoteLua(InCode), *QuoteLua(InChunkName), *FString::Join(Args, TEXT(", ")), *FString::Join(Lib, TEXT(", ")))
		+ GameTestLuaChunkLocal::WrapperBody;
}

FString GameTestLuaChunk::BuildFnCall(const FString& InName, const TSharedPtr<FJsonObject>& InLib, FString& OutError)
{
	const TSharedPtr<FJsonObject>* Def = nullptr;
	if (!InLib.IsValid() || !InLib->TryGetObjectField(InName, Def) || Def == nullptr || !Def->IsValid())
	{
		OutError = FString::Printf(TEXT("函数库里没有 %s：面板存盘、运行前会把用到的函数写进 lib，先在面板里打开这份脚本存一次"), *InName);
		return FString();
	}
	TArray<FString> Params;
	const TArray<TSharedPtr<FJsonValue>>* ParamList = nullptr;
	if ((*Def)->TryGetArrayField(TEXT("params"), ParamList) && ParamList != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& P : *ParamList)
		{
			FString Name;
			if (P.IsValid() && P->TryGetString(Name) && GameTestLuaChunkLocal::IsLuaName(Name))
			{
				Params.Add(Name);
			}
		}
	}
	return FString::Printf(TEXT("return fn[%s](%s)"), *QuoteLua(InName), *FString::Join(Params, TEXT(", ")));
}
