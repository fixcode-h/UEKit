// check / package / catalog 共用：列出插件、读清单和版本、写 zip、整理更新说明。只用 Node 自带模块，CI 不用 npm install。
import { execFileSync } from 'node:child_process'
import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { deflateRawSync } from 'node:zlib'

export const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
export const pluginsDir = path.join(root, 'plugins')
export const REPO = 'fixcode-h/UEKit'
export const semver = /^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/

/**
 * 一个插件：plugins/<名字>/ 下有 <名字>.uplugin（UE 认的清单）和 kit.json（插件库自己的元数据：
 * 说明、引擎范围、依赖、给客户端看的东西）。两份分开是因为 .uplugin 的字段由 UE 定，不能随便加。
 */
export function listPlugins() {
  return readdirSync(pluginsDir)
    .filter((n) => existsSync(path.join(pluginsDir, n, 'kit.json')))
    .sort((a, b) => (a === 'UELink' ? -1 : b === 'UELink' ? 1 : a.localeCompare(b)))
    .map(loadPlugin)
}

export function loadPlugin(name) {
  const dir = path.join(pluginsDir, name)
  const kit = JSON.parse(readFileSync(path.join(dir, 'kit.json'), 'utf8'))
  const uplugin = JSON.parse(readFileSync(path.join(dir, `${name}.uplugin`), 'utf8').replace(/^﻿/, ''))
  return { name, dir, kit, uplugin, version: uplugin.VersionName }
}

/** kit.json 的 versionHeader 指向的宏：#define <macro> TEXT("x.y.z") */
export function readVersionMacro(p) {
  const h = p.kit.versionHeader
  if (!h) return undefined
  const file = path.join(p.dir, h.file)
  if (!existsSync(file)) return undefined
  const m = readFileSync(file, 'utf8').match(new RegExp(`#define\\s+${h.macro}\\s+TEXT\\("([^"]+)"\\)`))
  return m?.[1]
}

/** CHANGELOG.md 的各节：[{ version, body }]，最新的在前 */
export function readChangelog(p) {
  const file = path.join(p.dir, 'CHANGELOG.md')
  if (!existsSync(file)) return []
  return readFileSync(file, 'utf8')
    .split(/^## v/m)
    .slice(1)
    .map((part) => {
      const nl = part.indexOf('\n')
      return { version: part.slice(0, nl).trim(), body: part.slice(nl + 1).trim() }
    })
}

export const tagOf = (name, version) => `${name}-v${version}`
export const isPrerelease = (version) => version.includes('-')

export function walk(dir, base = dir, out = []) {
  for (const n of readdirSync(dir).sort()) {
    const p = path.join(dir, n)
    const rel = path.relative(base, p).replace(/\\/g, '/')
    if (/^(Binaries|Intermediate|Saved|DerivedDataCache)(\/|$)/.test(rel)) continue
    if (statSync(p).isDirectory()) walk(p, base, out)
    else out.push(p)
  }
  return out
}

function git(args) {
  try {
    return execFileSync('git', args, { cwd: root, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim()
  } catch {
    return ''
  }
}

/** 这个插件上一个发布标签（按版本号最新的那个）；没有返回空 */
export function lastTag(name, exclude) {
  return (
    git(['tag', '--list', `${name}-v*`, '--sort=-v:refname'])
      .split('\n')
      .filter((t) => t && t !== exclude)[0] ?? ''
  )
}

/**
 * 上一版以来改了这个插件目录的提交（标题一行一条）。Release 说明靠它列出「这个包具体更新了什么」：
 * CHANGELOG 写的是给人看的归纳，提交列表是完整的流水，两样都给。
 * 内部提交（标题以「内部：」开头）跳过，和面板的更新说明规则一致。
 */
export function commitsSince(p, tag) {
  const range = tag ? [`${tag}..HEAD`] : []
  const out = git(['log', '--no-merges', '--format=%h\t%s', ...range, '--', path.relative(root, p.dir)])
  return out
    .split('\n')
    .filter(Boolean)
    .map((l) => {
      const [hash, ...rest] = l.split('\t')
      return { hash, subject: rest.join('\t') }
    })
    .filter((c) => !c.subject.startsWith('内部：'))
}

/** Release 说明：这一版的 CHANGELOG 节 + 上一版以来的提交列表 + 安装提示 */
export function releaseNotes(p) {
  const section = readChangelog(p).find((c) => c.version === p.version)?.body ?? ''
  const prev = lastTag(p.name, tagOf(p.name, p.version))
  const commits = commitsSince(p, prev)
  const lines = [section]
  lines.push('', `### 这个包包含的改动${prev ? `（${prev} 以来）` : ''}`, '')
  if (commits.length) for (const c of commits) lines.push(`- ${c.subject}（${c.hash}）`)
  else lines.push('- （没有找到提交记录：浅克隆或第一次发布）')
  lines.push(
    '',
    '---',
    `解压后把 \`${p.name}\` 目录放进项目的 \`Plugins/\`，关掉编辑器后完整编译一次（Live Coding 不够）。Agent 面板的 UE 页「UE 插件」可以直接安装和升级。`,
  )
  return lines.join('\n').trim() + '\n'
}

// ---- 最小的 zip 写入（deflate），不依赖第三方包。每个文件的时间固定、顺序固定：同一份源码出的包字节完全一样
const crcTable = new Uint32Array(256).map((_, n) => {
  let c = n
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1
  return c >>> 0
})
function crc32(buf) {
  let c = 0xffffffff
  for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8)
  return (c ^ 0xffffffff) >>> 0
}
const DOS_TIME = 0
const DOS_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1

export function zip(entries) {
  const locals = []
  const centrals = []
  let offset = 0
  for (const { name, data } of entries) {
    const nameBuf = Buffer.from(name, 'utf8')
    const comp = deflateRawSync(data, { level: 9 })
    const crc = crc32(data)
    const local = Buffer.alloc(30)
    local.writeUInt32LE(0x04034b50, 0)
    local.writeUInt16LE(20, 4)
    local.writeUInt16LE(0x0800, 6)
    local.writeUInt16LE(8, 8)
    local.writeUInt16LE(DOS_TIME, 10)
    local.writeUInt16LE(DOS_DATE, 12)
    local.writeUInt32LE(crc, 14)
    local.writeUInt32LE(comp.length, 18)
    local.writeUInt32LE(data.length, 22)
    local.writeUInt16LE(nameBuf.length, 26)
    locals.push(local, nameBuf, comp)
    const central = Buffer.alloc(46)
    central.writeUInt32LE(0x02014b50, 0)
    central.writeUInt16LE(20, 4)
    central.writeUInt16LE(20, 6)
    central.writeUInt16LE(0x0800, 8)
    central.writeUInt16LE(8, 10)
    central.writeUInt16LE(DOS_TIME, 12)
    central.writeUInt16LE(DOS_DATE, 14)
    central.writeUInt32LE(crc, 16)
    central.writeUInt32LE(comp.length, 20)
    central.writeUInt32LE(data.length, 24)
    central.writeUInt16LE(nameBuf.length, 28)
    central.writeUInt32LE(offset, 42)
    centrals.push(central, nameBuf)
    offset += 30 + nameBuf.length + comp.length
  }
  const centralBuf = Buffer.concat(centrals)
  const end = Buffer.alloc(22)
  end.writeUInt32LE(0x06054b50, 0)
  end.writeUInt16LE(entries.length, 8)
  end.writeUInt16LE(entries.length, 10)
  end.writeUInt32LE(centralBuf.length, 12)
  end.writeUInt32LE(offset, 16)
  return Buffer.concat([...locals, centralBuf, end])
}

/** 命令行参数：--plugin A,B（默认全部） */
export function pickPlugins(argv) {
  const all = listPlugins()
  const i = argv.indexOf('--plugin')
  const want = i >= 0 ? (argv[i + 1] ?? '').split(',').filter((s) => s && s !== 'all') : []
  if (!want.length) return all
  const unknown = want.filter((w) => !all.some((p) => p.name === w))
  if (unknown.length) throw new Error(`没有这个插件：${unknown.join('、')}（有：${all.map((p) => p.name).join('、')}）`)
  return all.filter((p) => want.includes(p.name))
}
