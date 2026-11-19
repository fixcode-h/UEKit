// 示例界面配套的 GameTest 场景（schemaVersion 3，要 GameTest 1.9.0 起）。gen.mjs 调 buildScenarios 生成，选择器路径由 sel(控件名) 算。
// 分工：冒烟 = 跳页、弹窗、返回的主路径；条目标识 = 列表按数据认条目、选中、滚动；控件操作 = 每种控件的 set / type / 点击 / 菜单和观察类动作；
// 流程脚本 = v3 脚本的块、条件、表达式、守护、调函数、接口；真实输入 = 真实鼠标和键盘（悬停、长按、拖动、滚轮、下拉框、按键）。
// 每份都先 call AssetForgeSample/common/打开示例界面（进图、重置界面、切到要测的页），所以单独跑哪一份都行、顺序无关。

export function buildScenarios({ sel, MAP }) {
  const OPEN = 'AssetForgeSample/common/打开示例界面' // call 的路径相对场景根；示例用例装在场景根的 AssetForgeSample/ 下
  const ROOT = 'WBP_ForgeSample:.'
  const nav = (page) => sel(`Go_Page_${page}__Nav`)
  const base = { crumb: sel('Crumb'), status: sel('Status'), back: sel('Back') }
  const click = (id, name, note, extra = {}) => ({ id, action: 'click', target: sel(name), ...extra, ...(note ? { note } : {}) })
  const open = (page, title) => ({
    id: 'open', action: 'call', script: OPEN, with: page ? { nav: nav(page), title } : {}, saveAs: 'opened',
    note: '进图、重置示例界面' + (page ? `、切到「${title}」页` : ''),
  })
  const closeGroup = (shot) => ({
    id: 'close', action: 'group', name: '截图收尾',
    do: [
      ...(shot ? [{ id: 'shot', action: 'screenshot', name: shot }] : []),
      { id: 'stop', action: 'pie.stop' },
      { id: 'stopped', action: 'wait', until: 'pieStopped' },
    ],
  })
  const statusHas = (id, s) => ({ id, action: 'assert', expr: `contains(text($status), '${s}')` })
  const scenario = (name, note, tags, rest) => ({
    kind: 'gametest-scenario', schemaVersion: 3, name, _note: note, revision: 1, tags: ['sample', 'assetforge', ...tags], stepDelayMs: 200, ...rest,
  })

  // ---- 公共子脚本 ----
  const common = {
    kind: 'gametest-scenario',
    schemaVersion: 3,
    name: '打开示例界面（子脚本）',
    _note: '给其他示例场景 call 的子脚本：进图（PIE 已经在跑这张图就沿用）、等界面出来、把示例界面重置回首页，再点侧栏切到要测的页。返回切完之后顶栏面包屑的文字。',
    revision: 1,
    tags: ['sample', 'assetforge', 'common'],
    params: {
      nav: { default: '', type: 'string', note: '侧栏按钮的选择器；空 = 停在首页' },
      title: { default: '首页', type: 'string', note: '切过去之后面包屑应该显示的页名' },
    },
    vars: { crumb: base.crumb },
    steps: [
      { id: 'pie', action: 'pie.start', map: MAP },
      { id: 'waitUi', action: 'wait', until: 'widget', target: '$crumb', note: 'GameMode 进图就把界面加进视口' },
      { id: 'reset', action: 'widget.call', target: ROOT, function: 'ResetScreen', note: '回首页、关弹窗、清计数和领取记录：上一份场景留下的状态不影响这一份' },
      { id: 'goPage', action: 'if', cond: { expr: "$nav != ''" }, then: [{ id: 'clickNav', action: 'click', target: '$nav' }] },
      { id: 'atPage', action: 'assert', target: '$crumb', text: '$title' },
      { id: 'done', action: 'return', value: '=text($crumb)' },
    ],
  }

  // ---- 冒烟：跳页、弹窗、返回 ----
  const smoke = scenario(
    'AssetForge 示例界面：跳页、弹窗、返回',
    '配套 AssetForge 示例测试界面（先在 AssetForge 页导入 Test/umg/WBP_ForgeSample.json，会连带导入 pages/ 下的子页面；再导入 map/）。每页一段 group、参数和变量、表达式断言、loop until 连按返回、块里的守护。',
    ['smoke'],
    {
      revision: 3,
      params: {
        playerName: { default: 'Tester01', type: 'string', note: '资料页填的名字' },
        quality: { default: '极高', type: 'string', note: '设置页视频画质选哪一档' },
      },
      vars: { ...base },
      steps: [
        open(null),
        {
          id: 'shop', action: 'group', name: '商店：买红宝石，弹窗上再叠一层',
          do: [
            click('goShop', 'Go_Page_Shop__Hero', '首页大图上的「去商店」'),
            { id: 'atShop', action: 'assert', target: '$crumb', text: '商店' },
            click('buyGem', 'Open_Popup_Buy__Gem', '买红宝石'),
            {
              id: 'buyShown', action: 'assert', note: '购买弹窗出来了，而且买的是红宝石',
              cond: { all: [{ target: sel('Popup_Buy'), visible: true }, { target: sel('Popup_Buy_Subject'), textContains: '红宝石' }] },
            },
            click('openHelp', 'Open_Popup_Help__Buy', '弹窗上再弹一层说明'),
            { id: 'helpShown', action: 'assert', target: sel('Popup_Help'), visible: true },
            { id: 'backHelp', action: 'click', target: '$back', note: '返回先关最上面的弹窗' },
            { id: 'buyStill', action: 'assert', target: sel('Popup_Buy'), visible: true },
            click('buyOk', 'Close_Popup_Buy__Ok'),
            statusHas('buyClosed', '关闭'),
          ],
        },
        {
          id: 'settings', action: 'group', name: '设置：视频画质',
          do: [
            click('goSettings', 'Go_Page_Settings__Nav'),
            click('goVideo', 'Go_Tab_Video'),
            { id: 'setQuality', action: 'set', target: sel('Quality'), option: '$quality' },
            { id: 'qualityEcho', action: 'assert', target: sel('Quality_Value'), text: '$quality' },
          ],
        },
        {
          id: 'profile', action: 'group', name: '资料：改名后返回',
          do: [
            click('goProfile', 'Go_Page_Profile__Nav'),
            { id: 'typeName', action: 'type', target: sel('PlayerName'), text: '$playerName' },
            click('submitName', 'Submit_PlayerName'),
            { id: 'readEcho', action: 'set', var: 'echo', from: { target: sel('PlayerName_Echo'), read: 'text' }, note: '把回显的名字读进变量，下一步拿它和参数比' },
            { id: 'echoOk', action: 'assert', expr: '$echo == $playerName' },
            { id: 'backToSettings', action: 'click', target: '$back' },
            { id: 'atSettings', action: 'assert', target: '$crumb', text: '设置' },
          ],
        },
        {
          id: 'tasks', action: 'group', name: '任务：进详情、跳商店、连按返回回到任务页',
          guards: [
            {
              id: 'strayBuy', name: '购买弹窗残留', when: { target: sel('Popup_Buy'), visible: true },
              do: [{ action: 'click', target: sel('Close_Popup_Buy__Ok') }], unlessStepTargets: false, max: 3,
              note: '这一段不该有购买弹窗：万一残留挡住了导航，先关掉再接着做',
            },
          ],
          do: [
            click('goTasks', 'Go_Page_Tasks__Nav'),
            click('goWeekly', 'Go_Tab_Weekly'),
            click('openDetail', 'Go_Page_TaskDetail__Weekly1', '从任务列表进详情页（侧栏里没有这一页）'),
            { id: 'atDetail', action: 'assert', cond: { all: [{ target: '$crumb', text: '任务详情' }, { target: sel('Page_TaskDetail_Subject'), textContains: '熔火之心' }] } },
            click('detailToShop', 'Go_Page_Shop__Task'),
            { id: 'atShop2', action: 'assert', target: '$crumb', text: '商店' },
            {
              id: 'backHome', action: 'loop', until: { target: '$crumb', text: '任务' }, max: 4,
              note: '返回历史：商店 → 任务详情 → 任务，按到回到任务页为止',
              do: [{ id: 'pressBack', action: 'click', target: '$back' }],
            },
          ],
        },
        closeGroup('forge-sample-tasks'),
      ],
    },
  )

  // ---- 条目标识：列表按数据认条目、选中、滚动 ----
  const itemKeys = scenario(
    'AssetForge 示例界面：按条目标识点列表',
    '配套「列表条目」页。三种容器写成一张数据表，用 loop in 逐行点、逐行核对状态栏：加一种条目只加一行。后半段用 select 按数据选中、scroll 按序号滚动。手动录制时点这页条目上的按钮：仓库按 ItemId 自动认，邮件和技能栏会暂停游戏、在面板上弹窗问用哪个字段认。',
    ['item-key'],
    {
      revision: 3,
      vars: {
        ...base,
        cases: [
          { name: '仓库：按 ItemId 认，第 30 格没滚到先滚过去', target: sel('ItemList') + '[ItemId=1030]>SlotBg/SlotCol/BtnUse', expect: 'ItemId 1030' },
          { name: '邮件：按发送时间认第 6 封', target: sel('MailList') + '[SentAt=202611101400]>RowPad/RowBg/RowCols/BtnClaim', expect: '11-10 14:00' },
          { name: '技能栏：两个字段一起才唯一，2 级治疗', target: sel('SkillBar') + '[SkillType=13,Level=2]>CellBg/CellWidth/CellCol/BtnCast', expect: 'SkillType 13' },
        ],
      },
      steps: [
        open('Lists', '列表条目'),
        {
          id: 'cases', action: 'loop', in: '$cases', as: 'row', max: 20,
          note: '每行：按条目标识点按钮，状态栏写出点到的是哪一条',
          do: [
            { id: 'clickRow', action: 'click', target: '$row.target' },
            { id: 'checkRow', action: 'assert', expr: 'contains(text($status), $row.expect)' },
          ],
        },
        {
          id: 'pick', action: 'group', name: '选中和滚动：直接改列表状态（准备数据用，录制器不写这些）',
          do: [
            { id: 'selectItem', action: 'select', target: sel('ItemList'), property: 'ItemId', equals: 1040, note: '按数据对象的属性选中' },
            { id: 'selectedItem', action: 'assert', target: sel('ItemList'), selectedProperty: 'ItemId', equals: 1040 },
            { id: 'selectMail', action: 'select', target: sel('MailList'), index: 3, note: '按序号选中第 4 封' },
            { id: 'mailSelected', action: 'assert', target: sel('MailList'), stateKey: 'selectedCount', equals: 1 },
            { id: 'scrollLast', action: 'scroll', target: sel('ItemList'), index: 47, note: '列表族用 index 滚到最后一格' },
            { id: 'lastShown', action: 'assert', target: sel('ItemList') + '[ItemId=1048]', exists: true },
            { id: 'itemCount', action: 'api', api: 'Widget.ItemCount', args: { target: sel('ItemList') }, saveAs: 'itemCount', expect: { equals: 48 }, note: '条目数含没滚到的（按数据对象数）' },
          ],
        },
        closeGroup('forge-sample-lists'),
      ],
    },
  )

  // ---- 控件操作：每种控件 ----
  const menuItem = (n) => `ForgeSampleMenu:MenuBg/MenuCol/${n}`
  const controls = scenario(
    'AssetForge 示例界面：各类控件操作',
    '配套「控件」页（AssetForge 1.5.0 起）。每种能操作的 UMG 控件各练一遍：按钮（普通、禁用到可用）、单行 / 多行输入框、滑条、微调框、下拉框、复选框、单选按钮组、按键选择器、滚动框、切换器、菜单锚点；最后一段是观察类动作（ping、find、snapshot、probe、标注截图）和临时加界面。都用代码方式（不动鼠标），真实鼠标键盘见「真实输入」。',
    ['controls'],
    {
      params: {
        nickname: { default: '火焰骑士', type: 'string', note: '昵称框填什么' },
        server: { default: '海外', type: 'string', note: '下拉框选哪一项' },
      },
      vars: { ...base },
      steps: [
        open('Controls', '控件'),
        {
          id: 'buttons', action: 'group', name: '按钮：普通、禁用到可用、短按',
          do: [
            click('clickNormal', 'Btn_Normal'),
            statusHas('normalDone', '普通按钮'),
            { id: 'lockGate', action: 'set', target: sel('Enable_Btn_Gated'), checked: false, note: '复选框 set 会广播：父类跟着把按钮禁用' },
            { id: 'gateLocked', action: 'assert', target: sel('Btn_Gated'), enabled: false },
            { id: 'openGate', action: 'set', target: sel('Enable_Btn_Gated'), checked: true },
            { id: 'gateOpen', action: 'assert', target: sel('Btn_Gated'), enabled: true },
            click('clickGated', 'Btn_Gated'),
            statusHas('gatedDone', '勾上才能点'),
            click('tapHold', 'Hold_Charge', '代码点击在同一帧按下松开：算短按（长按见「真实输入」）'),
            { id: 'tapShort', action: 'assert', target: sel('Hold_Charge_Value'), text: '短按' },
          ],
        },
        {
          id: 'texts', action: 'group', name: '文字输入：单行、回车提交、多行',
          do: [
            { id: 'typeNick', action: 'type', target: sel('Nickname'), text: '$nickname' },
            click('saveNick', 'Submit_Nickname'),
            { id: 'nickEcho', action: 'assert', target: sel('Nickname_Echo'), text: '$nickname' },
            { id: 'typeSearch', action: 'type', target: sel('SearchBox'), text: '火焰剑', commit: true, note: 'commit：打完按回车' },
            statusHas('searchCommitted', '输入：火焰剑'),
            { id: 'typeFeedback', action: 'type', target: sel('Feedback'), text: '第一行\n第二行', via: 'code', note: '多行框用代码写入（换行符直接写进去）' },
            { id: 'appendFeedback', action: 'type', target: sel('Feedback'), text: '（补充）', via: 'code', append: true },
            click('sendFeedback', 'Submit_Feedback'),
            { id: 'feedbackEcho', action: 'assert', cond: { all: [{ target: sel('Feedback_Echo'), textContains: '第二行' }, { target: sel('Feedback_Echo'), textContains: '（补充）' }] } },
          ],
        },
        {
          id: 'numbers', action: 'group', name: '数值：滑条、微调框',
          do: [
            { id: 'setBright', action: 'set', target: sel('Brightness'), value: 80, note: '滑条 set 不广播 OnValueChanged：右边的数字不变，用 stateKey 看滑条自己的值' },
            { id: 'brightValue', action: 'assert', target: sel('Brightness'), stateKey: 'value', equals: 80 },
            { id: 'setAmount', action: 'set', target: sel('Amount'), value: 5, note: '微调框 set 会广播' },
            { id: 'amountEcho', action: 'assert', target: sel('Amount_Value'), number: { eq: 5 } },
          ],
        },
        {
          id: 'choices', action: 'group', name: '选择：下拉框、复选框、单选按钮组、按键选择器',
          do: [
            { id: 'pickServer', action: 'set', target: sel('Server'), option: '$server' },
            { id: 'serverEcho', action: 'assert', target: sel('Server_Value'), text: '$server' },
            { id: 'pickServerByIndex', action: 'set', target: sel('Server'), index: 1, note: '按序号选第二项' },
            { id: 'serverIndex', action: 'assert', cond: { all: [{ target: sel('Server_Value'), text: '华南二区' }, { target: sel('Server'), stateKey: 'index', equals: 1 }] } },
            { id: 'lootOff', action: 'set', target: sel('AutoLoot'), checked: false },
            click('lootToggle', 'AutoLoot', '代码点击复选框：切换勾选'),
            { id: 'lootOn', action: 'assert', cond: { all: [{ target: sel('AutoLoot'), checked: true }, { target: sel('AutoLoot_Value'), text: '开' }] } },
            click('pickHard', 'Pick_Difficulty__Hard', '一组按钮当单选'),
            { id: 'hardPicked', action: 'assert', target: sel('Difficulty'), text: '困难' },
            { id: 'bindJump', action: 'set', target: sel('JumpKey'), key: 'SpaceBar', note: '按键选择器 set 不广播：看控件自己的 key' },
            { id: 'jumpBound', action: 'assert', target: sel('JumpKey'), stateKey: 'key', equals: 'SpaceBar' },
          ],
        },
        {
          id: 'containers', action: 'group', name: '容器：滚动框、切换器',
          do: [
            { id: 'scrollEnd', action: 'scroll', target: sel('LogList'), mode: 'end' },
            click('pickLast', 'Pick_LogPicked__20', '滚到底再点最后一条'),
            { id: 'lastPicked', action: 'assert', target: sel('LogPicked'), textContains: '日志 20' },
            { id: 'scrollStart', action: 'scroll', target: sel('LogList'), mode: 'start' },
            { id: 'atTop', action: 'wait', until: 'cond', cond: { target: sel('LogList'), stateKey: 'offset', equals: 0 }, timeoutMs: 3000, note: '滚动在下一帧布局时才生效：等它读回 0' },
            { id: 'scrollTo12', action: 'scroll', target: sel('LogList'), mode: 'widget', widget: sel('Pick_LogPicked__12'), note: '滚到某个子控件露出来' },
            click('pick12', 'Pick_LogPicked__12'),
            { id: 'picked12', action: 'assert', target: sel('LogPicked'), textContains: '日志 12' },
            click('stepB', 'Go_StepB', '按钮切切换器的页'),
            { id: 'onStepB', action: 'assert', target: sel('Steps'), stateKey: 'activeIndex', equals: 1 },
            { id: 'setStepC', action: 'set', target: sel('Steps'), index: 2, note: '直接设切换器页号' },
            { id: 'onStepC', action: 'assert', target: sel('StepC'), visible: true },
          ],
        },
        {
          id: 'observe', action: 'group', name: '观察：连通、标记、句柄、快照、命中测试、标注截图、临时界面',
          do: [
            { id: 'ping', action: 'ping' },
            { id: 'mark', action: 'mark', text: '下面这段只观察，不改界面' },
            { id: 'findBtn', action: 'find', target: sel('Btn_Normal'), into: 'normalBtn', note: '捕获控件句柄，后面用 @normalBtn 引用' },
            { id: 'clickHandle', action: 'click', target: '@normalBtn' },
            statusHas('handleClicked', '普通按钮'),
            { id: 'snap', action: 'snapshot', target: sel('ChoiceCard'), props: true, note: '只取「选择」卡片这棵子树' },
            { id: 'probe', action: 'probe', target: sel('Btn_Normal'), note: '只读命中测试：按钮中心有没有被挡住' },
            { id: 'shotAnnotated', action: 'screenshot', name: 'forge-sample-controls', annotate: true },
            { id: 'addMenu', action: 'widget.add', class: '/Script/AssetForgeTests.ForgeSampleMenu', zOrder: 50, note: '不靠游戏流程，直接把一个界面加进视口（放在菜单那段之前：屏上只有这一个 ForgeSampleMenu）' },
            { id: 'menuAdded', action: 'wait', until: 'widget', target: menuItem('MenuDelete') },
            { id: 'removeMenu', action: 'widget.remove', target: 'ForgeSampleMenu:.' },
            { id: 'menuRemoved', action: 'wait', until: 'widgetGone', target: menuItem('MenuDelete') },
          ],
        },
        {
          id: 'menu', action: 'group', name: '菜单锚点：按钮开菜单、点菜单项、直接设开合',
          do: [
            click('openMenu', 'Menu_ActionAnchor'),
            { id: 'menuOpen', action: 'assert', target: sel('ActionAnchor'), stateKey: 'open', equals: true },
            { id: 'pickRename', action: 'click', target: menuItem('MenuRename'), note: '菜单内容是另一个界面 ForgeSampleMenu（在弹出窗口里）' },
            statusHas('renamed', '菜单：重命名'),
            { id: 'menuClosed', action: 'assert', target: sel('ActionAnchor'), stateKey: 'open', equals: false },
            { id: 'setOpen', action: 'set', target: sel('ActionAnchor'), open: true },
            { id: 'menuShown', action: 'wait', until: 'widget', target: menuItem('MenuCopy') },
            { id: 'setClosed', action: 'set', target: sel('ActionAnchor'), open: false },
            { id: 'menuGone', action: 'wait', until: 'widgetGone', target: menuItem('MenuCopy') },
          ],
        },
        closeGroup(null),
      ],
    },
  )

  // ---- 流程脚本：v3 脚本的块、条件、表达式、守护、调函数、接口 ----
  const claimSel = (what) => sel('Claim_Daily').replace(/Daily/g, what)
  const counter = sel('Counter')
  const flow = scenario(
    'AssetForge 示例界面：脚本流程',
    '配套「流程」页（AssetForge 1.5.0 起）。把 GameTest 脚本的能力过一遍：参数和变量、子脚本 call / return、对象定位器、invoke 调界面函数、接口和表达式、loop 四种写法和 break / continue、if / elif / else、已达成就跳过（skipIf + also）、等待条件、守护处理定时弹出的公告、块级重试和时限、expectNoErrors、disabled、widget.call。',
    ['script'],
    {
      params: {
        times: { default: 3, type: 'int', note: '「计数」先按几次 +1' },
        claims: { default: ['Daily', 'Weekly', 'Mail'], type: 'any', note: '要领取的项（Claim_<项>）' },
      },
      vars: { ...base, counter, tipWas: '', total: 0 },
      objects: { screen: { class: 'ForgeSampleScreen', in: 'widgets' } },
      guards: [
        {
          id: 'notice', name: '活动公告弹出来了', when: { target: sel('Popup_Notice'), visible: true },
          do: [{ action: 'click', target: sel('Close_Popup_Notice__Ok') }], check: 'both', after: 'retry', max: 5, cooldownMs: 500,
          note: '「1 秒后弹公告」按钮弹的公告随时会挡住界面：不管在做哪一步，看到就点「知道了」',
        },
      ],
      steps: [
        open('Flow', '流程'),
        { id: 'openedFlow', action: 'assert', expr: "$opened == '流程'", note: '子脚本的返回值存在 $opened' },
        {
          id: 'count', action: 'group', name: '计数：四种循环、break / continue', expectNoErrors: true,
          do: [
            { id: 'incTimes', action: 'loop', times: '$times', do: [click('inc1', 'Inc_Counter')] },
            { id: 'countIsTimes', action: 'assert', target: '$counter', number: { eq: '$times' } },
            {
              id: 'incUntil', action: 'loop', until: { target: '$counter', number: { gte: 5 }, settleMs: 0 }, max: 10,
              do: [click('inc2', 'Inc_Counter')], note: '按到 5 为止：每轮做完再看',
            },
            { id: 'decWhile', action: 'loop', while: { expr: 'number($counter) > 2' }, max: 10, do: [click('dec1', 'Dec_Counter')], note: '大于 2 就继续减：每轮开始前看' },
            { id: 'atTwo', action: 'assert', expr: 'number($counter) == 2' },
            {
              id: 'addEach', action: 'loop', in: [1, 2, 3], as: 'step',
              do: [{ id: 'addStep', action: 'invoke', object: { $obj: 'screen' }, function: 'AddCounter', args: { InDelta: '$step' }, saveAs: 'last' }],
              note: '逐个加 1、2、3：调界面上的 BlueprintCallable 函数，返回值存 $last',
            },
            { id: 'lastIs8', action: 'assert', expr: '$last == 8 and number($counter) == 8' },
            { id: 'getCounter', action: 'invoke', object: { $obj: 'screen' }, function: 'GetCounter', saveAs: 'got', expect: { equals: 8 } },
            { id: 'double', action: 'set', var: 'doubled', value: '=$got * 2' },
            { id: 'doubledIs16', action: 'assert', value: '$doubled', equals: 16 },
            { id: 'zero', action: 'invoke', object: { $obj: 'screen' }, function: 'SetCounter', args: { InValue: 0 } },
            {
              id: 'evenOnly', action: 'loop', times: 10,
              note: '只在偶数轮 +1，到 3 就出循环：第 0、2、4 轮各加一次',
              do: [
                { id: 'skipOdd', action: 'continue', cond: { expr: '$index % 2 == 1' } },
                click('inc3', 'Inc_Counter'),
                { id: 'enough', action: 'break', cond: { expr: 'number($counter) >= 3' } },
              ],
            },
            { id: 'countIs3', action: 'assert', value: '@obj.screen.Counter', equals: 3, note: '直接读界面的 UPROPERTY' },
          ],
        },
        {
          id: 'branch', action: 'group', name: '分支：if / elif / else',
          do: [
            {
              id: 'byCount', action: 'if',
              cond: { target: '$counter', number: { gte: 5 } },
              then: [{ id: 'tooMany', action: 'fail', message: "='计数不该到 5：' + text($counter)" }],
              elif: [{ cond: { expr: 'number($counter) == 3' }, then: [{ id: 'isThree', action: 'mark', text: '计数是 3，走 elif' }] }],
              else: [{ id: 'unexpected', action: 'fail', message: "='计数应该是 3，实际 ' + text($counter)" }],
            },
            click('showTip', 'Toggle_Reveal_Tip'),
            {
              id: 'byTip', action: 'if', branch: 'state',
              cond: { target: sel('Reveal_Tip'), visible: true },
              then: [{ id: 'tipShown', action: 'set', var: 'tipWas', value: 'shown' }, click('hideTip', 'Toggle_Reveal_Tip')],
              else: [{ id: 'tipHidden', action: 'set', var: 'tipWas', value: 'hidden' }],
            },
            { id: 'tipGone', action: 'assert', cond: { all: [{ expr: "$tipWas == 'shown'" }, { not: { target: sel('Reveal_Tip'), visible: true } }] } },
          ],
        },
        {
          id: 'claim', action: 'group', name: '领取：已达成就跳过',
          do: [
            {
              id: 'claimAll', action: 'loop', in: '$claims', as: 'what',
              note: '按钮字变成「已领取」就是领过了：第二轮全部跳过（状态记「已达成」，算通过）',
              do: [{ id: 'claimOne', action: 'click', target: claimSel('$what'), skipIf: { textLike: '已领取' } }],
            },
            statusHas('claimedAll', '共领了 3 个'),
            {
              id: 'claimAgain', action: 'loop', in: '$claims', as: 'what',
              do: [{ id: 'claimTwice', action: 'click', target: claimSel('$what'), skipIf: { textLike: '已领取' } }],
            },
            { id: 'claimCount', action: 'assert', cond: { all: [{ value: '@obj.screen.ClaimCount', equals: 3 }, { expr: "@obj.screen.IsClaimed('Mail')" }] } },
            {
              id: 'claimWithAlso', action: 'click', target: claimSel('Daily'), skipIf: { textLike: '已领取', also: 1 },
              note: '领过了就连下一步一起跳过（also）',
            },
            { id: 'neverRuns', action: 'fail', message: '应该随上一步一起跳过' },
          ],
        },
        {
          id: 'waiting', action: 'group', name: '等待：延时出现的宝箱', timeoutMs: 20000,
          do: [
            click('delayLoot', 'Delay_Reveal_Loot', '1.5 秒后才出现'),
            { id: 'notYet', action: 'assert', target: sel('Reveal_Loot'), visible: false },
            { id: 'waitLoot', action: 'wait', until: 'cond', cond: { target: sel('Reveal_Loot'), visible: true }, timeoutMs: 5000 },
            click('openLoot', 'Open_Popup_Reward__Loot'),
            { id: 'rewardShown', action: 'assert', cond: { all: [{ target: sel('Popup_Reward'), visible: true }, { target: sel('Popup_Reward_Subject'), textContains: '宝箱' }] } },
            click('takeReward', 'Close_Popup_Reward__Ok'),
            { id: 'rewardGone', action: 'wait', until: 'cond', cond: { not: { target: sel('Popup_Reward'), visible: true } } },
            { id: 'pause', action: 'wait', until: 'seconds', seconds: 0.3 },
            { id: 'pauseApi', action: 'api', api: 'Wait.Seconds', args: { n: 0.2 } },
          ],
        },
        {
          id: 'guard', action: 'group', name: '守护：定时弹出的公告',
          do: [
            click('armNotice', 'Arm_Popup_Notice', '1 秒后弹公告'),
            { id: 'letItPop', action: 'wait', until: 'seconds', seconds: 2, note: '等待中守护每 0.2 秒看一次：公告一出来就被关掉' },
            { id: 'noticeGone', action: 'assert', target: sel('Popup_Notice'), visible: false },
            statusHas('noticeHandled', '活动公告'),
          ],
        },
        {
          id: 'apis', action: 'group', name: '接口和表达式',
          do: [
            { id: 'markLog', action: 'api', api: 'Log.Mark', saveAs: 'logMark' },
            click('inc4', 'Inc_Counter'),
            { id: 'logged', action: 'api', api: 'Log.Contains', args: { text: '计数 = 4', since: '$logMark' }, expect: { equals: true }, note: '示例界面把状态栏的话也写进日志' },
            { id: 'shown', action: 'assert', cond: { api: 'Widget.IsShown', args: { target: 'WBP_ForgeSample' } } },
            { id: 'readText', action: 'api', api: 'Widget.Text', args: { target: '$counter' }, saveAs: 'counterText' },
            { id: 'textIsNum', action: 'assert', expr: 'num($counterText) == number($counter) and len($counterText) == 1' },
            { id: 'mapName', action: 'api', api: 'World.Map', saveAs: 'mapName' },
            { id: 'netMode', action: 'api', api: 'Net.Mode', expect: { equals: 'standalone' } },
            { id: 'fps', action: 'api', api: 'Perf.Fps', saveAs: 'fps' },
            { id: 'seed', action: 'api', api: 'Random.Seed', args: { n: 42 } },
            { id: 'cvar', action: 'api', api: 'CVar.Get', args: { name: 't.MaxFPS' }, saveAs: 'maxFps' },
            {
              id: 'exprs', action: 'assert',
              cond: {
                all: [
                  { expr: "contains($mapName, 'Map_ForgeSample')" },
                  { expr: '$fps > 0' },
                  { expr: "format('{0}-{1}', upper('ab'), len([1, 2, 3])) == 'AB-3'" },
                  { expr: "join(split('a,b,c', ','), '|') == 'a|b|c' and trim('  x ') == 'x'" },
                  { expr: 'clamp(round(3.14159, 2) * 10, 0, 30) == 30 and abs(-2) == 2 and max(1, 5, 3) == 5' },
                  { any: [{ expr: "'流程' in text($crumb)" }, { target: '$crumb', text: '首页' }] },
                  { not: { expr: "startsWith(text($status), 'xyz')" } },
                ],
              },
            },
          ],
        },
        {
          id: 'robust', action: 'group', name: '容错：块级重试、时限、继续、跳过、禁用',
          onFail: 'retry', retries: 2, timeoutMs: 30000,
          do: [
            { id: 'consoleCvar', action: 'console', command: 't.MaxFPS', expectContains: 'MaxFPS', onFail: 'continue', note: '控制台查变量；输出格式因引擎版本略有不同，失败只记一条不停下' },
            { id: 'skipDone', action: 'click', target: claimSel('Weekly'), skipIf: { target: claimSel('Weekly'), textLike: '已领取', settleMs: 200 } },
            { id: 'disabledStep', action: 'fail', message: '禁用的步骤不执行', disabled: true },
            { id: 'stillOk', action: 'assert', expr: 'number($counter) == 4' },
          ],
        },
        {
          id: 'reset', action: 'group', name: '重置：调界面上的无参函数',
          do: [
            { id: 'resetFlow', action: 'widget.call', target: ROOT, function: 'ResetFlow' },
            { id: 'cleared', action: 'assert', cond: { all: [{ target: '$counter', text: '0' }, { target: claimSel('Daily'), textLike: '领取' }, { target: sel('Reveal_Loot'), visible: false }] } },
            { id: 'stopEarly', action: 'return', cond: { expr: "$tipWas != 'shown'" }, note: '条件成立时提前结束（这里不成立，接着收尾）' },
          ],
        },
        closeGroup('forge-sample-flow'),
      ],
    },
  )

  // ---- 真实输入：真实鼠标和键盘 ----
  const real = scenario(
    'AssetForge 示例界面：真实鼠标和键盘',
    '配套「控件」页（AssetForge 1.5.0 起）。用真实鼠标（via: pointer）和按键：悬停、长按蓄力、拖动滑条、滚轮、点开下拉框选项、菜单、输入框里退格和回车、按键选择器。会真的移动光标，跑的时候别动鼠标；PIE 窗口要在前台。',
    ['pointer'],
    {
      vars: { ...base },
      steps: [
        open('Controls', '控件'),
        {
          id: 'pointer', action: 'group', name: '鼠标：点击、悬停、长按',
          do: [
            click('pointNormal', 'Btn_Normal', null, { via: 'pointer' }),
            statusHas('pointed', '普通按钮'),
            click('hover', 'Hover_Card', '真实鼠标移上去会触发悬停', { via: 'pointer' }),
            { id: 'hovered', action: 'assert', target: sel('Hover_Card_Value'), text: '悬停中' },
            { id: 'moveAway', action: 'input.cursor', x: 0.02, y: 0.98 },
            { id: 'unhovered', action: 'wait', until: 'cond', cond: { target: sel('Hover_Card_Value'), text: '未悬停' }, timeoutMs: 3000 },
            click('aimHold', 'Hold_Charge', '先点一下把光标放到按钮上（这次是短按）', { via: 'pointer' }),
            { id: 'holdMouse', action: 'key', key: 'LeftMouseButton', holdMs: 1300, note: '在光标处按住 1.3 秒再松开' },
            { id: 'charged', action: 'assert', target: sel('Hold_Charge_Value'), text: '蓄力完成' },
          ],
        },
        {
          id: 'dragWheel', action: 'group', name: '拖动和滚轮',
          do: [
            { id: 'dragBright', action: 'drag', target: sel('Brightness'), at: [0.5, 0.5], toTarget: sel('Brightness'), toAt: [0.95, 0.5], note: '从滑块（值 50 在正中）拖到右端：拖动会广播' },
            { id: 'brightHigh', action: 'assert', target: sel('Brightness_Value'), number: { gte: 80 } },
            { id: 'wheelLogs', action: 'wheel', target: sel('LogList'), delta: -3 },
            { id: 'scrolled', action: 'snapshot', target: sel('LogList'), note: '快照里 state.offset 是滚过的距离' },
            click('pickLog8', 'Pick_LogPicked__8', null, { via: 'pointer' }),
            { id: 'log8', action: 'assert', target: sel('LogPicked'), textContains: '日志 08' },
          ],
        },
        {
          id: 'popups', action: 'group', name: '下拉框和菜单（弹出窗口里的控件）',
          do: [
            click('openCombo', 'Server', '点开下拉框', { via: 'pointer' }),
            { id: 'pickOption', action: 'click', target: sel('Server') + '>>STableRow[text=华东一区]', via: 'pointer', note: '下拉列表在单独的弹出窗口里：用 >> 进 Slate 层按文字找行' },
            { id: 'comboEcho', action: 'assert', target: sel('Server_Value'), text: '华东一区' },
            click('openMenu', 'Menu_ActionAnchor', null, { via: 'pointer' }),
            { id: 'pickCopy', action: 'click', target: menuItem('MenuCopy'), via: 'pointer' },
            statusHas('copied', '菜单：复制'),
          ],
        },
        {
          id: 'keys', action: 'group', name: '键盘：退格、回车、按键选择器',
          do: [
            { id: 'typeAbc', action: 'type', target: sel('SearchBox'), text: 'abcd' },
            { id: 'backspace', action: 'key', key: 'BackSpace', target: sel('SearchBox') },
            { id: 'enter', action: 'key', key: 'Enter', target: sel('SearchBox') },
            statusHas('committed', '输入：abc'),
            click('listenKey', 'JumpKey', '点一下按键选择器，进入等按键状态', { via: 'pointer' }),
            { id: 'pressJ', action: 'key', key: 'J' },
            { id: 'jumpIsJ', action: 'assert', target: sel('JumpKey_Value'), text: 'J' },
          ],
        },
        closeGroup('forge-sample-real-input'),
      ],
    },
  )

  return {
    'common/打开示例界面.json': common,
    'ForgeSample-冒烟.json': smoke,
    'ForgeSample-条目标识.json': itemKeys,
    'ForgeSample-控件操作.json': controls,
    'ForgeSample-流程脚本.json': flow,
    'ForgeSample-真实输入.json': real,
  }
}
