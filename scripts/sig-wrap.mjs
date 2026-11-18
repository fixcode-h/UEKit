// 函数声明和定义的签名写在一行里，参数超过 8 个才允许换行（用户 2026-11-18 定的风格）。
// 一个参数一行看着整齐，但头文件里一屏只剩几个函数，读接口要来回翻；所以统一写一行。
// 这里只认「返回类型 + 名字(」开头、每段都是「类型 名字[= 默认值]」的参数表：
// 函数调用、构造临时对象的实参是表达式，不会被误判。
const KEYWORD = /^\s*(return|if|else|for|while|switch|case|do|new|delete|throw|co_return|#|\/\/|\*|\/\*)\b/
const HEAD = /^(\s*)(?:template\s*<[^>]*>\s*)?((?:[\w:<>,*&\s]+?)[\s*&]+)?(~?[A-Za-z_][\w:]*)\s*\((.*,)?\s*$/
const PARAM =
  /^\s*(?:(?:const|volatile|class|struct|enum|typename)\s+)*[A-Za-z_][\w:]*(?:\s*<.*>)?(?:\s*const)?[\s*&]+[A-Za-z_]\w*(?:\s*\[[^\]]*\])?(?:\s*=\s*[^,;{}]+)?\s*,?\s*$|^\s*\.\.\.\s*$/
const TAIL = /^\)\s*(const\b|override\b|final\b|noexcept\b|PURE_VIRTUAL|;|\{|:|$|=\s*(0|default|delete)\s*;|->)/

// 按最外层逗号切参数（模板实参、默认值里的括号和字符串不切）
function splitTop(s) {
  const out = []
  let depth = 0
  let quote = false
  let cur = ''
  for (const ch of s) {
    if (ch === '"') quote = !quote
    if (!quote && '(<['.includes(ch)) depth++
    if (!quote && ')>]'.includes(ch)) depth--
    if (ch === ',' && depth === 0 && !quote) {
      out.push(cur.trim())
      cur = ''
    } else cur += ch
  }
  if (cur.trim()) out.push(cur.trim())
  return out
}

/** 返回换了行、参数又不超过 8 个的签名所在行号（从 1 开始） */
export function findWrappedSignatures(text) {
  const lines = text.replace(/\r\n/g, '\n').split('\n')
  const hits = []
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i]
    const m = HEAD.exec(line)
    if (!m || KEYWORD.test(line) || line.slice(0, line.indexOf('(')).includes('=')) continue
    const name = m[3]
    const rtype = (m[2] ?? '').trim()
    const parts = name.split('::')
    const ctor = parts.length >= 2 && parts.at(-1).replace(/^~/, '') === parts.at(-2)
    if (!rtype && !ctor) continue
    if (/\b(return|else|new|delete|co_await)$/.test(rtype)) continue
    const params = m[4] ? splitTop(m[4]) : []
    if (line.includes('//') || !params.every((p) => PARAM.test(p))) continue
    let closed = false
    for (let j = i + 1; j < lines.length && j - i <= 20; j++) {
      const s = lines[j].trim()
      if (s.startsWith('//')) continue
      const k = s.lastIndexOf(')')
      const head = k >= 0 ? s.slice(0, k) : s
      if (k >= 0 && head.split('(').length === head.split(')').length) {
        if (!TAIL.test(s.slice(k))) break
        params.push(...splitTop(head))
        closed = true
        break
      }
      params.push(...splitTop(s))
    }
    if (closed && params.length && params.length <= 8 && params.every((p) => PARAM.test(p))) hits.push(i + 1)
  }
  return hits
}
