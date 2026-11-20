// 合并编译（unity build）下的重名检查。
// UBT 把同一个模块的几个 .cpp 拼成一个 Module.X.N.cpp 编：匿名命名空间里的名字在拼起来的整个文件里都看得见，
// 别的文件在函数里 `using namespace XxxImpl;` 再用同名的东西就会报「不明确的符号」（GameTest 2.0.0 的
// GameTestRpcVars.cpp 的 FGroup 和 GameTestRpcProps.cpp 的 GameTestRpcPropsImpl::FGroup 就是这样）。
// 哪些文件拼在一起随文件数变化，以前编过不代表以后编得过，所以按模块整体查：
// 匿名命名空间（或文件级 using 的命名空间）里的名字 ∩ 被 `using namespace` 的命名空间里的名字 = 不许有。
import { readFileSync } from 'node:fs'

const DECL =
  /^(?:template\s*<[^>]*>\s*)?(?:static\s+)?(?:constexpr\s+|const\s+|inline\s+)*(?:(?:struct|class|enum class|enum|using)\s+(\w+)|[\w:<>,*& ]+?[\s*&](\w+)\s*(?:\(|=|\[|;|\{))/

function strip(text) {
  return text
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/\/\/[^\n]*/g, '')
    .replace(/"(?:\\.|[^"\\\n])*"/g, '""')
}

/** 命名空间体里最外层声明的名字 */
function topNames(body) {
  const out = new Set()
  let depth = 0
  let line = ''
  const take = (s) => {
    const m = DECL.exec(s.replace(/\s+/g, ' ').trim())
    const name = m && (m[1] ?? m[2])
    if (name) out.add(name)
  }
  for (const ch of body) {
    if (ch === '{') {
      if (depth === 0) take(line)
      depth++
      line = ''
    } else if (ch === '}') {
      depth--
      line = ''
    } else if (ch === ';' && depth === 0) {
      take(line + ';')
      line = ''
    } else if (depth === 0) line += ch
  }
  return out
}

/** files：同一个模块的 .cpp（绝对路径）；rel：显示用的相对路径。返回冲突说明 */
export function findUnityClashes(files, rel) {
  const local = new Map() // 名字 → 文件（匿名命名空间、文件级 using 的命名空间）
  const used = new Map() // 名字 → 文件（函数里 using 的命名空间）
  const add = (map, name, file) => map.set(name, [...(map.get(name) ?? []), file])
  for (const f of files) {
    const src = strip(readFileSync(f, 'utf8'))
    const fileUsing = new Set([...src.matchAll(/^using namespace (\w+);/gm)].map((m) => m[1]))
    const anyUsing = new Set([...src.matchAll(/using namespace (\w+);/g)].map((m) => m[1]))
    for (const m of src.matchAll(/^namespace(?:[ \t]+(\w+))?\s*\{/gm)) {
      let i = m.index + m[0].length
      let depth = 1
      const start = i
      while (depth && i < src.length) {
        if (src[i] === '{') depth++
        else if (src[i] === '}') depth--
        i++
      }
      const ns = m[1]
      const names = topNames(src.slice(start, i - 1))
      const target = !ns || fileUsing.has(ns) ? local : anyUsing.has(ns) ? used : null
      if (target) for (const n of names) add(target, n, rel(f))
    }
  }
  const out = []
  for (const [name, at] of local) {
    const other = used.get(name)?.filter((f) => !at.includes(f))
    if (other?.length) out.push(`${name}：${at.join('、')} 的匿名命名空间 和 ${other.join('、')} 里 using 的命名空间`)
  }
  return out
}
