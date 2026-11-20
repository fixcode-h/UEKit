// 提交前和 CI 都跑：node scripts/check.mjs [--plugin A,B]。
// 云端编不了 UE 的 C++，这里只查编译前就能查出来的东西：版本号四处一致、模块目录和清单对得上、
// 包里不会混进编译产物、扩展插件声明了 UELink 依赖、扩展不自己开 HTTP 服务、旧名字没有漏改。
import { readFileSync } from 'node:fs'
import path from 'node:path'
import { listPlugins, pickPlugins, readChangelog, readVersionMacro, semver, walk } from './lib.mjs'
import { findWrappedSignatures } from './sig-wrap.mjs'
import { findUnityClashes } from './unity-names.mjs'

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
  const headerNames = new Set(files.filter((f) => f.endsWith('.h')).map((f) => f.replace(/^.*[\\/]/, '')))
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
    // JSON 取对象不直接链式解引用：GetObjectField 缺字段、AsObject 类型不对时都返回空指针，
    // 链式 -> 会让整个编辑器崩溃（AssetForge 属性差异的崩溃就是这样来的）。先存进变量判 IsValid()，
    // 或用 TryGetObjectField。测试模块不查（断言失败本来就该停）
    if (!/Tests\//.test(rel) && /(\bGetObjectField\s*\([^;]*?\)|\bAsObject\s*\(\s*\))\s*->/.test(text))
      fail(`${rel} 对 GetObjectField / AsObject() 的结果直接用了 ->：先判 IsValid()（或用 TryGetObjectField），取不到时返回错误，不要让编辑器崩溃`)
    // 5.8 起 GetObjectsWithOuter / ForEachObjectWithOuter 的 bool bIncludeNestedObjects 弃用（编译出 C4996 警告），
    // 一律写 UELINK_OUTER_NESTED(...)（UELinkCompat.h），5.4 上展开成 bool、5.8 上展开成 EGetObjectsFlags
    if (/GetObjectsWithOuter\s*\([^;]*,\s*(true|false)\s*\)/.test(text) || /ForEachObjectWithOuter(Breakable)?\s*\([\s\S]{0,800}?\}\s*,\s*(true|false)\s*[,)]/.test(text))
      fail(`${rel} 给 GetObjectsWithOuter / ForEachObjectWithOuter 直接传了 true/false：5.8 上会报弃用警告，改用 UELINK_OUTER_NESTED(...)`)
    // X.cpp 有同名头文件 X.h 时，第一个 include 必须是它：UBT 的头文件顺序检查会直接报
    // 「Expected X.h to be first header included」（UELink 2.10.0 的 AgentPanelLuaTrace.cpp 就是这样）
    if (f.endsWith('.cpp')) {
      const base = f.replace(/^.*[\\/]/, '').replace(/\.cpp$/, '')
      const first = text.match(/^\s*#\s*include\s*[<"]([^>"]+)[>"]/m)?.[1]
      if (headerNames.has(`${base}.h`) && first && first.replace(/^.*\//, '') !== `${base}.h`)
        fail(`${rel} 的第一个 include 是 ${first}：先 include 同名头文件 ${base}.h，UBT 会按顺序检查直接报错`)
    }
    // 函数签名写一行，参数超过 8 个才换行（见 sig-wrap.mjs）
    if (/\.(h|cpp|inl)$/.test(f)) {
      const wrapped = findWrappedSignatures(text)
      if (wrapped.length) fail(`${rel} 第 ${wrapped.join('、')} 行的函数签名换行了：参数不超过 8 个时写在一行里`)
    }
    // 命名空间里写 `class UWidget*` 会就地声明出 命名空间::UWidget，和引擎的 UWidget 不是一个类型（GameTest 2.0.0 的
    // GameTestState.h 就是这样，5.8 上报「无法从 GameTestStateUtil::UWidget* 转换」）：前置声明写在全局，签名里不带 class
    if (f.endsWith('.h') && /\(\s*(const\s+)?(class|struct)\s+[A-Z]\w+\s*[*&]|_API\s+(const\s+)?(class|struct)\s+[A-Z]\w+\s*[*&]/.test(text))
      fail(`${rel} 的签名里用了 class / struct 就地前置声明：在文件开头的全局作用域写 class X;，签名里只写 X*`)
    // 「最烦解析」：`const FKey Key(FName(*Name));` 声明的是函数不是变量（GameTest 2.0.0 的 GameTestCloseKey.cpp）
    if (/^\s*(const\s+)?[A-Z]\w+\s+\w+\(\s*[FUT][A-Z]\w*\(\s*\*?\w+\s*\)\s*\);/m.test(text))
      fail(`${rel} 有 \`T x(U(*y));\` 这种写法，会被解析成函数声明：改成 T x{ U(*y) };`)
    // CQTest 的测试类是 struct 定义，结尾少了分号时本文件报 C1004「意外的文件尾」，合并编译时还会连累下一个文件
    // （UE 5.4 上表现成 UMG 的 PanelSlot.h 语法错误，GameTest 1.3.0 就是这样）
    const testClasses = (text.match(/^TEST_CLASS\w*\s*\(/gm) ?? []).length
    if (testClasses && (text.match(/^\};/gm) ?? []).length < testClasses)
      fail(`${rel} 的 TEST_CLASS 结尾少了分号：类体最后一行写 };`)
    if (p.name === 'UELink') {
      if (/AGENTPANELBRIDGE(EDITOR)?_API/.test(text)) fail(`${rel} 还在用旧的导出宏 AGENTPANELBRIDGE_API，改成 UELINKCORE_API`)
      continue
    }
    // 扩展插件：操作挂到 UELink 的 /rpc 上，不自己开 HTTP 服务（自己开的没有令牌，也不一定只听本机）
    if (/GetHttpRouter\s*\(/.test(text)) fail(`${rel} 自己开了 HTTP 服务：扩展的操作用 FAgentPanelBridge::AddExtensionOp 挂到 UELink 上`)
    // 原项目的前缀 Tk 已经去掉，防止搬代码时漏改
    if (/\b[FUSE]?Tk[A-Z]\w*|TK(ASSETFORGE|GAMETEST)\w*_API/.test(text)) fail(`${rel} 还有原项目前缀 Tk 的名字`)
  }
  // 合并编译下的重名（见 unity-names.mjs）：按模块查，关了合并编译的模块不查
  for (const name of moduleNames) {
    const dir = path.join(p.dir, 'Source', name)
    const buildCs = files.find((f) => f === path.join(dir, `${name}.Build.cs`))
    if (buildCs && /bUseUnity\s*=\s*false/.test(readFileSync(buildCs, 'utf8'))) continue
    const cpps = files.filter((f) => f.startsWith(dir + path.sep) && f.endsWith('.cpp'))
    for (const clash of findUnityClashes(cpps, (f) => path.relative(p.dir, f).replace(/\\/g, '/')))
      fail(`合并编译重名 ${clash}：改个名字（合并编译时会报「不明确的符号」）`)
  }
  if (p.name === 'UELink') checkEditorSharedNamespace(p.dir, fail)
  for (const name of moduleNames) if (!implemented.has(name)) fail(`没有找到 IMPLEMENT_MODULE(..., ${name})`)
  if (!errors.some((e) => e.startsWith(`${p.name}：`)))
    console.log(`✓ ${p.name} ${up.VersionName}（Version ${up.Version}，UE ${kit.engineMin}+）检查通过，${files.length} 个文件`)
}

// UELinkEditor 的共用工具（NoteDirty、FindClassByName、FTx……）都在 AgentPanelEditorShared.h 的 namespace AgentPanelEd 里。
// 只 include 了头文件、代码却写在 namespace AgentPanel 里直接用它们会「找不到标识符」（UELink 2.14.0 的 UELinkBpNames.cpp、
// UELinkTableMulti.cpp 就是这样）：要么文件里 using namespace AgentPanelEd，要么写全名 AgentPanelEd::X
function checkEditorSharedNamespace(dir, fail) {
  const edDir = path.join(dir, 'Source/UELinkEditor/Private')
  let header
  try {
    header = readFileSync(path.join(edDir, 'AgentPanelEditorShared.h'), 'utf8')
  } catch {
    return
  }
  const body = header.slice(header.indexOf('namespace AgentPanelEd'))
  const names = new Set()
  for (const m of body.matchAll(/^\t(?:inline\s+)?[\w:<>*&, ]+?[\s*&]+(\w+)\s*\(/gm)) names.add(m[1])
  for (const m of body.matchAll(/^\tstruct (\w+)/gm)) names.add(m[1])
  for (const f of walk(edDir).filter((x) => x.endsWith('.cpp'))) {
    const text = readFileSync(f, 'utf8')
    // using 了，或者自己就在 namespace AgentPanelEd 里定义这些工具的文件不查
    if (/using\s+namespace\s+AgentPanelEd\s*;|namespace\s+AgentPanelEd\s*\{?\s*$/m.test(text)) continue
    const rel = path.relative(dir, f).replace(/\\/g, '/')
    text.split('\n').forEach((line, i) => {
      if (/^\s*(\/\/|\*)/.test(line)) return
      for (const n of names)
        if (new RegExp(`(^|[^\\w:.>])${n}\\b`).test(line))
          fail(`${rel} 第 ${i + 1} 行直接用了 ${n}：它在 namespace AgentPanelEd 里，写成 AgentPanelEd::${n} 或在文件开头 using namespace AgentPanelEd`)
    })
  }
}

if (errors.length) {
  console.error(errors.map((e) => `✗ ${e}`).join('\n'))
  process.exit(1)
}
