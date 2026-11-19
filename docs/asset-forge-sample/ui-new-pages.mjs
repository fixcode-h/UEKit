// AssetForge 1.5.0 加的两页：「控件」（每种能操作的 UMG 控件各一个，GameTest 的 set / type / key / select / scroll / 菜单都有靶子）
// 和「流程」（计数、领取、延时出现、显示切换、定时弹窗，给 GameTest 脚本的循环、分支、等待、跳过、守护、调函数当靶子）。
// 按钮的行为全靠示例界面 C++ 父类的名字约定（Sample/ForgeSampleScreen.h、ForgeSampleFlow.cpp），这里只摆控件。
import { T, button, card, color, fill, hbox, margin, node, rounded, sized, spacer, text, vbox, vgap, gap } from './ui-kit.mjs'

const label = (name, value, extra = {}) => text(name, value, { size: 14, slot: { VerticalAlignment: 'VAlign_Center', Padding: margin(0, 0, 12, 0) }, ...extra })
const echo = (name, value) => text(name, value, { size: 14, bold: true, color: T.accent, slot: { VerticalAlignment: 'VAlign_Center', Padding: margin(12, 0, 0, 0) } })
const row = (name, children, bottom = 14) => hbox(name, children, vgap(bottom))
const section = (name, title, sub, children, slot) =>
  card(
    name,
    [vbox(name + 'Col', [text(name + 'Title', title, { size: 16, bold: true, slot: vgap(2) }), text(name + 'Sub', sub, { size: 11, color: T.muted, slot: vgap(14) }), ...children])],
    { pad: 18, slot: slot ?? vgap(14) },
  )
const field = (name, w, child) => sized(name, w, null, child, { VerticalAlignment: 'VAlign_Center' })
const inputBg = (name, child) =>
  node('Border', name, { Background: rounded('#121722', 8, 1, T.line), Padding: margin(10, 6) }, [child], { ...fill(), VerticalAlignment: 'VAlign_Center', Padding: margin(0, 0, 10, 0) })

// ---- 控件页 ----
// 「勾上才能点」一开始禁用（勾上 Enable_Btn_Gated 才可用）：练 enabled 判断
const gated = button('Btn_Gated', '勾上才能点', { kind: 'accent' })
gated.props.bIsEnabled = false
const buttonsCard = section('BtnCard', '按钮', '普通点击、禁用到可用、长按、悬停', [
  row('BtnRow1', [
    button('Btn_Normal', '普通按钮', { kind: 'neutral', slot: gap }),
    gated,
  ]),
  node('CheckBox', 'Enable_Btn_Gated', {}, [text('L_Enable_Btn_Gated', '允许点上面那个按钮', { size: 14 })], vgap(14)),
  row('HoldRow', [button('Hold_Charge', '按住蓄力（1 秒）', { kind: 'warn' }), echo('Hold_Charge_Value', '未按')]),
  row('HoverRow', [button('Hover_Card', '把鼠标移上来', { kind: 'ghost' }), echo('Hover_Card_Value', '未悬停')], 0),
])

const textCard = section('TextCard', '文字输入', '单行（无边框 / 带边框）、多行；带边框的按回车提交', [
  text('Nickname_Title', '昵称（EditableText）', { size: 12, color: T.muted, slot: vgap(6) }),
  row('NicknameRow', [
    inputBg('NicknameBg', node('EditableText', 'Nickname', { HintText: '输入昵称' })),
    button('Submit_Nickname', '保存', { kind: 'accent', slot: { VerticalAlignment: 'VAlign_Center' } }),
    echo('Nickname_Echo', '未设置'),
  ]),
  text('SearchBox_Title', '搜索（EditableTextBox，回车提交）', { size: 12, color: T.muted, slot: vgap(6) }),
  node('EditableTextBox', 'SearchBox', { HintText: '输入后按回车' }, null, vgap(14)),
  text('Feedback_Title', '反馈（MultiLineEditableTextBox）', { size: 12, color: T.muted, slot: vgap(6) }),
  sized('FeedbackSize', null, 72, node('MultiLineEditableTextBox', 'Feedback', { HintText: '可以换行' }), vgap(8)),
  row('FeedbackRow', [button('Submit_Feedback', '提交反馈', { kind: 'accent' }), echo('Feedback_Echo', '未提交')], 0),
])

const numberCard = section('NumCard', '数值', '滑条（拖动或直接设值）、微调框', [
  row('BrightnessRow', [
    label('Brightness_Title', '亮度'),
    field('BrightnessSize', 220, node('Slider', 'Brightness', { Value: 50, MinValue: 0, MaxValue: 100, StepSize: 1, SliderBarColor: color('#3A4560'), SliderHandleColor: color('#5A95FF') })),
    echo('Brightness_Value', '50'),
  ], 8),
  node('ProgressBar', 'Brightness_Bar', { Percent: 0.5, FillColorAndOpacity: color('#F5B04A') }, null, vgap(16)),
  row('AmountRow', [label('Amount_Title', '数量'), field('AmountSize', 120, node('SpinBox', 'Amount', { Value: 1 })), echo('Amount_Value', '1')], 0),
])

const choiceCard = section('ChoiceCard', '选择', '下拉框、复选框、单选按钮组、按键选择器', [
  row('ServerRow', [label('Server_Title', '服务器'), field('ServerSize', 200, node('ComboBoxString', 'Server', { DefaultOptions: ['华东一区', '华南二区', '海外'], SelectedOption: '华东一区' })), echo('Server_Value', '华东一区')]),
  row('AutoLootRow', [node('CheckBox', 'AutoLoot', {}, [text('L_AutoLoot', '自动拾取', { size: 14 })], { VerticalAlignment: 'VAlign_Center' }), echo('AutoLoot_Value', '关')]),
  row('DifficultyRow', [
    label('Difficulty_Title', '难度'),
    button('Pick_Difficulty__Easy', '简单', { kind: 'ghost', slot: gap }),
    button('Pick_Difficulty__Normal', '普通', { kind: 'ghost', slot: gap }),
    button('Pick_Difficulty__Hard', '困难', { kind: 'ghost' }),
    echo('Difficulty', '普通'),
  ]),
  row('JumpKeyRow', [label('JumpKey_Title', '跳跃键'), field('JumpKeySize', 160, node('InputKeySelector', 'JumpKey', {})), echo('JumpKey_Value', '未设置')], 0),
])

const logs = Array.from({ length: 20 }, (_, i) => i + 1)
const boxCard = section('BoxCard', '容器', '长列表（滚动、滚轮）、页签切换器', [
  hbox('LogHead', [text('LogLabel', '已选：', { size: 12, color: T.muted }), text('LogPicked', '无', { size: 12, color: T.accent })], vgap(6)),
  sized(
    'LogSize',
    null,
    150,
    node(
      'ScrollBox',
      'LogList',
      {},
      logs.map((i) => button('Pick_LogPicked__' + i, `日志 ${String(i).padStart(2, '0')} · ${['登录', '战斗', '商店', '任务'][i % 4]}`, { kind: 'ghost', align: 'HAlign_Left', slot: { Padding: margin(0, 0, 0, 4) } })),
    ),
    vgap(14),
  ),
  row('StepTabs', [button('Go_StepA', '第一步', { kind: 'ghost', slot: gap }), button('Go_StepB', '第二步', { kind: 'ghost', slot: gap }), button('Go_StepC', '第三步', { kind: 'ghost' })], 8),
  node('WidgetSwitcher', 'Steps', {}, [
    text('StepA', '第一步：填写资料', { size: 14 }),
    text('StepB', '第二步：选择服务器', { size: 14 }),
    text('StepC', '第三步：确认', { size: 14 }),
  ]),
])

const menuCard = section(
  'MenuCard',
  '菜单',
  '菜单锚点：按钮开菜单，菜单里的项是另一个窗口里的控件',
  [
    row('MenuRow', [
      node('MenuAnchor', 'ActionAnchor', { MenuClass: '/Script/AssetForgeTests.ForgeSampleMenu', Placement: 'MenuPlacement_BelowAnchor' }, [text('ActionAnchorText', '（菜单从这里弹出）', { size: 12, color: T.muted })], { VerticalAlignment: 'VAlign_Center', Padding: margin(0, 0, 12, 0) }),
      button('Menu_ActionAnchor', '更多操作 ▾', { kind: 'neutral' }),
    ], 0),
  ],
  { Padding: margin(0) },
)

export const pageControls = vbox('ControlsRoot', [
  hbox('ControlsHead', [
    text('Page_Controls_Title', '控件', { size: 20, bold: true, slot: { Padding: margin(0, 0, 16, 0), VerticalAlignment: 'VAlign_Center' } }),
    text('ControlsHint', '每种能操作的控件各一个：GameTest 的点击、打字、按键、设值、滚动、菜单都能在这页练；结果写在控件右边和顶栏状态里。', {
      size: 12,
      color: T.muted,
      wrap: true,
      slot: { ...fill(), VerticalAlignment: 'VAlign_Center' },
    }),
  ], vgap(14)),
  node('ScrollBox', 'ControlsScroll', {}, [
    hbox('ControlsBody', [
      vbox('ControlsLeft', [buttonsCard, textCard, numberCard], { ...fill(), Padding: margin(0, 0, 16, 0) }),
      vbox('ControlsRight', [choiceCard, boxCard, menuCard], fill()),
    ]),
  ], fill()),
])

// ---- 流程页 ----
const claimRow = (id, title, last) =>
  row('ClaimRow_' + id, [label('ClaimName_' + id, title, { slot: { ...fill(), VerticalAlignment: 'VAlign_Center' } }), button('Claim_' + id, '领取', { kind: 'success' })], last ? 0 : 10)
const reveal = (id, title, children) =>
  node('Border', 'Reveal_' + id, { Visibility: 'Collapsed', Background: rounded('#173A2A', 10, 1, '#2D6B4A'), Padding: margin(14, 10) }, [
    vbox('RevealCol_' + id, [text('Reveal_' + id + '_Title', title, { size: 15, bold: true, color: T.success, slot: vgap(6) }), ...children]),
  ], { Padding: margin(0, 12, 0, 0) })

export const pageFlow = vbox('FlowRoot', [
  hbox('FlowHead', [
    text('Page_Flow_Title', '流程', { size: 20, bold: true, slot: { Padding: margin(0, 0, 16, 0), VerticalAlignment: 'VAlign_Center' } }),
    text('FlowHint', '给 GameTest 脚本练手：计数配循环和分支，领取配「已达成就跳过」，延时出现配等待，定时弹公告配守护。离开界面或重置后恢复原样。', {
      size: 12,
      color: T.muted,
      wrap: true,
      slot: { ...fill(), VerticalAlignment: 'VAlign_Center' },
    }),
  ], vgap(14)),
  node('ScrollBox', 'FlowScroll', {}, [
    hbox('FlowBody', [
      vbox('FlowLeft', [
        section('CountCard', '计数', '循环几次、按到某个数为止、按数分支', [
          hbox('CountRow', [
            text('Counter_Title', '计数', { size: 14, color: T.muted, slot: { VerticalAlignment: 'VAlign_Center', Padding: margin(0, 0, 12, 0) } }),
            text('Counter', '0', { size: 36, bold: true, color: '#F5C451', slot: { ...fill(), VerticalAlignment: 'VAlign_Center' } }),
            button('Dec_Counter', '-1', { kind: 'neutral', slot: { ...gap, VerticalAlignment: 'VAlign_Center' } }),
            button('Inc_Counter', '+1', { kind: 'accent', slot: { ...gap, VerticalAlignment: 'VAlign_Center' } }),
            button('Reset_Counter', '归零', { kind: 'ghost', slot: { VerticalAlignment: 'VAlign_Center' } }),
          ]),
        ]),
        section('ClaimCard', '领取', '点过一次按钮变成「已领取」：回放时已经领过就该跳过', [claimRow('Daily', '每日签到'), claimRow('Weekly', '每周宝箱'), claimRow('Mail', '系统邮件', true)]),
      ], { ...fill(), Padding: margin(0, 0, 16, 0) }),
      vbox('FlowRight', [
        section('WaitCard', '等待', '点了不马上出现：1.5 秒后才显示，脚本要等它', [
          button('Delay_Reveal_Loot', '1.5 秒后出现宝箱', { kind: 'accent', slot: { HorizontalAlignment: 'HAlign_Left' } }),
          reveal('Loot', '宝箱出现了', [button('Open_Popup_Reward__Loot', '打开宝箱', { kind: 'success', tip: '宝箱：金币 ×100', slot: { HorizontalAlignment: 'HAlign_Left' } })]),
        ]),
        section('BranchCard', '分支', '提示时有时无：脚本按它在不在走不同的路', [
          button('Toggle_Reveal_Tip', '显示 / 隐藏提示', { kind: 'neutral', slot: { HorizontalAlignment: 'HAlign_Left' } }),
          reveal('Tip', '提示', [text('TipBody', '提示面板显示着：这条路要先把它关掉。', { size: 13, color: '#B6C0D3', wrap: true })]),
        ]),
        section('GuardCard', '守护', '公告会在 1 秒后自己弹出来、挡住界面：脚本里的守护看到它就关掉', [
          button('Arm_Popup_Notice', '1 秒后弹公告', { kind: 'warn', slot: { HorizontalAlignment: 'HAlign_Left' } }),
        ], { Padding: margin(0) }),
      ], fill()),
    ]),
  ], fill()),
])
