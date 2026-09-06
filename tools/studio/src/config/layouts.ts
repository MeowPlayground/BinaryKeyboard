/**
 * 键盘布局配置系统
 * 支持灵活定义任意键盘布局
 */

/** 按键尺寸类型 */
export type KeySize = '1u' | '2u-h' | '2u-v';

/** 单个按键定义 */
export interface KeyDef {
  /** 按键索引 (对应固件中的按键编号) */
  index: number;
  /** 行位置 (从 0 开始) */
  row: number;
  /** 列位置 (从 0 开始) */
  col: number;
  /** 按键尺寸 */
  size: KeySize;
  /** 自定义标签 (用于旋钮等特殊按键) */
  label?: string;
  /** 按键类型 (用于特殊样式) */
  type?: 'normal' | 'encoder-cw' | 'encoder-ccw' | 'encoder-press';
}

/** 键盘布局定义 */
export interface LayoutDef {
  /** 布局名称 */
  name: string;
  /** 网格列数 (以 1u 为单位) */
  cols: number;
  /** 网格行数 (以 1u 为单位) */
  rows: number;
  /** 按键定义列表 */
  keys: KeyDef[];
  /** 是否有旋钮编码器 */
  hasEncoder?: boolean;
}

/**
 * 基础款布局 (4 键)
 * · · |
 * · —
 * 说明: 2x2 小键 + 1个竖向2u + 1个横向2u
 */
export const LAYOUT_BASIC: LayoutDef = {
  name: '基础款',
  cols: 3,
  rows: 2,
  keys: [
    { index: 0, row: 0, col: 0, size: '1u' },
    { index: 1, row: 0, col: 1, size: '1u' },
    { index: 2, row: 0, col: 2, size: '2u-v' }, // 竖向 2u
    { index: 3, row: 1, col: 0, size: '2u-h' }, // 横向 2u
  ],
};

/**
 * 五键款布局 (5 键)
 * · · |
 * · ·
 * 说明: 2x2 小键 + 1个竖向2u
 */
export const LAYOUT_FIVE_KEYS: LayoutDef = {
  name: '五键款',
  cols: 3,
  rows: 2,
  keys: [
    { index: 0, row: 0, col: 0, size: '1u' },
    { index: 1, row: 0, col: 1, size: '1u' },
    { index: 2, row: 0, col: 2, size: '2u-v' }, // 竖向 2u
    { index: 3, row: 1, col: 0, size: '1u' },
    { index: 4, row: 1, col: 1, size: '1u' },
  ],
};

/**
 * 旋钮款布局 (4 键 + 旋钮)
 * + · |
 * · ·
 * 说明: + 代表旋钮（被分成3份：左转、按下、右转）
 * 旋钮的3个操作都在同一个位置（row 0, col 0），由 EncoderButton 组件合并显示
 * 实体键顺序按实际物理位置：0=P34, 1=P14, 2=P33, 3=P31
 * 虚拟按键与 CH592F 固件协议一致: 0-3=物理键, 4=顺时针, 5=逆时针, 6=按下
 */
export const LAYOUT_KNOB: LayoutDef = {
  name: '旋钮款',
  cols: 3,
  rows: 2,
  hasEncoder: true,
  keys: [
    { index: 6, row: 0, col: 0, size: '1u', label: '●', type: 'encoder-press' }, // 旋钮按下（+）
    { index: 4, row: 0, col: 0, size: '1u', label: '↻', type: 'encoder-cw' }, // 旋钮右转（与按下同一位置）
    { index: 5, row: 0, col: 0, size: '1u', label: '↺', type: 'encoder-ccw' }, // 旋钮左转（与按下同一位置）
    { index: 0, row: 0, col: 1, size: '1u' }, // 按键0（·）
    { index: 1, row: 0, col: 2, size: '2u-v' }, // 按键1（|，竖向2u）
    { index: 2, row: 1, col: 0, size: '1u' }, // 按键2（·）
    { index: 3, row: 1, col: 1, size: '1u' }, // 按键3（·）
  ],
};

/**
 * 旋钮款层选择布局（仅物理按键，旋钮不参与层选择）
 *   · |
 * · ·
 */
export const LAYOUT_KNOB_LAYER: LayoutDef = {
  name: '旋钮款（层选择）',
  cols: 3,
  rows: 2,
  hasEncoder: false,
  keys: [
    { index: -1, row: 0, col: 0, size: '1u', label: '🎛️', type: 'encoder-press' }, // 旋钮占位（不可点击）
    { index: 0, row: 0, col: 1, size: '1u' },
    { index: 1, row: 0, col: 2, size: '2u-v' },
    { index: 2, row: 1, col: 0, size: '1u' },
    { index: 3, row: 1, col: 1, size: '1u' },
  ],
};

/** 根据键盘类型获取布局 */
export function getLayoutByType(type: number): LayoutDef {
  switch (type) {
    case 0: return LAYOUT_BASIC;
    case 1: return LAYOUT_FIVE_KEYS;
    case 2: return LAYOUT_KNOB;
    default: return LAYOUT_BASIC;
  }
}

/** 根据键盘类型获取层选择布局（用于层选择面板） */
export function getLayerLayoutByType(type: number): LayoutDef {
  switch (type) {
    case 0: return LAYOUT_BASIC; // 基础款：4个按键对应4层
    case 1: return LAYOUT_FIVE_KEYS; // 五键款：5个按键对应5层
    case 2: return LAYOUT_KNOB_LAYER; // 旋钮款：4个实体按键对应4层
    default: return LAYOUT_BASIC;
  }
}

/** 计算按键样式 */
export function getKeyStyle(key: KeyDef, unitSize: number = 72, gap: number = 8): Record<string, string> {
  const style: Record<string, string> = {};
  
  // 位置
  style.gridRow = `${key.row + 1} / span ${key.size === '2u-v' ? 2 : 1}`;
  style.gridColumn = `${key.col + 1} / span ${key.size === '2u-h' ? 2 : 1}`;
  
  return style;
}

/** 计算按键尺寸类名 */
export function getKeySizeClass(key: KeyDef): string {
  if (key.type?.startsWith('encoder')) {
    return `key-${key.type}`;
  }
  return `key-${key.size}`;
}
