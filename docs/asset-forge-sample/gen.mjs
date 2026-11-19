// 生成 AssetForge 示例测试界面的配置和配套 GameTest 场景，输出到 plugins/AssetForge/docs/AssetForge/Test。
// 界面有上千个控件，手改容易漏改名字和选择器路径，所以由这个脚本生成：
// 改界面就改 ui-*.mjs 再跑 `node docs/asset-forge-sample/gen.mjs`，生成的 JSON 一起提交（改了哪份就把哪份的 revision 加 1）。
//
// AssetForge 1.5.0 起每一页是一个子 WBP（umg/pages/WBP_ForgeSample_<页>.json），父界面的 Pages 里用 ref 引用：
// 一份配置不会大到没法看，导入父界面时 AssetForge 先把引用的子界面导入。场景里跨子界面的选择器写成
// `父界面路径/Page_<页>>子界面根/…`（`>` 后面从子界面的根控件名开始）。
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { T, fill, flat, hbox, margin, names, node, vbox } from './ui-kit.mjs'
import { sidebar, topbar, pageHome, pageInventory, pageShop, pageTasks, pageTaskDetail, pageSettings, pageProfile, pageLists, popups } from './ui-pages.mjs'
import { pageControls, pageFlow } from './ui-new-pages.mjs'
import { buildScenarios } from './scenarios.mjs'

const OUT = process.argv[2] ?? path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../plugins/AssetForge/docs/AssetForge/Test')
const ASSET_DIR = '/Game/AgentTest/AssetForge'
const ASSET = ASSET_DIR + '/WBP_ForgeSample'
const MAP = ASSET_DIR + '/Map_ForgeSample'

// ---- 子页面：[页名, 子界面根控件, revision]。顺序就是 Pages 切换器里的顺序，首页必须第一个（ResetScreen 切回第 0 页） ----
const PAGES = [
  ['Home', pageHome, 1],
  ['Inventory', pageInventory, 1],
  ['Shop', pageShop, 1],
  ['Tasks', pageTasks, 1],
  ['TaskDetail', pageTaskDetail, 1],
  ['Settings', pageSettings, 1],
  ['Profile', pageProfile, 1],
  ['Lists', pageLists, 1],
  ['Controls', pageControls, 1],
  ['Flow', pageFlow, 1],
]
const pageFile = (id) => `pages/WBP_ForgeSample_${id}.json`
const pageRef = (id) => {
  const name = 'Page_' + id
  if (names.has(name)) throw new Error('重名：' + name)
  names.add(name)
  return { name, ref: './' + pageFile(id) }
}

// ---- 整体 ----
const root = node('Overlay', 'Root', {}, [
  node('Border', 'Bg', flat(T.bg), null, { HorizontalAlignment: 'HAlign_Fill', VerticalAlignment: 'VAlign_Fill' }),
  hbox(
    'Shell',
    [
      { ...sidebar, slot: { HorizontalAlignment: 'HAlign_Left', VerticalAlignment: 'VAlign_Fill' } },
      vbox('Main', [topbar, node('WidgetSwitcher', 'Pages', {}, PAGES.map(([id]) => pageRef(id)), { ...fill(), Padding: margin(28, 24) })], fill()),
    ],
    { HorizontalAlignment: 'HAlign_Fill', VerticalAlignment: 'VAlign_Fill' },
  ),
  ...popups,
])

const umg = {
  '//': '由 AssetForge 插件自带的示例测试界面：多页面、跳页、弹窗、列表、各类控件、脚本流程靶子，用来练习 GameTest 录制和脚本。每一页是 pages/ 下的一个子界面（ref 引用），导入这份时会先导入它们。跳页逻辑在父类 ForgeSampleScreen 里，按控件名约定工作，说明见同目录的 README.md。',
  kind: 'umg-widget',
  schemaVersion: 1,
  revision: 4,
  asset: ASSET,
  parent: '/Script/AssetForgeTests.ForgeSampleScreen',
  root,
}
const pageConfigs = PAGES.map(([id, pageRoot, revision]) => {
  const { slot: _slot, ...bare } = pageRoot
  return [
    id,
    {
      '//': `示例界面的「${id}」页，被 ../WBP_ForgeSample.json 的 Pages 引用（Page_${id}）。按钮的行为由父界面 ForgeSampleScreen 按名字统一处理，这里只摆控件；控件名在父界面和所有子页面里都不能重名。`,
      kind: 'umg-widget',
      schemaVersion: 1,
      revision,
      asset: `${ASSET_DIR}/Pages/WBP_ForgeSample_${id}`,
      parent: '/Script/UMG.UserWidget',
      root: bare,
    },
  ]
})
const map = {
  '//': '示例测试地图：只有世界设置，GameMode 进图就显示 WBP_ForgeSample。项目的默认世界设置类不是 WorldSettings 时，把 root.class 换成项目用的那个类。',
  kind: 'map',
  schemaVersion: 1,
  revision: 1,
  asset: MAP,
  root: { class: '/Script/Engine.WorldSettings', name: 'WorldSettings', props: { DefaultGameMode: '/Script/AssetForgeTests.ForgeSampleGameMode' } },
}

// ---- 核对 slot 写法：每种父面板的插槽认的属性不一样，写错了演练会拒（BorderSlot 没有 Size） ----
const ALIGN = ['Padding', 'HorizontalAlignment', 'VerticalAlignment']
const SLOT_KEYS = {
  Overlay: ALIGN,
  HorizontalBox: ['Size', ...ALIGN],
  VerticalBox: ['Size', ...ALIGN],
  Border: ALIGN,
  Button: ALIGN,
  SizeBox: ALIGN,
  ScrollBox: ALIGN,
  WidgetSwitcher: ALIGN,
  UniformGridPanel: ['Row', 'Column', 'HorizontalAlignment', 'VerticalAlignment'],
  CheckBox: [],
  MenuAnchor: [],
}
const slotErrors = []
const checkSlots = (n) => {
  for (const c of n.children ?? []) {
    const allowed = SLOT_KEYS[n.class]
    if (!allowed) slotErrors.push(`没登记 ${n.class} 的插槽属性`)
    else for (const k of Object.keys(c.slot ?? {})) if (!allowed.includes(k)) slotErrors.push(`${c.name} 的 slot.${k}：${n.class} 的插槽没有这个属性`)
    checkSlots(c)
  }
}
checkSlots(root)
for (const [, cfg] of pageConfigs) checkSlots(cfg.root)
if (slotErrors.length) throw new Error('\n' + [...new Set(slotErrors)].join('\n'))

// ---- 选择器路径：父界面里的控件直接写路径，子页面里的写成 `…/Page_<页>>子界面路径` ----
const paths = {}
const walk = (n, prefix, into) => {
  const p = prefix ? prefix + '/' + n.name : n.name
  paths[n.name] = into ? into + '>' + p : p
  for (const c of n.children ?? []) walk(c, p, into)
}
walk(root, '', '')
for (const [id, cfg] of pageConfigs) walk(cfg.root, '', paths['Page_' + id])
const sel = (name) => {
  if (!paths[name]) throw new Error('没有控件 ' + name)
  return 'WBP_ForgeSample:' + paths[name]
}

// ---- 写文件 ----
// 短的对象 / 数组写一行，长的逐项展开：配置要能让人读、能 diff
const pretty = (v, ind) => {
  const one = JSON.stringify(v)
  if (typeof v !== 'object' || v === null || one.length <= 140) return one
  const next = ind + '  '
  if (Array.isArray(v)) return '[\n' + v.map((x) => next + pretty(x, next)).join(',\n') + '\n' + ind + ']'
  return '{\n' + Object.entries(v).map(([k, x]) => next + JSON.stringify(k) + ': ' + pretty(x, next)).join(',\n') + '\n' + ind + '}'
}
const write = (rel, obj) => {
  const f = path.join(OUT, rel)
  fs.mkdirSync(path.dirname(f), { recursive: true })
  fs.writeFileSync(f, pretty(obj, '') + '\n')
  console.log(rel, fs.statSync(f).size, 'bytes')
}
write('umg/WBP_ForgeSample.json', umg)
for (const [id, cfg] of pageConfigs) write('umg/' + pageFile(id), cfg)
write('map/Map_ForgeSample.json', map)
for (const [rel, scenario] of Object.entries(buildScenarios({ sel, MAP }))) write('_GameTest/' + rel, scenario)
console.log('控件数', names.size)
