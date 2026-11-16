// 生成 AssetForge 示例测试界面的配置和配套 GameTest 场景，输出到 plugins/AssetForge/docs/AssetForge/Test。
// 界面是一份 600 多个控件的 UMG 配置，手改容易漏改名字和选择器路径，所以由这个脚本生成：
// 改界面就改这里再跑 `node docs/asset-forge-sample/gen.mjs`，生成的 JSON 一起提交（revision 记得加 1）。
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const OUT = process.argv[2] ?? path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../plugins/AssetForge/docs/AssetForge/Test')
const ASSET = '/Game/AgentTest/AssetForge/WBP_ForgeSample'
const MAP = '/Game/AgentTest/AssetForge/Map_ForgeSample'

// ---- 颜色：设计稿按 sRGB 写，FLinearColor 要线性值 ----
const lin = (c) => {
  c /= 255
  return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4)
}
const r4 = (x) => Math.round(x * 10000) / 10000
const color = (hex, a = 1) => {
  const h = hex.replace('#', '')
  return {
    R: r4(lin(parseInt(h.slice(0, 2), 16))),
    G: r4(lin(parseInt(h.slice(2, 4), 16))),
    B: r4(lin(parseInt(h.slice(4, 6), 16))),
    A: a,
  }
}
const slate = (hex, a = 1) => ({ SpecifiedColor: color(hex, a), ColorUseRule: 'UseColor_Specified' })
const flat = (hex, a = 1) => ({ BrushColor: color(hex, a) })
const rounded = (hex, radius, a = 1, outline) => {
  const b = {
    DrawAs: 'RoundedBox',
    TintColor: slate(hex, a),
    OutlineSettings: { CornerRadii: { X: radius, Y: radius, Z: radius, W: radius }, RoundingType: 'FixedRadius' },
  }
  if (outline) Object.assign(b.OutlineSettings, { Color: slate(outline), Width: 1 })
  return b
}
const margin = (l, t = l, r = l, b = t) => ({ Left: l, Top: t, Right: r, Bottom: b })

const T = {
  bg: '#0E1117',
  side: '#141923',
  top: '#121722',
  card: '#1A2130',
  card2: '#202838',
  line: '#2A3346',
  text: '#E8ECF4',
  muted: '#8D97AB',
  accent: '#4F8CFF',
  success: '#2FBF71',
  warn: '#F2A93B',
  danger: '#EF5B5B',
  neutral: '#2A3346',
  ghost: '#1E2534',
}

// ---- 控件 ----
const names = new Set()
const node = (cls, name, props, children, slot) => {
  if (names.has(name)) throw new Error('重名：' + name)
  names.add(name)
  const n = { class: cls, name }
  if (props && Object.keys(props).length) n.props = props
  if (slot && Object.keys(slot).length) n.slot = slot
  if (children && children.length) n.children = children
  return n
}
const font = (size, bold) => ({ Size: size, TypefaceFontName: bold ? 'Bold' : 'Regular' })
const text = (name, value, { size = 14, bold = false, color: c = T.text, wrap = false, justify, slot } = {}) => {
  const p = { Text: value, Font: font(size, bold) }
  if (c !== T.text) p.ColorAndOpacity = slate(c)
  if (wrap) p.AutoWrapText = true
  if (justify) p.Justification = justify
  return node('TextBlock', name, p, null, slot)
}
const box = (cls, name, children, slot, props) => node(cls, name, props, children, slot)
const fill = (v = 1) => ({ Size: { SizeRule: 'Fill', Value: v } })
const spacer = (name, w, h, slot) => node('Spacer', name, { Size: { X: w, Y: h } }, null, slot)
const sized = (name, w, h, child, slot) => {
  const p = {}
  if (w != null) Object.assign(p, { bOverride_WidthOverride: true, WidthOverride: w })
  if (h != null) Object.assign(p, { bOverride_HeightOverride: true, HeightOverride: h })
  return node('SizeBox', name, p, [child], slot)
}
const card = (name, children, { pad = 20, bg = T.card, radius = 12, slot, outline = T.line } = {}) =>
  node('Border', name, { Background: rounded(bg, radius, 1, outline), Padding: margin(pad) }, children, slot)

// 按钮用引擎默认的圆角样式，颜色走 BackgroundColor（跳页父类给当前页的按钮换底色就是改它）
const BG = {
  accent: color('#5A95FF'),
  success: color('#36C97A'),
  danger: color('#F06464'),
  warn: color('#F5B04A'),
  neutral: color('#323C52'),
  ghost: color('#222A3A'),
}
const button = (name, label, { kind = 'neutral', align = 'HAlign_Center', tip, slot, size = 14, radius = 8, extra } = {}) => {
  // 用引擎默认的圆角按钮样式，只换底色：每个按钮写一整套 WidgetStyle 会让配置膨胀几十倍
  const props = { BackgroundColor: BG[kind] }
  if (tip) props.ToolTipText = tip
  const content = extra
    ? extra
    : text('L_' + name, label, { size, bold: kind !== 'ghost' && kind !== 'neutral', color: T.text })
  return node('Button', name, props, [content], slot)
}
const navButton = (name, label, slot) =>
  button(name, label, { kind: 'ghost', align: 'HAlign_Left', slot: slot ?? { Padding: margin(0, 0, 0, 6) } })

const hbox = (name, children, slot) => box('HorizontalBox', name, children, slot)
const vbox = (name, children, slot) => box('VerticalBox', name, children, slot)
const gap = { Padding: margin(0, 0, 10, 0) }
const vgap = (n = 12) => ({ Padding: margin(0, 0, 0, n) })

// ---- 侧栏 ----
const sidebar = sized(
  'SideWidth',
  248,
  null,
  node('Border', 'SideBg', { ...flat(T.side), Padding: margin(18, 22) }, [
    vbox('SideCol', [
      hbox(
        'BrandRow',
        [
          sized(
            'BrandMarkSize',
            40,
            40,
            node('Border', 'BrandMark', { Background: rounded(T.accent, 10), HorizontalAlignment: 'HAlign_Center', VerticalAlignment: 'VAlign_Center' }, [
              text('BrandMarkText', 'AF', { size: 16, bold: true }),
            ]),
            { Padding: margin(0, 0, 12, 0), VerticalAlignment: 'VAlign_Center' },
          ),
          vbox(
            'BrandText',
            [text('BrandTitle', 'Forge 示例', { size: 18, bold: true }), text('BrandSub', 'GameTest 录制靶子', { size: 11, color: T.muted })],
            { VerticalAlignment: 'VAlign_Center' },
          ),
        ],
        vgap(28),
      ),
      text('NavLabel', '导航', { size: 11, color: T.muted, slot: vgap(8) }),
      navButton('Go_Page_Home__Nav', '首页'),
      navButton('Go_Page_Inventory__Nav', '背包'),
      navButton('Go_Page_Shop__Nav', '商店'),
      navButton('Go_Page_Tasks__Nav', '任务'),
      navButton('Go_Page_Settings__Nav', '设置'),
      navButton('Go_Page_Profile__Nav', '资料'),
      spacer('SideFill', 1, 1, fill()),
      navButton('Open_Popup_Help__Side', '帮助'),
      button('Open_Popup_Quit', '退出', { kind: 'danger', slot: { Padding: margin(0, 4, 0, 0) } }),
    ]),
  ]),
)

// ---- 顶栏 ----
const topbar = node('Border', 'TopBar', { ...flat(T.top), Padding: margin(24, 14) }, [
  hbox('TopRow', [
    button('Back', '← 返回', { kind: 'neutral', slot: { Padding: margin(0, 0, 16, 0), VerticalAlignment: 'VAlign_Center' } }),
    text('Crumb', '首页', { size: 22, bold: true, slot: { VerticalAlignment: 'VAlign_Center' } }),
    spacer('TopFill', 1, 1, fill()),
    node(
      'Border',
      'StatusChip',
      { Background: rounded(T.card, 16, 1, T.line), Padding: margin(14, 7) },
      [text('Status', '就绪', { size: 13, color: T.muted })],
      { VerticalAlignment: 'VAlign_Center', Padding: margin(0, 0, 12, 0) },
    ),
    button('Open_Popup_Help__Top', '?', { kind: 'neutral', slot: { VerticalAlignment: 'VAlign_Center' } }),
  ]),
])

// ---- 首页 ----
const stat = (id, label, value, accent, extra = []) =>
  card(
    'Stat' + id,
    [
      vbox('Stat' + id + 'Col', [
        text('Stat' + id + 'Label', label, { size: 12, color: T.muted, slot: vgap(6) }),
        text('Stat' + id + 'Value', value, { size: 26, bold: true, color: accent }),
        ...extra,
      ]),
    ],
    { slot: { ...fill(), Padding: margin(0, 0, 14, 0) } },
  )
const news = [
  ['1', '版本', '2.4 版本更新说明：新增跨服竞技场'],
  ['2', '活动', '周末双倍经验，周六 10:00 开始'],
  ['3', '维护', '11 月 20 日 6:00–8:00 停服维护'],
  ['4', '公告', '防沉迷系统升级说明'],
  ['5', '活动', '首充礼包限时返场'],
  ['6', '社区', '优秀攻略征集，奖励限定称号'],
]
const pageHome = vbox('Page_Home', [
  node(
    'Border',
    'Hero',
    { Background: rounded('#1C3260', 14, 1, '#2D4D8C'), Padding: margin(28, 24) },
    [
      vbox('HeroCol', [
        text('HeroTitle', '欢迎回来，指挥官', { size: 28, bold: true, slot: vgap(8) }),
        text('HeroDesc', '这是 AssetForge 用一份 JSON 生成的多页面界面：侧栏跳页、页签、返回、弹窗、列表、输入框、开关、滑条都有，用来练习 GameTest 录制。', {
          size: 14,
          color: '#B9C7E4',
          wrap: true,
          slot: vgap(18),
        }),
        hbox('HeroBtns', [
          button('Go_Page_Shop__Hero', '去商店', { kind: 'accent', slot: gap }),
          button('Go_Page_Tasks__Hero', '查看任务', { kind: 'neutral', slot: gap }),
          button('Open_Popup_Reward__Daily', '领取每日奖励', { kind: 'success', tip: '每日登录奖励：金币 ×500' }),
        ]),
      ]),
    ],
    vgap(16),
  ),
  hbox(
    'Stats',
    [
      stat('Gold', '金币', '12,800', '#F5C451'),
      stat('Level', '等级', 'Lv.27', T.text),
      stat('Energy', '体力', '96 / 120', '#5FD39A', [
        node('ProgressBar', 'EnergyBar', { Percent: 0.8, FillColorAndOpacity: color('#36C97A') }, null, { Padding: margin(0, 10, 0, 0) }),
      ]),
    ],
    vgap(16),
  ),
  card(
    'NewsCard',
    [
      vbox('NewsCol', [
        hbox(
          'NewsHead',
          [
            text('NewsTitle', '公告', { size: 16, bold: true, slot: { ...fill(), VerticalAlignment: 'VAlign_Center' } }),
            text('NewsPickedLabel', '已读：', { size: 12, color: T.muted, slot: { VerticalAlignment: 'VAlign_Center' } }),
            text('NewsPicked', '无', { size: 12, color: T.accent, slot: { VerticalAlignment: 'VAlign_Center' } }),
          ],
          vgap(10),
        ),
        node(
          'ScrollBox',
          'NewsList',
          {},
          news.map(([id, tag, title]) =>
            button('Pick_NewsPicked__' + id, title, {
              kind: 'ghost',
              radius: 8,
              slot: { Padding: margin(0, 0, 0, 6) },
              extra: hbox('NewsRow' + id, [
                node('Border', 'NewsTag' + id, { Background: rounded(tag === '活动' ? '#3A2F12' : '#1F2D4D', 6), Padding: margin(8, 2) }, [
                  text('NewsTagText' + id, tag, { size: 11, color: tag === '活动' ? T.warn : T.accent }),
                ], { Padding: margin(0, 0, 10, 0), VerticalAlignment: 'VAlign_Center' }),
                text('L_Pick_NewsPicked__' + id, title, { size: 14, slot: { ...fill(), VerticalAlignment: 'VAlign_Center' } }),
              ]),
            }),
          ),
          fill(),
        ),
      ]),
    ],
    { slot: fill() },
  ),
])

// ---- 背包 ----
const items = {
  Weapon: [
    ['Sword', '铁剑', '剑', '攻击 +12，基础单手武器', '#7FA7FF'],
    ['Bow', '猎弓', '弓', '远程攻击，射程 25 米', '#7FA7FF'],
    ['Staff', '橡木法杖', '杖', '法术强度 +18', '#C38BFF'],
    ['Axe', '战斧', '斧', '攻击 +20，攻速较慢', '#7FA7FF'],
  ],
  Potion: [
    ['Hp', '生命药水', '红', '立即恢复 300 点生命', '#FF7A7A'],
    ['Mp', '法力药水', '蓝', '立即恢复 200 点法力', '#6FB2FF'],
    ['Speed', '疾行药剂', '速', '移动速度 +30%，持续 20 秒', '#5FD39A'],
    ['Elixir', '万能灵药', '灵', '解除所有负面状态', '#F5C451'],
  ],
}
const itemButton = (tab, [id, label, glyph, desc, tint], i) =>
  button(`Pick_DetailName__${id}_${tab}`, label, {
    kind: 'ghost',
    radius: 10,
    tip: desc,
    slot: { Row: Math.floor(i / 4), Column: i % 4 },
    extra: vbox(`ItemCol_${id}_${tab}`, [
      node('Border', `ItemIcon_${id}_${tab}`, { Background: rounded('#2A3550', 10), Padding: margin(20, 14) }, [
        text(`ItemGlyph_${id}_${tab}`, glyph, { size: 22, bold: true, color: tint }),
      ], { HorizontalAlignment: 'HAlign_Center', Padding: margin(0, 4, 0, 8) }),
      text(`L_Pick_DetailName__${id}_${tab}`, label, { size: 13, justify: 'Center', slot: { HorizontalAlignment: 'HAlign_Center' } }),
    ]),
  })
const grid = (tab, list) =>
  node('UniformGridPanel', 'Grid_' + tab, { SlotPadding: margin(6), MinDesiredSlotWidth: 120, MinDesiredSlotHeight: 130 }, list.map((it, i) => itemButton(tab, it, i)))
const tabButton = (name, label, slot) => button(name, label, { kind: 'ghost', slot: slot ?? gap })
const pageInventory = vbox('Page_Inventory', [
  hbox(
    'InvHead',
    [
      text('Page_Inventory_Title', '背包', { size: 20, bold: true, slot: { ...fill(), VerticalAlignment: 'VAlign_Center' } }),
      tabButton('Go_Tab_All', '全部'),
      tabButton('Go_Tab_Weapon', '武器'),
      tabButton('Go_Tab_Potion', '药水', { Padding: margin(0) }),
    ],
    vgap(14),
  ),
  hbox(
    'InvBody',
    [
      card(
        'InvGridCard',
        [
          node('WidgetSwitcher', 'InvTabs', {}, [
            node('ScrollBox', 'Tab_All', {}, [grid('All', [...items.Weapon, ...items.Potion])]),
            node('ScrollBox', 'Tab_Weapon', {}, [grid('Weapon', items.Weapon)]),
            node('ScrollBox', 'Tab_Potion', {}, [grid('Potion', items.Potion)]),
          ]),
        ],
        { pad: 14, slot: { ...fill(2), Padding: margin(0, 0, 16, 0) } },
      ),
      card(
        'DetailCard',
        [
          vbox('DetailCol', [
            text('DetailLabel', '物品详情', { size: 12, color: T.muted, slot: vgap(8) }),
            text('DetailName', '选择一件物品', { size: 22, bold: true, slot: vgap(8) }),
            text('DetailName_Subject', '点左边的格子查看说明。', { size: 14, color: '#B6C0D3', wrap: true, slot: vgap(20) }),
            spacer('DetailFill', 1, 1, fill()),
            button('Open_Popup_UseItem', '使用', { kind: 'accent', slot: vgap(8) }),
            button('Open_Popup_Discard', '丢弃', { kind: 'danger' }),
          ]),
        ],
        { slot: fill(1) },
      ),
    ],
    fill(),
  ),
])

// ---- 商店 ----
const goods = [
  ['Hp', '生命药水', '立即恢复 300 点生命', 120, '#FF7A7A', '红'],
  ['Mp', '法力药水', '立即恢复 200 点法力', 100, '#6FB2FF', '蓝'],
  ['Scroll', '回城卷轴', '5 秒后传送回主城', 60, '#F5C451', '卷'],
  ['Gem', '红宝石', '镶嵌后攻击 +5', 880, '#FF7A9C', '宝'],
  ['Pet', '幼年狐狸', '跟随宠物，拾取范围 +2 米', 2400, '#FFA35C', '狐'],
  ['Mount', '疾风马', '坐骑，移动速度 +60%', 6800, '#9FD36F', '马'],
  ['Bag', '扩容背包', '背包格子 +12', 1500, '#8FA3C7', '包'],
]
const shopRow = ([id, label, desc, price, tint, glyph]) =>
  card(
    'ShopRow_' + id,
    [
      hbox('ShopRowBox_' + id, [
        node('Border', 'ShopIcon_' + id, { Background: rounded('#2A3550', 10), Padding: margin(14, 10) }, [
          text('ShopGlyph_' + id, glyph, { size: 18, bold: true, color: tint }),
        ], { Padding: margin(0, 0, 14, 0), VerticalAlignment: 'VAlign_Center' }),
        vbox('ShopText_' + id, [text('ShopName_' + id, label, { size: 15, bold: true, slot: vgap(4) }), text('ShopDesc_' + id, desc, { size: 12, color: T.muted })], {
          ...fill(),
          VerticalAlignment: 'VAlign_Center',
        }),
        text('ShopPrice_' + id, price.toLocaleString('en-US') + ' 金币', { size: 14, bold: true, color: '#F5C451', slot: { VerticalAlignment: 'VAlign_Center', Padding: margin(0, 0, 16, 0) } }),
        button('Open_Popup_Buy__' + id, '购买', { kind: 'accent', tip: `${label} · ${price.toLocaleString('en-US')} 金币`, slot: { VerticalAlignment: 'VAlign_Center' } }),
      ]),
    ],
    { pad: 12, radius: 10, slot: { Padding: margin(0, 0, 0, 8) } },
  )
const pageShop = vbox('Page_Shop', [
  hbox(
    'ShopHead',
    [
      text('Page_Shop_Title', '商店', { size: 20, bold: true, slot: { ...fill(), VerticalAlignment: 'VAlign_Center' } }),
      text('ShopGoldLabel', '持有金币', { size: 12, color: T.muted, slot: { VerticalAlignment: 'VAlign_Center', Padding: margin(0, 0, 8, 0) } }),
      text('ShopGold', '12,800', { size: 16, bold: true, color: '#F5C451', slot: { VerticalAlignment: 'VAlign_Center' } }),
    ],
    vgap(14),
  ),
  node('ScrollBox', 'ShopList', {}, goods.map(shopRow), fill()),
])

// ---- 任务 ----
const tasks = {
  Daily: [
    ['Daily1', '完成 3 场匹配', 0.66, '2 / 3'],
    ['Daily2', '击败 50 个敌人', 1, '50 / 50'],
    ['Daily3', '在商店购买任意物品', 0, '0 / 1'],
  ],
  Weekly: [
    ['Weekly1', '通关团队副本「熔火之心」', 0.25, '1 / 4'],
    ['Weekly2', '累计在线 10 小时', 0.7, '7 / 10'],
  ],
  Story: [
    ['Story1', '第三章：失落的王冠', 0.5, '进行中'],
    ['Story2', '第四章：北境来信', 0, '未解锁'],
  ],
}
const taskRow = ([id, title, pct, progress]) =>
  card(
    'TaskRow_' + id,
    [
      hbox('TaskBox_' + id, [
        node('CheckBox', 'Task_' + id, pct >= 1 ? { CheckedState: 'Checked' } : {}, [text('TaskName_' + id, title, { size: 14 })], {
          ...fill(),
          VerticalAlignment: 'VAlign_Center',
        }),
        sized('TaskBarSize_' + id, 160, null, node('ProgressBar', 'TaskBar_' + id, { Percent: pct, FillColorAndOpacity: color(pct >= 1 ? '#36C97A' : '#5A95FF') }), {
          VerticalAlignment: 'VAlign_Center',
          Padding: margin(12, 0),
        }),
        text('TaskProgress_' + id, progress, { size: 12, color: T.muted, slot: { VerticalAlignment: 'VAlign_Center', Padding: margin(0, 0, 12, 0) } }),
        button('Go_Page_TaskDetail__' + id, '详情', { kind: 'neutral', tip: title, slot: { VerticalAlignment: 'VAlign_Center' } }),
      ]),
    ],
    { pad: 12, radius: 10, slot: { Padding: margin(0, 0, 0, 8) } },
  )
const pageTasks = vbox('Page_Tasks', [
  hbox(
    'TaskHead',
    [
      text('Page_Tasks_Title', '任务', { size: 20, bold: true, slot: { ...fill(), VerticalAlignment: 'VAlign_Center' } }),
      tabButton('Go_Tab_Daily', '每日'),
      tabButton('Go_Tab_Weekly', '每周'),
      tabButton('Go_Tab_Story', '主线', { Padding: margin(0) }),
    ],
    vgap(14),
  ),
  node(
    'WidgetSwitcher',
    'TaskTabs',
    {},
    Object.entries(tasks).map(([tab, list]) => node('ScrollBox', 'Tab_' + tab, {}, list.map(taskRow))),
    fill(),
  ),
])
const pageTaskDetail = vbox('Page_TaskDetail', [
  card('TaskDetailCard', [
    vbox('TaskDetailCol', [
      text('Page_TaskDetail_Title', '任务详情', { size: 12, color: T.muted, slot: vgap(6) }),
      text('Page_TaskDetail_Subject', '（从任务列表点「详情」进来）', { size: 24, bold: true, slot: vgap(12) }),
      text('TaskDetailDesc', '完成后可领取奖励。这一页不在侧栏里，只能从任务列表进来，用「返回」回去：用来录多层跳转。', {
        size: 14,
        color: '#B6C0D3',
        wrap: true,
        slot: vgap(16),
      }),
      hbox('TaskRewards', [
        node('Border', 'RewardGold', { Background: rounded('#3A2F12', 8), Padding: margin(12, 6) }, [text('RewardGoldText', '金币 ×300', { size: 13, color: T.warn })], gap),
        node('Border', 'RewardExp', { Background: rounded('#1F2D4D', 8), Padding: margin(12, 6) }, [text('RewardExpText', '经验 ×1200', { size: 13, color: T.accent })], gap),
      ], vgap(20)),
      hbox('TaskDetailBtns', [
        button('Open_Popup_Reward__Task', '领取奖励', { kind: 'success', tip: '任务奖励：金币 ×300、经验 ×1200', slot: gap }),
        button('Go_Page_Shop__Task', '去商店补给', { kind: 'neutral' }),
      ]),
    ]),
  ], { pad: 28, slot: { HorizontalAlignment: 'HAlign_Fill' } }),
])

// ---- 设置 ----
const settingRow = (id, label, control, value) =>
  hbox(
    'Row_' + id,
    [
      text(id + '_Title', label, { size: 14, slot: { VerticalAlignment: 'VAlign_Center' } }),
      spacer('RowGap_' + id, 24, 1),
      sized('Ctl_' + id, 280, null, control, { VerticalAlignment: 'VAlign_Center' }),
      ...(value ? [text(id + '_Value', value, { size: 14, bold: true, color: T.accent, slot: { VerticalAlignment: 'VAlign_Center', Padding: margin(14, 0, 0, 0) } })] : []),
    ],
    vgap(16),
  )
const slider = (id, v, min, max, step = 1) =>
  node('Slider', id, { Value: v, MinValue: min, MaxValue: max, StepSize: step, SliderBarColor: color('#3A4560'), SliderHandleColor: color('#5A95FF') })
const check = (id, label, on) =>
  node('CheckBox', id, on ? { CheckedState: 'Checked' } : {}, [text('L_' + id, label, { size: 14 })], vgap(14))
const combo = (id, options, selected) => node('ComboBoxString', id, { DefaultOptions: options, SelectedOption: selected })
const pageSettings = vbox('Page_Settings', [
  hbox(
    'SetHead',
    [
      text('Page_Settings_Title', '设置', { size: 20, bold: true, slot: { ...fill(), VerticalAlignment: 'VAlign_Center' } }),
      tabButton('Go_Tab_Audio', '声音'),
      tabButton('Go_Tab_Video', '画面'),
      tabButton('Go_Tab_Controls', '操作', { Padding: margin(0) }),
    ],
    vgap(14),
  ),
  card(
    'SetCard',
    [
      node('WidgetSwitcher', 'SettingsTabs', {}, [
        vbox('Tab_Audio', [
          settingRow('MasterVolume', '主音量', slider('MasterVolume', 80, 0, 100), '80'),
          settingRow('MusicVolume', '音乐', slider('MusicVolume', 60, 0, 100), '60'),
          settingRow('SfxVolume', '音效', slider('SfxVolume', 70, 0, 100), '70'),
          check('Mute', '静音', false),
        ]),
        vbox('Tab_Video', [
          settingRow('Quality', '画质', combo('Quality', ['低', '中', '高', '极高'], '高'), '高'),
          settingRow('FrameCap', '帧率上限', combo('FrameCap', ['30', '60', '120', '不限'], '60'), '60'),
          check('VSync', '垂直同步', true),
          check('Fullscreen', '全屏', false),
        ]),
        vbox('Tab_Controls', [
          settingRow('Sensitivity', '灵敏度', slider('Sensitivity', 5, 1, 10), '5'),
          check('InvertY', '反转 Y 轴', false),
          check('Vibration', '手柄震动', true),
          button('Open_Popup_Reset', '恢复默认', { kind: 'neutral', slot: { HorizontalAlignment: 'HAlign_Left', Padding: margin(0, 8, 0, 0) } }),
        ]),
      ], fill()),
    ],
    { pad: 24, slot: { ...fill(), Padding: margin(0, 0, 0, 14) } },
  ),
  hbox('SetFoot', [
    spacer('SetFootFill', 1, 1, fill()),
    button('Open_Popup_Saved', '保存设置', { kind: 'accent' }),
  ]),
])

// ---- 资料 ----
const inputRow = (id, label, hint) =>
  vbox('Field_' + id, [
    text(id + '_Title', label, { size: 12, color: T.muted, slot: vgap(6) }),
    hbox('FieldRow_' + id, [
      node('EditableTextBox', id, { HintText: hint }, null, { ...fill(), Padding: margin(0, 0, 10, 0) }),
      button('Submit_' + id, '保存', { kind: 'accent' }),
    ], vgap(6)),
    hbox('EchoRow_' + id, [
      text('EchoLabel_' + id, '当前：', { size: 12, color: T.muted }),
      text(id + '_Echo', '未设置', { size: 12, color: T.text }),
    ]),
  ], vgap(18))
const pageProfile = hbox('Page_Profile', [
  card(
    'ProfileCard',
    [
      vbox('ProfileCol', [
        sized('AvatarSize', 96, 96, node('Border', 'Avatar', { Background: rounded('#2D4D8C', 48), HorizontalAlignment: 'HAlign_Center', VerticalAlignment: 'VAlign_Center' }, [
          text('AvatarText', '指', { size: 36, bold: true }),
        ]), { HorizontalAlignment: 'HAlign_Center', Padding: margin(0, 0, 0, 14) }),
        text('Page_Profile_Title', '资料', { size: 20, bold: true, justify: 'Center', slot: { HorizontalAlignment: 'HAlign_Center', Padding: margin(0, 0, 0, 4) } }),
        text('ProfileId', 'ID 100 238 771', { size: 12, color: T.muted, slot: { HorizontalAlignment: 'HAlign_Center', Padding: margin(0, 0, 0, 18) } }),
        text('ExpLabel', '经验 7,420 / 10,000', { size: 12, color: T.muted, slot: vgap(6) }),
        node('ProgressBar', 'ExpBar', { Percent: 0.742, FillColorAndOpacity: color('#5A95FF') }, null, vgap(18)),
        check('PublicProfile', '公开资料', true),
        check('FriendRequests', '允许好友申请', true),
      ]),
    ],
    { pad: 24, slot: { Padding: margin(0, 0, 16, 0) } },
  ),
  card(
    'ProfileForm',
    [
      vbox('ProfileFormCol', [
        inputRow('PlayerName', '昵称', '输入昵称（2–12 个字）'),
        inputRow('Signature', '个性签名', '写一句话介绍自己'),
        inputRow('InviteCode', '邀请码', '输入好友的邀请码'),
        button('Open_Popup_Logout', '退出登录', { kind: 'danger', slot: { HorizontalAlignment: 'HAlign_Left' } }),
      ]),
    ],
    { pad: 24, slot: fill() },
  ),
])

// ---- 弹窗 ----
const popup = (id, title, body, buttons, { subject, width = 460 } = {}) =>
  node(
    'Overlay',
    'Popup_' + id,
    { Visibility: 'Collapsed' },
    [
      node('Border', 'Dim_' + id, flat('#000000', 0.62), null, { HorizontalAlignment: 'HAlign_Fill', VerticalAlignment: 'VAlign_Fill' }),
      sized(
        'CardSize_' + id,
        width,
        null,
        card('Card_' + id, [
          vbox('CardCol_' + id, [
            text('Popup_' + id + '_Title', title, { size: 20, bold: true, slot: vgap(10) }),
            ...(subject !== undefined ? [text('Popup_' + id + '_Subject', subject, { size: 15, bold: true, color: T.accent, slot: vgap(8) })] : []),
            text('Body_' + id, body, { size: 14, color: '#B6C0D3', wrap: true, slot: vgap(22) }),
            hbox('CardBtns_' + id, [spacer('CardFill_' + id, 1, 1, fill()), ...buttons]),
          ]),
        ], { pad: 24, radius: 14, bg: T.card2 }),
        { HorizontalAlignment: 'HAlign_Center', VerticalAlignment: 'VAlign_Center' },
      ),
    ],
    { HorizontalAlignment: 'HAlign_Fill', VerticalAlignment: 'VAlign_Fill' },
  )
const cancel = (id) => button(`Close_Popup_${id}__Cancel`, '取消', { kind: 'neutral', slot: gap })
const ok = (id, label = '确定', kind = 'accent') => button(`Close_Popup_${id}__Ok`, label, { kind })
const popups = [
  popup('Reward', '领取奖励', '奖励已放进背包，可以在背包里查看。', [ok('Reward', '收下', 'success')], { subject: '' }),
  popup('UseItem', '使用物品', '确定使用选中的物品吗？使用后不能撤销。', [cancel('UseItem'), ok('UseItem', '使用')]),
  popup('Discard', '丢弃物品', '丢弃后物品会消失，确定吗？', [cancel('Discard'), ok('Discard', '丢弃', 'danger')]),
  popup('Buy', '购买确认', '购买后金币会立即扣除。不清楚可以先看说明（会再弹一层）。', [
    button('Open_Popup_Help__Buy', '说明', { kind: 'ghost', slot: gap }),
    cancel('Buy'),
    ok('Buy', '购买'),
  ], { subject: '' }),
  popup('Reset', '恢复默认', '把所有设置恢复成默认值？', [cancel('Reset'), ok('Reset', '恢复', 'danger')]),
  popup('Saved', '设置已保存', '新的设置已经生效。', [ok('Saved', '好的')]),
  popup('Logout', '退出登录', '退出后回到首页，资料页的输入不会保存。', [cancel('Logout'), button('Go_Page_Home__Logout', '退出并回首页', { kind: 'danger' })]),
  popup('Quit', '退出游戏', '要退出吗？未保存的进度会丢失。', [cancel('Quit'), button('Go_Page_Home__Quit', '回到首页', { kind: 'neutral', slot: gap }), ok('Quit', '退出', 'danger')]),
  popup('Help', '帮助', '侧栏切页，页签切子页，「返回」先关最上面的弹窗、再回上一页。右上角的状态条显示最近一次操作的结果，GameTest 可以断言它。', [ok('Help', '知道了')], { width: 520 }),
]

// ---- 整体 ----
const root = node('Overlay', 'Root', {}, [
  node('Border', 'Bg', flat(T.bg), null, { HorizontalAlignment: 'HAlign_Fill', VerticalAlignment: 'VAlign_Fill' }),
  hbox(
    'Shell',
    [
      { ...sidebar, slot: { HorizontalAlignment: 'HAlign_Left', VerticalAlignment: 'VAlign_Fill' } },
      vbox('Main', [topbar, node('WidgetSwitcher', 'Pages', {}, [pageHome, pageInventory, pageShop, pageTasks, pageTaskDetail, pageSettings, pageProfile], { ...fill(), Padding: margin(28, 24) })], fill()),
    ],
    { HorizontalAlignment: 'HAlign_Fill', VerticalAlignment: 'VAlign_Fill' },
  ),
  ...popups,
])

const umg = {
  '//': '由 AssetForge 插件自带的示例测试界面：多页面、跳页、弹窗、列表、输入框、开关，用来练习 GameTest 录制。跳页逻辑在父类 ForgeSampleScreen 里，按控件名约定工作，说明见同目录的 README.md。',
  kind: 'umg-widget',
  schemaVersion: 1,
  revision: 1,
  asset: ASSET,
  parent: '/Script/AssetForgeTests.ForgeSampleScreen',
  root,
}
const map = {
  '//': '示例测试地图：只有世界设置，GameMode 进图就显示 WBP_ForgeSample。项目的默认世界设置类不是 WorldSettings 时，把 root.class 换成项目用的那个类。',
  kind: 'map',
  schemaVersion: 1,
  revision: 1,
  asset: MAP,
  root: { class: '/Script/Engine.WorldSettings', name: 'WorldSettings', props: { DefaultGameMode: '/Script/AssetForgeTests.ForgeSampleGameMode' } },
}

// ---- 选择器路径 ----
const paths = {}
const walk = (n, prefix) => {
  const p = prefix ? prefix + '/' + n.name : n.name
  paths[n.name] = p
  for (const c of n.children ?? []) walk(c, p)
}
walk(root, '')
const sel = (name) => {
  if (!paths[name]) throw new Error('没有控件 ' + name)
  return 'WBP_ForgeSample:' + paths[name]
}
const click = (name, note) => ({ action: 'click', target: sel(name), ...(note ? { note } : {}) })
const crumb = (t) => ({ action: 'assert', target: sel('Crumb'), text: t })
const scenario = {
  kind: 'gametest-scenario',
  schemaVersion: 2,
  name: 'AssetForge 示例界面：跳页、弹窗、返回',
  _note: '配套 AssetForge 示例测试界面（先在 AssetForge 页导入 Test/ 下的两份配置）。手动录制：打开 Map_ForgeSample → PIE → 在 GameTest 页点录制。',
  revision: 1,
  tags: ['sample', 'assetforge'],
  stepDelayMs: 300,
  steps: [
    { action: 'pie.start', map: MAP },
    { action: 'wait', until: 'widget', target: sel('Crumb'), note: '等示例界面出来（GameMode 进图就加进视口）' },
    crumb('首页'),
    click('Go_Page_Shop__Hero', '首页大图上的「去商店」'),
    crumb('商店'),
    click('Open_Popup_Buy__Gem', '买红宝石'),
    { action: 'assert', target: sel('Popup_Buy'), visible: true },
    { action: 'assert', target: sel('Popup_Buy_Subject'), textContains: '红宝石' },
    click('Open_Popup_Help__Buy', '弹窗上再弹一层说明'),
    { action: 'assert', target: sel('Popup_Help'), visible: true },
    click('Back', '返回先关最上面的弹窗'),
    { action: 'assert', target: sel('Popup_Buy'), visible: true },
    click('Close_Popup_Buy__Ok'),
    { action: 'assert', target: sel('Status'), textContains: '关闭' },
    click('Go_Page_Settings__Nav'),
    click('Go_Tab_Video'),
    { action: 'set', target: sel('Quality'), option: '极高' },
    { action: 'assert', target: sel('Quality_Value'), text: '极高' },
    click('Go_Page_Profile__Nav'),
    { action: 'type', target: sel('PlayerName'), text: 'Tester01' },
    click('Submit_PlayerName'),
    { action: 'assert', target: sel('PlayerName_Echo'), text: 'Tester01' },
    click('Back'),
    crumb('设置'),
    click('Go_Page_Tasks__Nav'),
    click('Go_Tab_Weekly'),
    click('Go_Page_TaskDetail__Weekly1', '从任务列表进详情页（侧栏里没有这一页）'),
    crumb('任务详情'),
    { action: 'assert', target: sel('Page_TaskDetail_Subject'), textContains: '熔火之心' },
    click('Go_Page_Shop__Task'),
    crumb('商店'),
    click('Back'),
    crumb('任务详情'),
    click('Back'),
    crumb('任务'),
    { action: 'screenshot', name: 'forge-sample-tasks' },
    { action: 'pie.stop' },
    { action: 'wait', until: 'pieStopped' },
  ],
}

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
write('map/Map_ForgeSample.json', map)
write('_GameTest/ForgeSample-冒烟.json', scenario)
console.log('控件数', names.size)
