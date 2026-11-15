// 更新插件目录：node scripts/catalog.mjs dist/<插件>/manifest.json ...
// 发布工作流在发完 Release 后跑，把刚发布的清单并进仓库根目录的 catalog.json，再提交回 main。
// 客户端（Agent 面板 UE 插件、UEToVS）只读这一份：https://raw.githubusercontent.com/fixcode-h/UEKit/main/catalog.json
// （镜像照原样放 catalog.json 和各插件的源码包即可）。
//
// 每个插件一条：latest 是最新正式版，prerelease 是比它新的预发布版（没有就是 null）。客户端默认只装 latest。
import { existsSync, readFileSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { REPO, isPrerelease, listPlugins, root } from './lib.mjs'

const file = path.join(root, 'catalog.json')
const catalog = existsSync(file) ? JSON.parse(readFileSync(file, 'utf8')) : { schema: 1, repo: REPO, plugins: [] }

const cmp = (a, b) => {
  const [ma, pa = ''] = a.split('-')
  const [mb, pb = ''] = b.split('-')
  const na = ma.split('.').map(Number)
  const nb = mb.split('.').map(Number)
  for (let i = 0; i < 3; i++) if (na[i] !== nb[i]) return na[i] - nb[i]
  if (!pa || !pb) return pa ? -1 : pb ? 1 : 0
  return pa.localeCompare(pb, undefined, { numeric: true })
}

for (const f of process.argv.slice(2)) {
  const m = JSON.parse(readFileSync(f, 'utf8'))
  let entry = catalog.plugins.find((e) => e.name === m.name)
  if (!entry) {
    entry = { name: m.name, latest: null, prerelease: null }
    catalog.plugins.push(entry)
  }
  Object.assign(entry, { displayName: m.displayName, description: m.description, kind: m.kind })
  if (isPrerelease(m.version)) entry.prerelease = m
  else {
    if (!entry.latest || cmp(m.version, entry.latest.version) >= 0) entry.latest = m
    // 正式版发了以后，比它旧的预发布版不再列出
    if (entry.prerelease && cmp(entry.prerelease.version, m.version) <= 0) entry.prerelease = null
  }
}

// 顺序和仓库里的插件目录一致（UELink 在最前）
const order = listPlugins().map((p) => p.name)
catalog.plugins.sort((a, b) => order.indexOf(a.name) - order.indexOf(b.name))
catalog.updatedAt = new Date().toISOString()
writeFileSync(file, `${JSON.stringify(catalog, null, 2)}\n`)
console.log(`✓ catalog.json：${catalog.plugins.map((e) => `${e.name} ${e.latest?.version ?? '-'}${e.prerelease ? ` / ${e.prerelease.version}` : ''}`).join('，')}`)
