// C++ 原始字符串 R"(...)" 遇到第一个 `)"` 就结束：内容里有 `()"`（例如 JSON 里的 "@.GetCount()"）时
// 后半截变成代码，MSVC 报 C4002「TEXT 参数过多」/ C2001「常量中有换行符」（GameTest 2.1.6 的 GameTestRefTest.cpp）。
// 这里查：不带自定义分隔符的 R"(，到第一个 `)"` 为止的内容里引号不成对（截在了某个字符串中间）。内容里有 `)"` 时改用 R"JSON(...)JSON"。

/** 返回问题说明（带行号） */
export function findRawStringIssues(text) {
  const out = []
  for (const m of text.matchAll(/\bR"\(/g)) {
    const start = m.index + m[0].length
    const end = text.indexOf(')"', start)
    if (end < 0) continue
    // 测试里的原始字符串都是 JSON：正常结束时内容里的引号成对；单数说明是在某个 "…()" 字符串中间截断了
    const quotes = (text.slice(start, end).match(/"/g) ?? []).length
    if (quotes % 2) {
      const line = text.slice(0, m.index).split('\n').length
      out.push(`第 ${line} 行的原始字符串 R"(...)" 内容里有 \`)"\`，会提前结束：改用自定义分隔符，如 R"JSON(...)JSON"`)
    }
  }
  return out
}
