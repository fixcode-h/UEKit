// 出包：node scripts/package.mjs [--plugin A,B] → dist/<插件>/
//   <插件>-<版本>-src.zip   只有源码，解压出 <插件>/ 目录，放进项目的 Plugins/
//   manifest.json           这个插件这一版的清单（catalog.json 里的一条就是它）
//   notes.md                Release 说明：CHANGELOG 这一节 + 上一版以来的提交列表
//   SHA256SUMS
// 压缩包里每个文件的时间固定、顺序固定，同一份源码出的包字节完全一样：发布时拿 sha256 判断内容变没变。
import { createHash } from 'node:crypto'
import { mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { REPO, pickPlugins, readChangelog, releaseNotes, root, tagOf, walk, zip } from './lib.mjs'

const dist = path.join(root, 'dist')
rmSync(dist, { recursive: true, force: true })

for (const p of pickPlugins(process.argv)) {
  const { kit, uplugin: up, version } = p
  // kit.json、CHANGELOG.md 是插件库自己的元数据，不进源码包（项目里只要 UE 认的那些文件）
  const files = walk(p.dir).filter((f) => !/^(kit\.json|CHANGELOG\.md)$/.test(path.relative(p.dir, f)))
  const entries = files.map((f) => ({
    name: `${p.name}/${path.relative(p.dir, f).replace(/\\/g, '/')}`,
    data: readFileSync(f),
  }))
  const asset = `${p.name}-${version}-src.zip`
  const buf = zip(entries)
  const sha256 = createHash('sha256').update(buf).digest('hex')
  const out = path.join(dist, p.name)
  mkdirSync(out, { recursive: true })
  writeFileSync(path.join(out, asset), buf)

  const notes = releaseNotes(p)
  const tag = tagOf(p.name, version)
  const manifest = {
    name: p.name,
    displayName: kit.displayName,
    description: kit.description,
    kind: kit.kind,
    version,
    versionCode: up.Version,
    ...(p.name === 'UELink' ? { protocol: readProtocol(p) } : {}),
    engineMin: kit.engineMin,
    engineTested: kit.engineTested ?? [],
    requires: kit.requires ?? {},
    enginePlugins: kit.enginePlugins ?? [],
    optionalPlugins: kit.optionalPlugins ?? [],
    modules: (up.Modules ?? []).map((m) => ({ name: m.Name, type: m.Type })),
    editorOnly: (up.Modules ?? []).every((m) => /Editor|UncookedOnly|Developer/.test(m.Type)),
    replaces: kit.replaces ?? [],
    panelTools: kit.panelTools ?? [],
    tag,
    asset,
    url: `https://github.com/${REPO}/releases/download/${tag}/${asset}`,
    sha256,
    size: buf.length,
    homepage: `https://github.com/${REPO}/tree/main/plugins/${p.name}`,
    notes: readChangelog(p).find((c) => c.version === version)?.body ?? '',
  }
  writeFileSync(path.join(out, 'manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`)
  writeFileSync(path.join(out, 'notes.md'), notes)
  const manifestSha = createHash('sha256').update(readFileSync(path.join(out, 'manifest.json'))).digest('hex')
  writeFileSync(path.join(out, 'SHA256SUMS'), `${sha256}  ${asset}\n${manifestSha}  manifest.json\n`)
  console.log(`✓ dist/${p.name}/${asset}（${entries.length} 个文件，${(buf.length / 1024).toFixed(0)} KB，sha256 ${sha256.slice(0, 12)}…）`)
}

function readProtocol(p) {
  const h = readFileSync(path.join(p.dir, 'Source/UELinkCore/Public/UELinkVersion.h'), 'utf8')
  return Number(h.match(/#define\s+UELINK_STREAM_PROTOCOL\s+(\d+)/)?.[1] ?? 2)
}
