/**
 * HID 协议帧解析与翻译
 * 将原始字节解码为人类可读格式
 */

import {
  Command,
  ResponseCode,
  ActionType,
  RgbMode,
  FnAction,
  KeyboardType,
  LayerOp,
  MouseButton,
  OsMode,
  PressEffect,
  WheelDirection,
  LogCategory,
  SystemLogEvent,
} from "@/types/protocol";
import { getKeycodeName } from "@/utils/keycodes";

// ============================================================================
// 命令名称映射
// ============================================================================

const COMMAND_NAMES: Record<number, string> = {
  [Command.SYS_INFO]: "SYS_INFO",
  [Command.SYS_STATUS]: "SYS_STATUS",
  [Command.CFG_SAVE]: "CFG_SAVE",
  [Command.CFG_LOAD]: "CFG_LOAD",
  [Command.CFG_RESET]: "CFG_RESET",
  [Command.CFG_OS_GET]: "CFG_OS_GET",
  [Command.CFG_OS_SET]: "CFG_OS_SET",
  [Command.KEYMAP_GET]: "KEYMAP_GET",
  [Command.KEYMAP_SET]: "KEYMAP_SET",
  [Command.LAYER_GET]: "LAYER_GET",
  [Command.LAYER_SET]: "LAYER_SET",
  [Command.RGB_GET]: "RGB_GET",
  [Command.RGB_SET]: "RGB_SET",
  [Command.MACRO_INFO]: "MACRO_INFO",
  [Command.MACRO_GET]: "MACRO_GET",
  [Command.MACRO_SET]: "MACRO_SET",
  [Command.MACRO_DEL]: "MACRO_DEL",
  [Command.FNKEY_GET]: "FNKEY_GET",
  [Command.FNKEY_SET]: "FNKEY_SET",
  [Command.BATTERY]: "BATTERY",
  [Command.LOG]: "LOG",
  [Command.LOG_GET]: "LOG_GET",
  [Command.LOG_SET]: "LOG_SET",
  [Command.DATAFLASH_INFO]: "DATAFLASH_INFO",
  [Command.DATAFLASH_READ]: "DATAFLASH_READ",
  [Command.DATAFLASH_WRITE]: "DATAFLASH_WRITE",
};

const COMMAND_LABELS: Record<number, string> = {
  [Command.SYS_INFO]: "获取设备信息",
  [Command.SYS_STATUS]: "获取系统状态",
  [Command.CFG_SAVE]: "保存配置到 Flash",
  [Command.CFG_LOAD]: "从 Flash 加载配置",
  [Command.CFG_RESET]: "恢复出厂设置",
  [Command.CFG_OS_GET]: "获取系统模式",
  [Command.CFG_OS_SET]: "设置系统模式",
  [Command.KEYMAP_GET]: "获取按键映射",
  [Command.KEYMAP_SET]: "设置按键映射",
  [Command.LAYER_GET]: "获取当前层",
  [Command.LAYER_SET]: "设置当前层",
  [Command.RGB_GET]: "获取 RGB 配置",
  [Command.RGB_SET]: "设置 RGB 配置",
  [Command.MACRO_INFO]: "获取宏信息",
  [Command.MACRO_GET]: "读取宏数据",
  [Command.MACRO_SET]: "写入宏数据",
  [Command.MACRO_DEL]: "删除宏",
  [Command.FNKEY_GET]: "获取 FN 键配置",
  [Command.FNKEY_SET]: "设置 FN 键配置",
  [Command.BATTERY]: "获取电池信息",
  [Command.LOG]: "设备日志",
  [Command.LOG_GET]: "获取日志配置",
  [Command.LOG_SET]: "设置日志配置",
  [Command.DATAFLASH_INFO]: "获取 DataFlash 布局",
  [Command.DATAFLASH_READ]: "读取 DataFlash",
  [Command.DATAFLASH_WRITE]: "写入 DataFlash 字节",
};

const RESPONSE_CODE_NAMES: Record<number, string> = {
  [ResponseCode.OK]: "OK",
  [ResponseCode.ERR_INVALID]: "ERR_INVALID",
  [ResponseCode.ERR_PARAM]: "ERR_PARAM",
  [ResponseCode.ERR_BUSY]: "ERR_BUSY",
  [ResponseCode.ERR_FLASH]: "ERR_FLASH",
  [ResponseCode.ERR_TOO_LARGE]: "ERR_TOO_LARGE",
  [ResponseCode.ERR_NO_SPACE]: "ERR_NO_SPACE",
  [ResponseCode.ERR_NOT_FOUND]: "ERR_NOT_FOUND",
};

const RESPONSE_CODE_LABELS: Record<number, string> = {
  [ResponseCode.OK]: "成功",
  [ResponseCode.ERR_INVALID]: "无效命令",
  [ResponseCode.ERR_PARAM]: "参数错误",
  [ResponseCode.ERR_BUSY]: "设备忙",
  [ResponseCode.ERR_FLASH]: "Flash 操作失败",
  [ResponseCode.ERR_TOO_LARGE]: "数据过大",
  [ResponseCode.ERR_NO_SPACE]: "存储空间不足",
  [ResponseCode.ERR_NOT_FOUND]: "未找到目标",
};

const ACTION_TYPE_NAMES: Record<number, string> = {
  [ActionType.NONE]: "无",
  [ActionType.KEYBOARD]: "键盘",
  [ActionType.MOUSE_BTN]: "鼠标按键",
  [ActionType.MOUSE_WHEEL]: "鼠标滚轮",
  [ActionType.CONSUMER]: "多媒体",
  [ActionType.MACRO]: "宏",
  [ActionType.LAYER]: "层切换",
};

const RGB_MODE_NAMES: Record<number, string> = {
  [RgbMode.OFF]: "关闭",
  [RgbMode.STATIC]: "静态",
  [RgbMode.BREATHING]: "呼吸",
  [RgbMode.BLINK]: "闪烁",
  [RgbMode.RAINBOW]: "彩虹",
  [RgbMode.INDICATOR]: "仅指示灯",
};

const PRESS_EFFECT_NAMES: Record<number, string> = {
  [PressEffect.NONE]: "无",
  [PressEffect.PRESS_LIGHT_FADE]: "按下亮起",
  [PressEffect.PRESS_DARK_FADE]: "按下熄灭",
};

const KEYBOARD_TYPE_NAMES: Record<number, string> = {
  [KeyboardType.BASIC]: "基础款",
  [KeyboardType.FIVE_KEYS]: "五键款",
  [KeyboardType.KNOB]: "旋钮款",
};

const FN_ACTION_NAMES: Record<number, string> = {
  [FnAction.NONE]: "无",
  [FnAction.MODE_TOGGLE]: "切换模式",
  [FnAction.BLE_ADV]: "蓝牙广播",
  [FnAction.BLE_DISCONNECT]: "蓝牙断开",
  [FnAction.BLE_CLEAR_BONDS]: "清除配对",
  [FnAction.RGB_TOGGLE]: "RGB 开关",
  [FnAction.RGB_MODE_NEXT]: "RGB 下一模式",
  [FnAction.RGB_MODE_PREV]: "RGB 上一模式",
  [FnAction.RGB_BRIGHT_UP]: "亮度+",
  [FnAction.RGB_BRIGHT_DOWN]: "亮度-",
  [FnAction.LAYER_NEXT]: "下一层",
  [FnAction.LAYER_PREV]: "上一层",
  [FnAction.LAYER_SET]: "设置层",
  [FnAction.MACRO]: "执行宏",
  [FnAction.SLEEP]: "休眠",
  [FnAction.BOOTLOADER]: "进入 Bootloader",
};

const LAYER_OP_NAMES: Record<number, string> = {
  [LayerOp.MOMENTARY]: "按住",
  [LayerOp.TOGGLE]: "切换",
  [LayerOp.SET]: "设置",
};

// ============================================================================
// 工具函数
// ============================================================================

/** Uint8Array 转 hex 字符串 */
export function toHexDump(data: Uint8Array, maxBytes = 64): string {
  const bytes = Array.from(data.slice(0, maxBytes));
  return bytes
    .map((b) => b.toString(16).padStart(2, "0").toUpperCase())
    .join(" ");
}

/** 单字节转 hex */
function hex(n: number): string {
  return "0x" + n.toString(16).padStart(2, "0").toUpperCase();
}

/** 格式化按键动作 */
function formatKeyAction(
  type: number,
  mod: number,
  param1: number,
  param2: number,
): string {
  const typeName = ACTION_TYPE_NAMES[type] || `未知(${hex(type)})`;

  switch (type) {
    case ActionType.NONE:
      return "无动作";
    case ActionType.KEYBOARD: {
      const keyName = getKeycodeName(param1, mod);
      return `键盘: ${keyName}`;
    }
    case ActionType.MOUSE_BTN: {
      const btns: string[] = [];
      if (param1 & MouseButton.LEFT) btns.push("左键");
      if (param1 & MouseButton.RIGHT) btns.push("右键");
      if (param1 & MouseButton.MIDDLE) btns.push("中键");
      if (param1 & MouseButton.BACK) btns.push("后退");
      if (param1 & MouseButton.FORWARD) btns.push("前进");
      return `鼠标按键: ${btns.join("+")}`;
    }
    case ActionType.MOUSE_WHEEL: {
      const dir =
        param1 === WheelDirection.UP
          ? "上"
          : param1 === WheelDirection.DOWN
            ? "下"
            : param1 === WheelDirection.CLICK
              ? "点击"
              : `未知(${param1})`;
      return `鼠标滚轮: ${dir}`;
    }
    case ActionType.CONSUMER: {
      const usageId = param1 | (param2 << 8);
      return `多媒体键: ${hex(usageId)}`;
    }
    case ActionType.MACRO:
      return `执行宏 #${param1}`;
    case ActionType.LAYER: {
      const op = LAYER_OP_NAMES[mod] || `未知(${mod})`;
      return `层切换: ${op} → 层${param1 + 1}`;
    }
    default:
      return `${typeName}: mod=${hex(mod)} p1=${hex(param1)} p2=${hex(param2)}`;
  }
}

// ============================================================================
// 发送帧解析
// ============================================================================

export function getCommandName(cmd: number): string {
  return COMMAND_NAMES[cmd] || `UNKNOWN_${hex(cmd)}`;
}

export function getCommandLabel(cmd: number): string {
  return COMMAND_LABELS[cmd] || `未知命令 ${hex(cmd)}`;
}

/** 解析发送帧为人类可读文本 */
export function parseSendFrame(frame: Uint8Array): {
  command: string;
  cmdHex: string;
  sub: number;
  dataLen: number;
  rawHex: string;
  parsed: string;
} {
  const cmd = frame[0];
  const sub = frame[1];
  const len = frame[2];
  const data = frame.slice(3, 3 + len);

  const command = getCommandName(cmd);
  const label = getCommandLabel(cmd);
  const rawHex = toHexDump(frame, 3 + len);
  let parsed = `${label}`;

  // 详细解析各命令
  switch (cmd) {
    case Command.KEYMAP_GET:
      parsed += ` | 层 ${sub + 1}`;
      break;

    case Command.KEYMAP_SET: {
      parsed += ` | 层 ${sub + 1}`;
      if (len >= 35) {
        const numLayers = data[0];
        const defaultLayer = data[2];
        parsed += ` | 总层数=${numLayers}, 默认层=${defaultLayer + 1}`;
        // 解析按键
        const keys: string[] = [];
        for (let i = 0; i < 8 && 3 + i * 4 < len; i++) {
          const offset = 3 + i * 4;
          const t = data[offset],
            m = data[offset + 1],
            p1 = data[offset + 2],
            p2 = data[offset + 3];
          if (t !== ActionType.NONE) {
            keys.push(`K${i}=[${formatKeyAction(t, m, p1, p2)}]`);
          }
        }
        if (keys.length) parsed += ` | ${keys.join(", ")}`;
      }
      break;
    }

    case Command.LAYER_SET:
      parsed += ` | 切换到层 ${sub + 1}`;
      break;

    case Command.CFG_OS_SET:
      if (len >= 1) {
        parsed += ` | ${data[0] === OsMode.MAC ? "Mac" : "Win"}`;
      }
      break;

    case Command.RGB_SET:
      if (len >= 9) {
        const enabled = data[0] ? "开" : "关";
        const mode = RGB_MODE_NAMES[data[1]] || `未知(${data[1]})`;
        const brightness = Math.round((data[2] * 100) / 255);
        const r = data[4],
          g = data[5],
          b = data[6];
        const press =
          PRESS_EFFECT_NAMES[data[9] ?? PressEffect.NONE] || `未知(${data[9]})`;
        parsed += ` | ${enabled}, 模式=${mode}, 亮度=${brightness}%, 颜色=#${r.toString(16).padStart(2, "0")}${g.toString(16).padStart(2, "0")}${b.toString(16).padStart(2, "0")}, 按键附魔=${press}`;
        if (len >= 13) parsed += `, 无感唤醒=${data[12] ? "开" : "关"}`;
      }
      break;

    case Command.FNKEY_SET:
      if (len >= 32) {
        const fnDescs: string[] = [];
        for (let i = 0; i < 4; i++) {
          const offset = i * 8;
          const click = FN_ACTION_NAMES[data[offset]] || hex(data[offset]);
          const long =
            FN_ACTION_NAMES[data[offset + 2]] || hex(data[offset + 2]);
          if (data[offset] !== 0 || data[offset + 2] !== 0) {
            fnDescs.push(`FN${i + 1}[单击=${click}, 长按=${long}]`);
          }
        }
        if (fnDescs.length) parsed += ` | ${fnDescs.join(", ")}`;
      }
      break;

    case Command.MACRO_SET: {
      const seq = len > 0 ? data[0] : -1;
      if (seq === 0) parsed += ` | 槽位 ${sub}, 开始写入`;
      else if (seq === 0xff) parsed += ` | 槽位 ${sub}, 写入完成`;
      else parsed += ` | 槽位 ${sub}, 块 #${seq}`;
      break;
    }

    case Command.MACRO_DEL:
      parsed += ` | 删除槽位 ${sub}`;
      break;

    case Command.DATAFLASH_READ:
      if (len >= 3) {
        const offset = (data[0] << 8) | data[1];
        parsed += ` | ${hex(offset)} +${data[2]}B`;
      }
      break;

    case Command.DATAFLASH_WRITE:
      if (len >= 3) {
        const offset = (data[0] << 8) | data[1];
        parsed += ` | ${hex(offset)} = ${hex(data[2])}`;
      }
      break;
  }

  return {
    command,
    cmdHex: cmd.toString(16).padStart(2, "0"),
    sub,
    dataLen: len,
    rawHex,
    parsed,
  };
}

// ============================================================================
// 响应帧解析
// ============================================================================

/** 解析响应帧为人类可读文本 */
export function parseReceiveFrame(frame: Uint8Array): {
  command: string;
  cmdHex: string;
  sub: number;
  dataLen: number;
  rawHex: string;
  parsed: string;
  statusCode: string;
  isError: boolean;
} {
  const cmdByte = frame[0];
  const sub = frame[1];
  const len = frame[2];
  const data = frame.slice(3, 3 + Math.max(len, 1));

  // 响应帧 CMD 可能是原始命令码直接回传
  const status = data[0];
  const statusName = RESPONSE_CODE_NAMES[status] || hex(status);
  const statusLabel = RESPONSE_CODE_LABELS[status] || "未知状态";
  const isError = status !== ResponseCode.OK;

  // 响应的命令码通常与请求相同
  const command = getCommandName(cmdByte);
  const label = getCommandLabel(cmdByte);
  const rawHex = toHexDump(frame, 3 + len);

  let parsed = `${label} → ${statusName}(${statusLabel})`;

  if (isError) {
    return {
      command,
      cmdHex: cmdByte.toString(16).padStart(2, "0"),
      sub,
      dataLen: len,
      rawHex,
      parsed,
      statusCode: statusName,
      isError,
    };
  }

  // 详细解析响应数据
  switch (cmdByte) {
    case Command.SYS_INFO:
      if (len >= 14) {
        const vid = (data[1] << 8) | data[2];
        const pid = (data[3] << 8) | data[4];
        const ver = `${data[5]}.${data[6]}.${data[7]}`;
        const maxLayers = data[8];
        const macros = data[10];
        const kbType = KEYBOARD_TYPE_NAMES[data[11]] || `未知(${data[11]})`;
        const keyCount = data[12];
        const fnCount = data[13];
        parsed += ` | VID=${hex(vid)} PID=${hex(pid)} v${ver} | ${kbType} ${keyCount}键 ${fnCount}FN | ${maxLayers}层 ${macros}宏`;
      }
      break;

    case Command.SYS_STATUS:
      if (len >= 6) {
        const mode = data[1] === 0 ? "USB" : "BLE";
        const connectionStates = ["未连接", "广播中", "已连接", "挂起"];
        const conn = connectionStates[data[2]] ?? `未知状态(${data[2]})`;
        const layer = data[3];
        const battery = data[4];
        const charging = data[5] ? "充电中" : "未充电";
        parsed += ` | ${mode} ${conn} | 层${layer + 1} | 电量 ${battery}% ${charging}`;
        if (len >= 9) {
          const adcRaw = data[6] | (data[7] << 8);
          const chargePinRaw = data[8];
          parsed += ` | ADC原始值 ${adcRaw} | CHRG原始值 ${chargePinRaw}`;
        }
      }
      break;

    case Command.DATAFLASH_INFO:
      if (len >= 17) {
        const total = (data[1] << 8) | data[2];
        const page = (data[3] << 8) | data[4];
        const macroBase = (data[9] << 8) | data[10];
        const macroSize = (data[11] << 8) | data[12];
        parsed += ` | 总计 ${total}B, 页 ${page}B, 宏区 ${hex(macroBase)} +${macroSize}B`;
      }
      break;

    case Command.DATAFLASH_READ:
      if (len >= 2) {
        parsed += ` | ${data[1]}B`;
      }
      break;

    case Command.DATAFLASH_WRITE:
      if (len >= 4) {
        const offset = (data[1] << 8) | data[2];
        parsed += ` | ${hex(offset)} = ${hex(data[3])}`;
      }
      break;

    case Command.CFG_OS_GET:
      if (len >= 2) {
        parsed += ` | ${data[1] === OsMode.MAC ? "Mac" : "Win"}`;
      }
      break;

    case Command.KEYMAP_GET:
      if (len >= 36) {
        const numLayers = data[1];
        const curLayer = data[2];
        const defLayer = data[3];
        parsed += ` | 层 ${sub + 1}/${numLayers} (当前=${curLayer + 1}, 默认=${defLayer + 1})`;
        const keys: string[] = [];
        for (let i = 0; i < 8 && 4 + i * 4 < len; i++) {
          const offset = 4 + i * 4;
          const t = data[offset],
            m = data[offset + 1],
            p1 = data[offset + 2],
            p2 = data[offset + 3];
          if (t !== ActionType.NONE) {
            keys.push(`K${i}=[${formatKeyAction(t, m, p1, p2)}]`);
          }
        }
        if (keys.length) parsed += ` | ${keys.join(", ")}`;
      }
      break;

    case Command.RGB_GET:
      if (len >= 9) {
        const enabled = data[1] ? "开" : "关";
        const mode = RGB_MODE_NAMES[data[2]] || `未知(${data[2]})`;
        const brightness = Math.round((data[3] * 100) / 255);
        const r = data[5],
          g = data[6],
          b = data[7];
        const indicator = data[8] ? "指示灯开" : "指示灯关";
        const press =
          PRESS_EFFECT_NAMES[data[10] ?? PressEffect.NONE] ||
          `未知(${data[10]})`;
        parsed += ` | ${enabled}, ${mode}, 亮度=${brightness}%, #${r.toString(16).padStart(2, "0")}${g.toString(16).padStart(2, "0")}${b.toString(16).padStart(2, "0")}, ${indicator}, 按键附魔=${press}`;
        if (len >= 14) parsed += `, 无感唤醒=${data[13] ? "开" : "关"}`;
      }
      break;

    case Command.FNKEY_GET:
      if (len >= 33) {
        const fnDescs: string[] = [];
        for (let i = 0; i < 4; i++) {
          const offset = 1 + i * 8;
          const click = FN_ACTION_NAMES[data[offset]] || hex(data[offset]);
          const long =
            FN_ACTION_NAMES[data[offset + 2]] || hex(data[offset + 2]);
          const longMs = data[offset + 4] | (data[offset + 5] << 8);
          if (data[offset] !== 0 || data[offset + 2] !== 0) {
            fnDescs.push(
              `FN${i + 1}[单击=${click}, 长按=${long}, ${longMs}ms]`,
            );
          }
        }
        if (fnDescs.length) parsed += ` | ${fnDescs.join(", ")}`;
      }
      break;

    case Command.BATTERY:
      if (len >= 5) {
        const battery = data[1];
        const charging = data[2] ? "充电中" : "未充电";
        const voltageMv = data[3] | (data[4] << 8);
        const voltage = (voltageMv / 1000).toFixed(2);
        parsed += ` | ${battery}% ${charging} ${voltage}V`;
        if (len >= 8) {
          const adcRaw = data[5] | (data[6] << 8);
          const chargePinRaw = data[7];
          parsed += ` | ADC原始值 ${adcRaw} | CHRG原始值 ${chargePinRaw}`;
        }
      }
      break;

    case Command.LAYER_GET:
      if (len >= 3) {
        parsed += ` | 当前层=${data[1] + 1}, 默认层=${data[2] + 1}`;
      }
      break;
  }

  return {
    command,
    cmdHex: cmdByte.toString(16).padStart(2, "0"),
    sub,
    dataLen: len,
    rawHex,
    parsed,
    statusCode: statusName,
    isError,
  };
}

// ============================================================================
// 固件日志帧解析 (CMD=0x70)
// ============================================================================

const LOG_CATEGORY_NAMES: Record<number, string> = {
  [LogCategory.KEY_EVENT]: "KEY",
  [LogCategory.FN_EVENT]: "FN",
  [LogCategory.LAYER_EVENT]: "LAYER",
  [LogCategory.MODE_EVENT]: "MODE",
  [LogCategory.BLE_EVENT]: "BLE",
  [LogCategory.RGB_EVENT]: "RGB",
  [LogCategory.SYSTEM_EVENT]: "SYSTEM",
};

const LOG_CATEGORY_LABELS: Record<number, string> = {
  [LogCategory.KEY_EVENT]: "按键事件",
  [LogCategory.FN_EVENT]: "FN 键事件",
  [LogCategory.LAYER_EVENT]: "层切换",
  [LogCategory.MODE_EVENT]: "模式切换",
  [LogCategory.BLE_EVENT]: "蓝牙事件",
  [LogCategory.RGB_EVENT]: "RGB 事件",
  [LogCategory.SYSTEM_EVENT]: "系统事件",
};

type LogCategoryKey =
  | "key"
  | "fn"
  | "layer"
  | "mode"
  | "ble"
  | "rgb"
  | "system";

const LOG_CATEGORY_KEYS: Record<number, LogCategoryKey> = {
  [LogCategory.KEY_EVENT]: "key",
  [LogCategory.FN_EVENT]: "fn",
  [LogCategory.LAYER_EVENT]: "layer",
  [LogCategory.MODE_EVENT]: "mode",
  [LogCategory.BLE_EVENT]: "ble",
  [LogCategory.RGB_EVENT]: "rgb",
  [LogCategory.SYSTEM_EVENT]: "system",
};

/** 解析固件日志帧 */
export function parseLogFrame(frame: Uint8Array): {
  command: string;
  cmdHex: string;
  sub: number;
  dataLen: number;
  rawHex: string;
  parsed: string;
  category: LogCategoryKey;
} {
  const sub = frame[1]; // category
  const len = frame[2];
  const data = frame.slice(3, 3 + len);

  const catName = LOG_CATEGORY_NAMES[sub] || `CAT_${sub}`;
  const catLabel = LOG_CATEGORY_LABELS[sub] || `未知类别(${sub})`;
  const category = LOG_CATEGORY_KEYS[sub] || "system";
  const rawHex = toHexDump(frame, 3 + len);

  let parsed = catLabel;

  switch (sub) {
    case LogCategory.KEY_EVENT:
      if (len >= 4) {
        const keyIdx = data[0];
        const pressed = data[1] ? "按下" : "释放";
        const actType = ACTION_TYPE_NAMES[data[2]] || hex(data[2]);
        const param = data[3];
        const keyName =
          data[2] === ActionType.KEYBOARD
            ? getKeycodeName(param, 0)
            : hex(param);
        parsed += ` | 键${keyIdx} ${pressed} [${actType}: ${keyName}]`;
      }
      break;

    case LogCategory.FN_EVENT:
      if (len >= 4) {
        const fnId = data[0];
        const isLong = data[1] ? "长按" : "单击";
        const action = FN_ACTION_NAMES[data[2]] || hex(data[2]);
        parsed += ` | FN${fnId + 1} ${isLong} → ${action}`;
      }
      break;

    case LogCategory.LAYER_EVENT:
      if (len >= 2) {
        parsed += ` | 层${data[0] + 1} → 层${data[1] + 1}`;
      }
      break;

    case LogCategory.MODE_EVENT:
      if (len >= 2) {
        const oldMode = data[0] === 0 ? "USB" : "BLE";
        const newMode = data[1] === 0 ? "USB" : "BLE";
        parsed += ` | ${oldMode} → ${newMode}`;
      }
      break;

    case LogCategory.BLE_EVENT:
      if (len >= 1) {
        const states = ["未连接", "广播中", "已连接", "休眠"];
        parsed += ` | ${states[data[0]] || hex(data[0])}`;
      }
      break;

    case LogCategory.RGB_EVENT:
      if (len >= 2) {
        const mode = RGB_MODE_NAMES[data[0]] || hex(data[0]);
        const brightness = Math.round((data[1] * 100) / 255);
        parsed += ` | ${mode} 亮度=${brightness}%`;
      }
      break;

    case LogCategory.SYSTEM_EVENT:
      if (len >= 1) {
        const events: Record<number, string> = {
          [SystemLogEvent.BOOT]: "设备启动",
          [SystemLogEvent.SLEEP]: "进入休眠",
          [SystemLogEvent.WAKEUP]: "唤醒",
        };
        parsed += ` | ${events[data[0]] || hex(data[0])}`;
      }
      break;
  }

  return {
    command: `LOG_${catName}`,
    cmdHex: "70",
    sub,
    dataLen: len,
    rawHex,
    parsed,
    category,
  };
}
