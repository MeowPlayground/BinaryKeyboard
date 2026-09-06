/**
 * BinaryKeyboard 无线版 HID 通讯协议定义
 * 与固件 kbd_types.h 对应
 */

// ============================================================================
// 设备信息常量
// ============================================================================

export const CH592_VENDOR_ID = 0x413d;
export const CH592_PRODUCT_ID = 0x2107;
export const CH552_VENDOR_ID = 0x1209;
export const CH552_PRODUCT_ID = 0xc55d;

// 向后兼容：默认常量仍指向 CH592F
export const VENDOR_ID = CH592_VENDOR_ID;
export const PRODUCT_ID = CH592_PRODUCT_ID;

// 无 Report ID 模式 - 通过独立 HID 接口区分（兼容 Linux/macOS）
export const REPORT_ID_COMMAND = 0; // 主机 → 键盘 (Output Report, 无 Report ID)
export const REPORT_ID_RESPONSE = 0; // 键盘 → 主机 (Input Report, 无 Report ID)

export const FRAME_SIZE = 64; // 帧大小

export const CH552_REPORT_ID_COMMAND = 0x04;
export const CH552_REPORT_ID_RESPONSE = 0x05;
export const CH552_FRAME_SIZE = 31;

// ============================================================================
// 键盘类型
// ============================================================================

export enum KeyboardType {
  BASIC = 0, // 基础款: 4 键
  FIVE_KEYS = 1, // 五键款: 5 键
  KNOB = 2, // 旋钮款: 4 键 + 旋钮 (7 虚拟键)
}

export const KeyboardTypeInfo: Record<
  KeyboardType,
  { name: string; keys: number; physical: number; layers: number }
> = {
  [KeyboardType.BASIC]: { name: "基础款", keys: 4, physical: 4, layers: 4 },
  [KeyboardType.FIVE_KEYS]: { name: "五键款", keys: 5, physical: 5, layers: 5 },
  [KeyboardType.KNOB]: { name: "旋钮款", keys: 7, physical: 4, layers: 4 },
};

export enum DeviceProtocol {
  CH592 = "ch592",
  CH552 = "ch552",
}

export interface DeviceCapabilities {
  multiLayer: boolean;
  layerKeyActions: boolean;
  rgb: boolean;
  rgbOverlay: boolean;
  fnKeys: boolean;
  osMode: boolean;
  macroActions: boolean;
  wheelClickAction: boolean;
  battery: boolean;
  logs: boolean;
  reset: boolean;
  explicitSave: boolean;
  wireless: boolean;
  iap: boolean;
}

export function createDeviceCapabilities(
  overrides: Partial<DeviceCapabilities> = {},
): DeviceCapabilities {
  return {
    multiLayer: true,
    layerKeyActions: true,
    rgb: true,
    rgbOverlay: false,
    fnKeys: true,
    osMode: true,
    macroActions: true,
    wheelClickAction: true,
    battery: true,
    logs: true,
    reset: true,
    explicitSave: true,
    wireless: true,
    iap: true,
    ...overrides,
  };
}

export const CH592_CAPABILITIES = createDeviceCapabilities();
export const CH552_CAPABILITIES = createDeviceCapabilities({
  multiLayer: true,
  layerKeyActions: false,
  rgb: true,
  rgbOverlay: true,
  fnKeys: false,
  osMode: false,
  macroActions: true,
  wheelClickAction: false,
  battery: false,
  logs: false,
  reset: true,
  explicitSave: false,
  wireless: false,
  iap: false,
});

// ============================================================================
// HID 命令码
// ============================================================================

export enum Command {
  // 系统命令 0x01-0x0F
  SYS_INFO = 0x01,
  SYS_STATUS = 0x02,

  // 配置管理 0x10-0x1F
  CFG_SAVE = 0x10,
  CFG_LOAD = 0x11,
  CFG_RESET = 0x12,
  CFG_OS_GET = 0x13,
  CFG_OS_SET = 0x14,

  // 按键映射 0x20-0x2F
  KEYMAP_GET = 0x20,
  KEYMAP_SET = 0x21,
  LAYER_GET = 0x22,
  LAYER_SET = 0x23,

  // RGB 控制 0x30-0x3F
  RGB_GET = 0x30,
  RGB_SET = 0x31,

  // 宏管理 0x40-0x4F
  MACRO_INFO = 0x40,
  MACRO_GET = 0x41,
  MACRO_SET = 0x42,
  MACRO_DEL = 0x43,

  // FN 键配置 0x50-0x5F
  FNKEY_GET = 0x50,
  FNKEY_SET = 0x51,

  // 电源管理 0x60-0x6F
  BATTERY = 0x60,

  // 设备日志 0x70-0x7F
  LOG = 0x70,
  LOG_GET = 0x71,
  LOG_SET = 0x72,

  // IAP 固件更新 0x80-0x8F
  IAP_INFO = 0x80,
  IAP_PREPARE = 0x81,
  IAP_WRITE = 0x82,
  IAP_VERIFY = 0x83,
  IAP_ACTIVATE = 0x84,

  // DataFlash 调试 0x90-0x9F
  DATAFLASH_INFO = 0x90,
  DATAFLASH_READ = 0x91,
  DATAFLASH_WRITE = 0x92,
}

// ============================================================================
// 固件日志类别
// ============================================================================

export enum LogCategory {
  KEY_EVENT = 0x01,
  FN_EVENT = 0x02,
  LAYER_EVENT = 0x03,
  MODE_EVENT = 0x04,
  BLE_EVENT = 0x05,
  RGB_EVENT = 0x06,
  SYSTEM_EVENT = 0x07,
}

export enum SystemLogEvent {
  BOOT = 0x01,
  SLEEP = 0x02,
  WAKEUP = 0x03,
}

// ============================================================================
// 日志配置
// ============================================================================

/** 日志类别掩码常量 */
export const LOG_MASK = {
  KEY: 0x01,
  FN: 0x02,
  LAYER: 0x04,
  MODE: 0x08,
  BLE: 0x10,
  RGB: 0x20,
  SYS: 0x40,
  ALL: 0x7f,
} as const;

/** 日志类别掩码标签 (用于 UI) */
export const LOG_MASK_LABELS: { mask: number; label: string; key: string }[] = [
  { mask: LOG_MASK.KEY, label: "按键", key: "key" },
  { mask: LOG_MASK.FN, label: "FN", key: "fn" },
  { mask: LOG_MASK.LAYER, label: "层切换", key: "layer" },
  { mask: LOG_MASK.MODE, label: "模式", key: "mode" },
  { mask: LOG_MASK.BLE, label: "蓝牙", key: "ble" },
  { mask: LOG_MASK.RGB, label: "RGB", key: "rgb" },
  { mask: LOG_MASK.SYS, label: "系统", key: "sys" },
];

/** 固件端日志配置 (仅开关，类别过滤在 UI 端) */
export interface LogConfig {
  enabled: boolean;
}

export function createDefaultLogConfig(): LogConfig {
  return { enabled: true };
}

// ============================================================================
// 响应状态码
// ============================================================================

export enum ResponseCode {
  OK = 0x00,
  ERR_INVALID = 0x01,
  ERR_PARAM = 0x02,
  ERR_BUSY = 0x03,
  ERR_FLASH = 0x04,
  ERR_TOO_LARGE = 0x10,
  ERR_NO_SPACE = 0x11,
  ERR_NOT_FOUND = 0x12,
}

// ============================================================================
// 按键动作类型
// ============================================================================

export enum ActionType {
  NONE = 0x00,
  KEYBOARD = 0x01,
  MOUSE_BTN = 0x02,
  MOUSE_WHEEL = 0x03,
  CONSUMER = 0x04,
  MACRO = 0x05,
  LAYER = 0x06,
}

// ============================================================================
// 修饰键掩码
// ============================================================================

export enum Modifier {
  LCTRL = 0x01,
  LSHIFT = 0x02,
  LALT = 0x04,
  LGUI = 0x08,
  RCTRL = 0x10,
  RSHIFT = 0x20,
  RALT = 0x40,
  RGUI = 0x80,
}

export enum OsMode {
  WIN = 0,
  MAC = 1,
}

export interface OsModeConfig {
  mode: OsMode;
}

// ============================================================================
// 鼠标按键掩码
// ============================================================================

export enum MouseButton {
  LEFT = 0x01,
  RIGHT = 0x02,
  MIDDLE = 0x04,
  BACK = 0x08,
  FORWARD = 0x10,
}

// ============================================================================
// 滚轮方向
// ============================================================================

export enum WheelDirection {
  NONE = 0,
  UP = 1,
  DOWN = 2,
  CLICK = 3,
}

// ============================================================================
// 层操作类型
// ============================================================================

export enum LayerOp {
  MOMENTARY = 0, // 按住切换，松开恢复
  TOGGLE = 1, // 切换开关
  SET = 2, // 直接设置
}

// ============================================================================
// FN 动作类型
// ============================================================================

export enum FnAction {
  NONE = 0x00,
  MODE_TOGGLE = 0x01,
  BLE_ADV = 0x02,
  BLE_DISCONNECT = 0x03,
  BLE_CLEAR_BONDS = 0x04,
  RGB_TOGGLE = 0x10,
  RGB_MODE_NEXT = 0x11,
  RGB_MODE_PREV = 0x12,
  RGB_BRIGHT_UP = 0x13,
  RGB_BRIGHT_DOWN = 0x14,
  LAYER_NEXT = 0x20,
  LAYER_PREV = 0x21,
  LAYER_SET = 0x22,
  MACRO = 0x30,
  SLEEP = 0x40,
  BOOTLOADER = 0x41,
}

// ============================================================================
// RGB 模式
// ============================================================================

export enum RgbMode {
  OFF = 0,
  STATIC = 1,
  BREATHING = 2,
  BLINK = 3,
  RAINBOW = 4,
  INDICATOR = 5,
}

export enum PressEffect {
  NONE = 0,
  PRESS_LIGHT_FADE = 1,
  PRESS_DARK_FADE = 2,
}

// ============================================================================
// 数据结构
// ============================================================================

/** 单键动作 (4 字节) */
export interface KeyAction {
  type: ActionType;
  modifier: number; // 修饰键或操作类型
  param1: number; // 键码/按键/方向/ID
  param2: number; // 多媒体键高字节
}

/** 单层映射 (32 字节 = 8 键 × 4 字节) */
export interface LayerConfig {
  keys: KeyAction[]; // 8 个按键
}

/** 完整按键映射配置 */
export interface KeymapConfig {
  numLayers: number;
  currentLayer: number;
  defaultLayer: number;
  layers: LayerConfig[]; // 最多 4 层
}

/** FN 键配置 (8 字节) */
export interface FnKeyEntry {
  clickAction: FnAction;
  clickParam: number;
  longAction: FnAction;
  longParam: number;
  longPressMs: number;
}

/** FN 键配置集合 */
export interface FnKeyConfig {
  fnKeys: FnKeyEntry[]; // 最多 4 个 FN 键
}

/** 默认亮度 20% (255 * 20 / 100 ≈ 51) */
export const RGB_DEFAULT_BRIGHTNESS = 51;

/** 指示灯最低亮度 (5%)，不可完全关闭，与固件 KBD_INDICATOR_MIN_BRIGHTNESS 一致 */
export const RGB_INDICATOR_MIN_BRIGHTNESS = 13;

/** RGB(0-255) 转 hex 字符串，如 "#ffffff" */
export function rgbToHex(r: number, g: number, b: number): string {
  const pad = (n: number) => n.toString(16).padStart(2, "0");
  return "#" + pad(r) + pad(g) + pad(b);
}

/** hex 字符串解析为 RGB，返回 {r,g,b} 或 null。支持 "#ffffff" 或 "ffffff" */
export function hexToRgb(
  hex: string | unknown,
): { r: number; g: number; b: number } | null {
  const s = typeof hex === "string" ? hex.replace(/^#/, "") : "";
  const m = s.match(/^([0-9a-fA-F]{2})([0-9a-fA-F]{2})([0-9a-fA-F]{2})$/);
  if (!m) return null;
  return {
    r: parseInt(m[1], 16),
    g: parseInt(m[2], 16),
    b: parseInt(m[3], 16),
  };
}

/** RGB 配置 */
export interface RgbConfig {
  enabled: boolean;
  mode: RgbMode;
  brightness: number; /**< RGB/按键灯亮度 (0-255) */
  speed: number;
  colorR: number;
  colorG: number;
  colorB: number;
  indicatorEnabled: boolean;
  indicatorBrightness: number; /**< 指示灯亮度 (0-255) */
  pressEffect: PressEffect;
  lightSleepMin?: number; /**< LIGHT 休眠时间（分钟，0=禁用） */
  deepSleepMin?: number; /**< DEEP 延时（在 LIGHT 后，分钟，0=禁用） */
  seamlessWakeEnabled?: boolean; /**< 唤醒首键透传 */
  pollRate?: number; /**< USB HID 轮询率 bInterval (1=1000Hz, 2=500Hz, 5=200Hz, 10=100Hz) */
}

/** 设备信息 (SYS_INFO 响应) */
export interface DeviceInfo {
  vendorId: number;
  productId: number;
  chipFamily: string;
  versionMajor: number;
  versionMinor: number;
  versionPatch: number;
  maxLayers: number;
  maxKeys: number;
  macroSlots: number;
  keyboardType: KeyboardType;
  actualKeyCount: number;
  fnKeyCount: number;
  protocol: DeviceProtocol;
  protocolLabel: string;
  capabilities: DeviceCapabilities;
}

/** 系统状态 (SYS_STATUS 响应) */
export interface DeviceStatus {
  workMode: number; // 0=USB, 1=BLE
  connectionState: number; // 0=断开, 1=广播中, 2=已连接, 3=挂起
  currentLayer: number;
  batteryLevel: number;
  isCharging: boolean;
  adcRaw?: number;
  chargePinRaw?: number;
}

/** HID 命令帧 */
export interface CommandFrame {
  cmd: Command;
  sub: number;
  len: number;
  data: Uint8Array;
}

// ============================================================================
// 常量
// ============================================================================

export const MAX_LAYERS = 5;
export const MAX_KEYS = 8;
export const MAX_FN_KEYS = 4;
export const MACRO_SLOTS = 8;
export const MACRO_MAX_ACTIONS = 1000;
export const MACRO_MAX_DATA_SIZE = 2024; // 2048 - 24 header
export const MACRO_NAME_MAX_BYTES = 16;

/** MeowFS: CH552G 动态宏存储 (1KB Flash) */
export const CH552_MEOWFS_SIZE = 1024;
export const CH552_MEOWFS_PAGE_SIZE = 64;
export const CH552_MEOWFS_HEADER_SIZE = 2;
export const CH552_MEOWFS_ACTION_SIZE = 2;
export const CH552_MEOWFS_MAX_ACTIONS = 255;
export const CH552_MEOWFS_APPEND_SLOTS = 1;

/** 宏触发模式 */
export enum MacroTrigger {
  ONCE = 0x00,         /** 按下触发 1 次 */
  HOLD_ABORT = 0x01,   /** 按住循环，松开立即中断 */
  HOLD_FINISH = 0x02,  /** 按住循环，松开跑完当轮 */
  TOGGLE = 0x03,       /** 按下切换循环 */
}

/** 宏动作类型 */
export enum MacroActionType {
  KEY_DOWN = 0x01,
  KEY_UP = 0x02,
  MOD_DOWN = 0x03,
  MOD_UP = 0x04,
  DELAY = 0x10,
  CONSUMER = 0x20,
  MOUSE_DOWN = 0x30,
  MOUSE_UP = 0x31,
  WHEEL = 0x32,
  END = 0xff,
}

/** 宏动作单元 (2 bytes) */
export interface MacroAction {
  type: MacroActionType;
  param: number;
}

/** 宏头部信息 (24 bytes) */
export interface MacroHeader {
  valid: number;
  id: number;
  actionCount: number;
  dataSize: number;
  name: string;
}

/** 完整宏数据 */
export interface MacroData {
  header: MacroHeader;
  actions: MacroAction[];
}

/** 宏槽位概览 */
export interface MacroOverview {
  /** UI 当前可展示的槽位数；动态文件系统设备可包含一个“新建”入口 */
  totalSlots: number;
  /** 当前有效宏数量 */
  usedCount: number;
  /** 与 totalSlots 对齐的有效位图 */
  slotValid: boolean[];
  /** 是否为动态宏列表（MeowFS） */
  dynamic?: boolean;
  /** 设备硬上限（若已知） */
  maxSlots?: number;
  /** MeowFS: 总容量 (字节) */
  fsTotal?: number;
  /** MeowFS: 剩余空间 (字节) */
  fsFree?: number;
}

// ============================================================================
// 辅助函数
// ============================================================================

/** 创建空动作 */
export function createEmptyAction(): KeyAction {
  return { type: ActionType.NONE, modifier: 0, param1: 0, param2: 0 };
}

/** 创建键盘动作 */
export function createKeyboardAction(keycode: number, modifier = 0): KeyAction {
  return { type: ActionType.KEYBOARD, modifier, param1: keycode, param2: 0 };
}

/** 创建鼠标按键动作 */
export function createMouseButtonAction(button: MouseButton): KeyAction {
  return { type: ActionType.MOUSE_BTN, modifier: 0, param1: button, param2: 0 };
}

/** 创建滚轮动作 */
export function createWheelAction(direction: WheelDirection): KeyAction {
  return {
    type: ActionType.MOUSE_WHEEL,
    modifier: 0,
    param1: direction,
    param2: 0,
  };
}

/** 创建多媒体键动作 */
export function createConsumerAction(usageId: number): KeyAction {
  return {
    type: ActionType.CONSUMER,
    modifier: 0,
    param1: usageId & 0xff,
    param2: (usageId >> 8) & 0xff,
  };
}

/** 创建层切换动作 */
export function createLayerAction(op: LayerOp, layerId: number): KeyAction {
  return { type: ActionType.LAYER, modifier: op, param1: layerId, param2: 0 };
}

/** 创建宏动作 */
export function createMacroAction(macroId: number, trigger: MacroTrigger = MacroTrigger.ONCE): KeyAction {
  return { type: ActionType.MACRO, modifier: trigger, param1: macroId, param2: 0 };
}

/** 创建空层 */
export function createEmptyLayer(): LayerConfig {
  return { keys: Array.from({ length: MAX_KEYS }, () => createEmptyAction()) };
}

/** 创建空映射配置 */
export function createEmptyKeymap(): KeymapConfig {
  return {
    numLayers: 1,
    currentLayer: 0,
    defaultLayer: 0,
    layers: Array.from({ length: MAX_LAYERS }, () => createEmptyLayer()),
  };
}

/** 创建空 FN 键配置 */
export function createEmptyFnKey(): FnKeyEntry {
  return {
    clickAction: FnAction.NONE,
    clickParam: 0,
    longAction: FnAction.NONE,
    longParam: 0,
    longPressMs: 800,
  };
}

/** 创建空 FN 键配置集合 */
export function createEmptyFnKeyConfig(): FnKeyConfig {
  return {
    fnKeys: Array.from({ length: MAX_FN_KEYS }, () => createEmptyFnKey()),
  };
}

/** 创建默认 RGB 配置 (默认 20% 亮度) */
export function createDefaultRgbConfig(): RgbConfig {
  return {
    enabled: true,
    mode: RgbMode.RAINBOW,
    brightness: RGB_DEFAULT_BRIGHTNESS,
    speed: 128,
    colorR: 255,
    colorG: 255,
    colorB: 255,
    indicatorEnabled: true,
    indicatorBrightness: RGB_DEFAULT_BRIGHTNESS,
    pressEffect: PressEffect.NONE,
    lightSleepMin: 1,
    deepSleepMin: 1,
    seamlessWakeEnabled: true,
    pollRate: 10,
  };
}
