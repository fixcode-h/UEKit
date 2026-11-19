// 示例界面原有的几页（首页、背包、商店、任务、任务详情、设置、资料、列表条目）、侧栏、顶栏和弹窗。
// 每一页的根控件叫 `<页>Root`，作为子 WBP 的根；父界面 Pages 里引用它的节点叫 `Page_<页>`（跳页按钮 Go_Page_<页> 认这个名字）。
import { T, BG, button, navButton, card, color, fill, flat, gap, hbox, margin, node, rounded, sized, slate, spacer, text, vbox, vgap } from './ui-kit.mjs'

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
      navButton('Go_Page_Lists__Nav', '列表'),
      navButton('Go_Page_Controls__Nav', '控件'),
      navButton('Go_Page_Flow__Nav', '流程'),
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
const pageHome = vbox('HomeRoot', [
  node(
    'Border',
    'Hero',
    { Background: rounded('#1C3260', 14, 1, '#2D4D8C'), Padding: margin(28, 24) },
    [
      vbox('HeroCol', [
        text('HeroTitle', '欢迎回来，指挥官', { size: 28, bold: true, slot: vgap(8) }),
        text('HeroDesc', '这是 AssetForge 用 JSON 生成的多页面界面：每一页是一个子 WBP，导入父界面时一起生成。侧栏跳页、页签、返回、弹窗、列表、各种输入控件、计数和定时弹窗都有，用来练习 GameTest 录制和脚本。', {
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
const pageInventory = vbox('InventoryRoot', [
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
const pageShop = vbox('ShopRoot', [
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
const pageTasks = vbox('TasksRoot', [
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
const pageTaskDetail = vbox('TaskDetailRoot', [
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
const pageSettings = vbox('SettingsRoot', [
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
      ]),
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
const pageProfile = hbox('ProfileRoot', [
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

// ---- 列表条目（AssetForge 1.4.0）：GameTest「这一格怎么认」的靶子 ----
// 三个容器在配置里是空的：数据对象和运行时生成的格子由父类 ForgeSampleScreen 构造时填（Sample/ForgeSampleLists.cpp），
// 条目控件是 C++ 类（没有蓝图资源），所以 EntryWidgetClass 直接填 /Script 路径。改数据记得同步这里的场景。
const ENTRY = '/Script/AssetForgeTests.'
const listHead = (id, title, sub, note, tone) =>
  hbox(
    id + 'Head',
    [
      vbox(id + 'HeadText', [text(id + 'Title', title, { size: 16, bold: true, slot: vgap(2) }), text(id + 'Sub', sub, { size: 11, color: T.muted })], {
        ...fill(),
        VerticalAlignment: 'VAlign_Center',
      }),
      node(
        'Border',
        id + 'Note',
        { Background: rounded(tone === 'ok' ? '#173A2A' : '#3A2F12', 6), Padding: margin(8, 3) },
        [text(id + 'NoteText', note, { size: 11, color: tone === 'ok' ? T.success : T.warn })],
        { VerticalAlignment: 'VAlign_Center' },
      ),
    ],
    vgap(12),
  )
const pageLists = vbox('ListsRoot', [
  hbox(
    'ListsHead',
    [
      text('Page_Lists_Title', '列表条目', { size: 20, bold: true, slot: { Padding: margin(0, 0, 16, 0), VerticalAlignment: 'VAlign_Center' } }),
      text('ListsHint', '录制时点下面条目上的按钮：GameTest 挑一个能唯一认出这一条的字段写进选择器，拿不准时暂停游戏、在面板上弹窗问你。', {
        size: 12,
        color: T.muted,
        wrap: true,
        slot: { ...fill(), VerticalAlignment: 'VAlign_Center' },
      }),
    ],
    vgap(14),
  ),
  hbox(
    'ListsBody',
    [
      card(
        'BagCard',
        [
          vbox('BagCol', [
            listHead('Bag', '仓库', 'TileView · 48 格 · 数据对象 ForgeSampleItem', '有 ItemId：自动认，不问', 'ok'),
            node('TileView', 'ItemList', { EntryWidgetClass: ENTRY + 'ForgeBagSlot', EntryWidth: 118, EntryHeight: 146 }, null, fill()),
          ]),
        ],
        { pad: 16, slot: { ...fill(3), Padding: margin(0, 0, 16, 0) } },
      ),
      vbox(
        'ListsRight',
        [
          card(
            'MailCard',
            [
              vbox('MailCol', [
                listHead('Mail', '邮件', 'ListView · 10 封 · 数据对象 ForgeSampleMail', '只有 SentAt 不重复：会问', 'warn'),
                node('ListView', 'MailList', { EntryWidgetClass: ENTRY + 'ForgeMailRow' }, null, fill()),
              ]),
            ],
            { pad: 16, slot: { ...fill(3), Padding: margin(0, 0, 0, 16) } },
          ),
          card(
            'SkillCard',
            [
              vbox('SkillCol', [
                listHead('Skill', '技能栏', 'WrapBox · 运行时生成的 8 个 ForgeSkillCell', '要 SkillType + Level 组合：会问', 'warn'),
                node('WrapBox', 'SkillBar', {}, null, fill()),
              ]),
            ],
            { pad: 16, slot: fill(2) },
          ),
        ],
        fill(2),
      ),
    ],
    fill(),
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
  popup('Notice', '活动公告', '限时活动开始了！这个公告是「流程」页的按钮定时弹出来的：不定时弹出、挡住后面操作的界面，GameTest 用守护处理掉它。', [ok('Notice', '知道了')]),
  popup('Help', '帮助', '侧栏切页，页签切子页，「返回」先关最上面的弹窗、再回上一页。右上角的状态条显示最近一次操作的结果，GameTest 可以断言它。', [ok('Help', '知道了')], { width: 520 }),
]

export { sidebar, topbar, pageHome, pageInventory, pageShop, pageTasks, pageTaskDetail, pageSettings, pageProfile, pageLists, popups }
