// 5.8 起 FString::Printf / UE_LOG 在编译期核对格式串：占位个数和参数个数对不上、%s 给了 FString 本身
// 或给了 TCHAR（多写了一个 *）都直接编不过（GameTest 2.0.2 的 `**DescribeTarget(...)` 就是这样）。
// 这里查得出来的两类：个数不一致；%s 的参数是「** + 函数调用 / 成员」（函数返回 FString 时只该有一个 *）。
const SPEC = /%(?:%|[-+ #0]*(\d+|\*)?(?:\.(\d+|\*))?(?:hh|h|ll|l|L|z|j|t|I64)?([sSdiuoxXcfFeEgGp]))/g

/** 从 text[i]（左括号）起切出顶层参数 */
function splitArgs(text, i) {
  const args = []
  let depth = 0
  let cur = ''
  let quote = null
  for (let j = i; j < text.length; j++) {
    const c = text[j]
    if (quote) {
      cur += c
      if (c === '\\') {
        cur += text[++j]
        continue
      }
      if (c === quote) quote = null
    } else if (c === '"' || c === "'") {
      quote = c
      cur += c
    } else if ('([{'.includes(c)) {
      depth++
      if (depth > 1) cur += c
    } else if (')]}'.includes(c)) {
      depth--
      if (depth === 0) {
        args.push(cur.trim())
        return args
      }
      cur += c
    } else if (c === ',' && depth === 1) {
      args.push(cur.trim())
      cur = ''
    } else cur += c
  }
  return args
}

/** 返回问题说明（带行号） */
export function findPrintfIssues(text) {
  const out = []
  for (const m of text.matchAll(/\b(Printf|Appendf|UE_LOG)\s*\(/g)) {
    const args = splitArgs(text, m.index + m[0].length - 1)
    const at = args.findIndex((a) => /^TEXT\(\s*"(?:\\.|[^"\\])*"\s*\)$/s.test(a))
    if (at < 0) continue
    const fmt = /^TEXT\(\s*"((?:\\.|[^"\\])*)"\s*\)$/s.exec(args[at])[1]
    const rest = args.slice(at + 1)
    const need = []
    for (const s of fmt.matchAll(SPEC)) {
      if (s[0] === '%%') continue
      if (s[1] === '*') need.push('d')
      if (s[2] === '*') need.push('d')
      need.push(s[3])
    }
    const line = text.slice(0, m.index).split('\n').length
    if (need.length !== rest.length) {
      out.push(`第 ${line} 行格式串有 ${need.length} 个占位、${rest.length} 个参数`)
      continue
    }
    need.forEach((t, k) => {
      if ('sS'.includes(t) && /^\*\*[A-Za-z_][\w:]*\s*(\(|\.|->)/.test(rest[k]))
        out.push(`第 ${line} 行 %s 的参数 ${rest[k].slice(0, 40)} 多了一个 *（函数返回 FString 时只写一个 *）`)
    })
  }
  return out
}
