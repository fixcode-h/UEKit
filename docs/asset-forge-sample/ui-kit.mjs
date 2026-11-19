// 示例界面生成脚本的积木：配色、控件节点、按钮、布局。gen.mjs 和各页面文件共用。
// 控件名在**整个界面**（父界面 + 所有子页面）里不能重名：示例界面的 C++ 父类按名字在所有子界面里找控件。

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

export { lin, r4, color, slate, flat, rounded, margin, T, names, node, font, text, box, fill, spacer, sized, card, BG, button, navButton, hbox, vbox, gap, vgap }
