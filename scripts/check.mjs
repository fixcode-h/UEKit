// 提交前和 CI 都跑：node scripts/check.mjs [--plugin A,B]。
// 云端编不了 UE 的 C++，这里只查编译前就能查出来的东西：版本号四处一致、模块目录和清单对得上、
// 包里不会混进编译产物、扩展插件声明了 UELink 依赖、扩展不自己开 HTTP 服务、旧名字没有漏改。
import { readFileSync } from 'node:fs'
import path from 'node:path'
import { listPlugins, pickPlugins, readChangelog, readVersionMacro, semver, walk } from './lib.mjs'

const errors = []
const all = listPlugins()
const plugins = pickPlugins(process.argv)

for (const p of plugins) {
  const fail = (msg) => errors.push(`${p.name}：${msg}`)
  const { kit, uplugin: up } = p

  // ---- 版本：.uplugin、版本宏、CHANGELOG 最上面一节
  if (!semver.test(up.VersionName ?? '')) fail(`.uplugin 的 VersionName 不是语义化版本：${up.VersionName}`)
  if (!Number.isInteger(up.Version) || up.Version < 1) fail('.uplugin 的 Version 要是正整数，每发一版加 1')
  const macro = readVersionMacro(p)
  if (kit.versionHeader && macro !== up.VersionName)
    fail(`${kit.versionHeader.file} 的 ${kit.versionHeader.macro}（${macro}）和 .uplugin 的 VersionName（${up.VersionName}）不一致`)
  const log = readChangelog(p)
  if (!log.length) fail('CHANGELOG.md 里没有 ## vX.Y.Z 的版本节')
  else if (log[0].version !== up.VersionName)
    fail(`CHANGELOG.md 最上面一节是 v${log[0].version}，插件版本是 ${up.VersionName}：改了插件就在最上面加新版本节`)
  else if (!log[0].body) fail(`CHANGELOG.md 的 v${log[0].version} 没写内容`)

  // ---- kit.json
  for (const k of ['displayName', 'description', 'kind', 'engineMin']) if (!kit[k]) fail(`kit.json 缺 ${k}`)
  if (!['base', 'extension'].includes(kit.kind)) fail(`kit.json 的 kind 只能是 base / extension：${kit.kind}`)
  // .uplugin 不写 EngineVersion：写了以后别的引擎版本（哪怕 5.4.0 写成后在 5.4.1、5.8 打开）都会弹「插件不兼容」。
  // 最低引擎版本只记在 kit.json 的 engineMin，面板安装时按它拦。
  if (up.EngineVersion !== undefined) fail('.uplugin 不要写 EngineVersion（别的引擎版本打开会弹「插件不兼容」），最低版本只写 kit.json 的 engineMin')
  if (!/^\d+\.\d+$/.test(kit.engineMin ?? '')) fail(`kit.json 的 engineMin 要写成「主.次」，例如 5.4：${kit.engineMin}`)
  for (const dep of Object.keys(kit.requires ?? {})) {
    if (!all.some((o) => o.name === dep)) fail(`kit.json 的 requires 里的 ${dep} 不在插件库里`)
    if (!(up.Plugins ?? []).some((x) => x.Name === dep && x.Enabled !== false && !x.Optional))
      fail(`.uplugin 的 Plugins 里没有声明依赖 ${dep}（UE 不知道要先加载它）`)
  }
  if (kit.kind === 'extension' && !kit.requires?.UELink) fail('扩展插件要在 kit.json 的 requires 里写 UELink 的最低版本')

  // ---- 模块：清单里的每个模块都有 Source/<名字>/<名字>.Build.cs，Source 下没有清单外的目录
  const srcDir = path.join(p.dir, 'Source')
  const moduleNames = (up.Modules ?? []).map((m) => m.Name)
  const files = walk(p.dir)
  const rels = files.map((f) => path.relative(p.dir, f).replace(/\\/g, '/'))
  for (const name of moduleNames) {
    const buildCs = `Source/${name}/${name}.Build.cs`
    if (!rels.includes(buildCs)) {
      fail(`缺 ${buildCs}`)
      continue
    }
    if (!new RegExp(`public\\s+class\\s+${name}\\s*:\\s*ModuleRules`).test(readFileSync(path.join(p.dir, buildCs), 'utf8')))
      fail(`${name}.Build.cs 的类名不是 ${name}`)
  }
  // UBT 读 .uplugin 时这几个列表填错一个值整个项目就编不过（例如把配置名 Shipping 写进 TargetDenyList）
  const TARGET_TYPES = ['Game', 'Server', 'Client', 'Editor', 'Program']
  const CONFIGS = ['Debug', 'DebugGame', 'Development', 'Shipping', 'Test']
  for (const m of up.Modules ?? []) {
    for (const key of ['TargetAllowList', 'TargetDenyList'])
      for (const v of m[key] ?? [])
        if (!TARGET_TYPES.includes(v)) fail(`模块 ${m.Name} 的 ${key} 里「${v}」不是目标类型（只能是 ${TARGET_TYPES.join('、')}）`)
    for (const key of ['TargetConfigurationAllowList', 'TargetConfigurationDenyList'])
      for (const v of m[key] ?? [])
        if (!CONFIGS.includes(v)) fail(`模块 ${m.Name} 的 ${key} 里「${v}」不是编译配置（只能是 ${CONFIGS.join('、')}）`)
  }
  for (const rel of rels) {
    const m = rel.match(/^Source\/([^/]+)\//)
    if (m && !moduleNames.includes(m[1])) fail(`Source/${m[1]} 不在 .uplugin 的 Modules 里`)
  }

  // ---- 逐个源文件
  const implemented = new Set()
  for (let i = 0; i < files.length; i++) {
    const f = files[i]
    const rel = rels[i]
    if (!/\.(h|cpp|cs|inl)$/.test(f)) continue
    const text = readFileSync(f, 'utf8')
    if (text.charCodeAt(0) === 0xfeff) fail(`${rel} 带 BOM：统一用不带 BOM 的 UTF-8`)
    for (const m of text.matchAll(/IMPLEMENT_MODULE\s*\(\s*\w+\s*,\s*(\w+)\s*\)/g)) implemented.add(m[1])
    // 版本差异只写在 Compat 头里：业务代码不直接判断引擎版本（UELink 也一样）
    if (!/Compat\.h$/.test(rel) && /#\s*if[^\n]*ENGINE_(MAJOR|MINOR)_VERSION|UE_VERSION_(NEWER|OLDER)_THAN/.test(text))
      fail(`${rel} 直接判断了引擎版本：版本差异写进 <插件>Compat.h，这里只用它定义的宏`)
    if (p.name === 'UELink') {
      if (/AGENTPANELBRIDGE(EDITOR)?_API/.test(text)) fail(`${rel} 还在用旧的导出宏 AGENTPANELBRIDGE_API，改成 UELINKCORE_API`)
      continue
    }
    // 扩展插件：操作挂到 UELink 的 /rpc 上，不自己开 HTTP 服务（自己开的没有令牌，也不一定只听本机）
    if (/GetHttpRouter\s*\(/.test(text)) fail(`${rel} 自己开了 HTTP 服务：扩展的操作用 FAgentPanelBridge::AddExtensionOp 挂到 UELink 上`)
    // 原项目的前缀 Tk 已经去掉，防止搬代码时漏改
    if (/\b[FUSE]?Tk[A-Z]\w*|TK(ASSETFORGE|GAMETEST)\w*_API/.test(text)) fail(`${rel} 还有原项目前缀 Tk 的名字`)
  }
  for (const name of moduleNames) if (!implemented.has(name)) fail(`没有找到 IMPLEMENT_MODULE(..., ${name})`)
  if (!errors.some((e) => e.startsWith(`${p.name}：`)))
    console.log(`✓ ${p.name} ${up.VersionName}（Version ${up.Version}，UE ${kit.engineMin}+）检查通过，${files.length} 个文件`)
}

if (errors.length) {
  console.error(errors.map((e) => `✗ ${e}`).join('\n'))
  process.exit(1)
}
