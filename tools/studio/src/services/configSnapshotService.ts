import { hidService } from '@/services/HidService';
import { useDeviceStore } from '@/stores/deviceStore';
import { useMacroStore } from '@/stores/macroStore';
import {
  CONFIG_BACKUP_SCHEMA,
  CONFIG_BACKUP_SCHEMA_VERSION,
  type ConfigBackupDeviceInfo,
  type ConfigBackupFile,
  type ConfigBackupMacroEntry,
  type ConfigCompatibilityResult,
} from '@/types/configBackup';
import type { DeviceInfo, MacroData } from '@/types/protocol';
import { ActionType, FnAction, MacroActionType } from '@/types/protocol';

const MEOWFS_ENTRY_HEADER_BYTES = 2;
const MEOWFS_ACTION_BYTES = 2;
const MEOWFS_MAX_ACTIONS = 255;
const VALID_ACTION_TYPES = new Set(Object.values(ActionType).filter((value): value is number => typeof value === 'number'));
const VALID_FN_ACTIONS = new Set(Object.values(FnAction).filter((value): value is number => typeof value === 'number'));
const VALID_MACRO_ACTION_TYPES = new Set(Object.values(MacroActionType).filter((value): value is number => typeof value === 'number'));

function cloneJson<T>(value: T): T {
  return JSON.parse(JSON.stringify(value)) as T;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null;
}

function isByte(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= 0xff;
}

function isUint16(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= 0xffff;
}

function firmwareVersion(info: DeviceInfo): string {
  const dev = info.versionMajor === 0 && info.versionMinor === 0 && info.versionPatch === 0;
  return dev ? 'dev' : `${info.versionMajor}.${info.versionMinor}.${info.versionPatch}`;
}

function backupHashInput(backup: ConfigBackupFile): string {
  const { checksum, ...rest } = backup;
  return JSON.stringify(rest);
}

function assertBackupEnvelope(backup: ConfigBackupFile): void {
  if (backup.schema !== CONFIG_BACKUP_SCHEMA) {
    throw new Error('不是 BinaryKeyboard 配置文件');
  }
  if (backup.schemaVersion !== CONFIG_BACKUP_SCHEMA_VERSION) {
    throw new Error(`不支持的配置文件版本：${backup.schemaVersion}`);
  }
}

function validateConfigBackupShape(backup: ConfigBackupFile): void {
  const errors: string[] = [];
  const keymap = backup.config?.keymap;

  if (!keymap || !Number.isInteger(keymap.numLayers) || keymap.numLayers < 1) {
    errors.push('键位层数无效');
  }
  if (!Array.isArray(keymap?.layers) || keymap.layers.length < (keymap?.numLayers ?? 0)) {
    errors.push('键位层数据不完整');
  }
  if (keymap && (!Number.isInteger(keymap.currentLayer) || keymap.currentLayer < 0 || keymap.currentLayer >= keymap.numLayers)) {
    errors.push('当前层超出范围');
  }
  if (keymap && (!Number.isInteger(keymap.defaultLayer) || keymap.defaultLayer < 0 || keymap.defaultLayer >= keymap.numLayers)) {
    errors.push('默认层超出范围');
  }

  keymap?.layers?.slice(0, keymap.numLayers).forEach((layer, layerIndex) => {
    if (!Array.isArray(layer.keys)) {
      errors.push(`第 ${layerIndex + 1} 层缺少按键数据`);
      return;
    }
    layer.keys.forEach((action, keyIndex) => {
      if (!isRecord(action) || !VALID_ACTION_TYPES.has(action.type as number)) {
        errors.push(`第 ${layerIndex + 1} 层按键 ${keyIndex + 1} 动作类型无效`);
        return;
      }
      if (!isByte(action.modifier) || !isByte(action.param1) || !isByte(action.param2)) {
        errors.push(`第 ${layerIndex + 1} 层按键 ${keyIndex + 1} 动作参数无效`);
      }
    });
  });

  const rgb = backup.config?.rgb;
  if (rgb) {
    if (!isRecord(rgb)) {
      errors.push('RGB 配置格式无效');
    }
    if (typeof rgb.enabled !== 'boolean' || typeof rgb.indicatorEnabled !== 'boolean') errors.push('RGB 开关字段无效');
    ['mode', 'brightness', 'speed', 'colorR', 'colorG', 'colorB', 'indicatorBrightness', 'pressEffect'].forEach((key) => {
      if (!isByte((rgb as unknown as Record<string, unknown>)[key])) errors.push(`RGB ${key} 无效`);
    });
    ['lightSleepMin', 'deepSleepMin', 'pollRate'].forEach((key) => {
      const value = (rgb as unknown as Record<string, unknown>)[key];
      if (value !== undefined && !isByte(value)) errors.push(`RGB ${key} 无效`);
    });
    if (rgb.seamlessWakeEnabled !== undefined && typeof rgb.seamlessWakeEnabled !== 'boolean') {
      errors.push('无感唤醒配置无效');
    }
  }

  const fnConfig = backup.config?.fnKeys;
  if (fnConfig !== undefined && (!isRecord(fnConfig) || !Array.isArray(fnConfig.fnKeys))) {
    errors.push('FN 配置格式无效');
  } else if (fnConfig) {
    fnConfig.fnKeys.forEach((fn, index) => {
      if (!isRecord(fn) || !VALID_FN_ACTIONS.has(fn.clickAction as number) || !VALID_FN_ACTIONS.has(fn.longAction as number)) {
        errors.push(`FN ${index + 1} 动作类型无效`);
        return;
      }
      if (!isByte(fn.clickAction) || !isByte(fn.clickParam) || !isByte(fn.longAction) || !isByte(fn.longParam) || !isUint16(fn.longPressMs)) {
        errors.push(`FN ${index + 1} 参数无效`);
      }
    });
  }

  if (backup.config?.log && typeof backup.config.log.enabled !== 'boolean') {
    errors.push('日志配置无效');
  }

  if (backup.config?.osMode && ![0, 1].includes(backup.config.osMode.mode)) {
    errors.push('OS 模式无效');
  }

  const macros = backup.config?.macros;
  if (macros !== undefined && !Array.isArray(macros)) {
    errors.push('宏配置格式无效');
  } else {
    macros?.forEach((macro, macroIndex) => {
    if (!Number.isInteger(macro.slot) || macro.slot < 0) {
      errors.push(`宏 ${macroIndex + 1} 槽位无效`);
    }
    if (!Array.isArray(macro.data?.actions)) {
      errors.push(`宏 ${macroIndex + 1} 缺少动作数据`);
      return;
    }
    if (storedMacroActionCount(macro.data) > MEOWFS_MAX_ACTIONS) errors.push(`宏 ${macroIndex + 1} 动作数超出上限`);
    macro.data.actions.forEach((action, actionIndex) => {
      if (!VALID_MACRO_ACTION_TYPES.has(action.type) || !isByte(action.param)) {
        errors.push(`宏 ${macroIndex + 1} 动作 ${actionIndex + 1} 参数无效`);
      }
    });
    });
  }

  if (errors.length) {
    throw new Error(`配置文件结构无效：${errors.join('；')}`);
  }
}

async function sha256(text: string): Promise<string> {
  const data = new TextEncoder().encode(text);
  const digest = await crypto.subtle.digest('SHA-256', data);
  return Array.from(new Uint8Array(digest), (byte) => byte.toString(16).padStart(2, '0')).join('');
}

function deviceInfoForBackup(info: DeviceInfo): ConfigBackupDeviceInfo {
  return {
    protocol: info.protocol,
    protocolLabel: info.protocolLabel,
    chipFamily: info.chipFamily,
    keyboardType: info.keyboardType,
    firmwareVersion: firmwareVersion(info),
    maxLayers: info.maxLayers,
    maxKeys: info.maxKeys,
    actualKeyCount: info.actualKeyCount,
    fnKeyCount: info.fnKeyCount,
    macroSlots: info.macroSlots,
  };
}

async function readMacros(): Promise<ConfigBackupMacroEntry[] | undefined> {
  const deviceStore = useDeviceStore();
  if (!deviceStore.supportsMacroActions) return undefined;

  const overview = await hidService.getMacroOverview();
  const macros: ConfigBackupMacroEntry[] = [];
  for (let slot = 0; slot < overview.slotValid.length; slot++) {
    if (!overview.slotValid[slot]) continue;
    const macro = await hidService.getMacroData(slot);
    macros.push({ slot, data: cloneJson(macro) });
  }
  return macros;
}

async function clearMacros(): Promise<void> {
  const overview = await hidService.getMacroOverview();
  for (let slot = overview.slotValid.length - 1; slot >= 0; slot--) {
    if (!overview.slotValid[slot]) continue;
    await hidService.deleteMacro(slot);
  }
}

function normalizeMacroForSlot(macro: MacroData, slot: number): MacroData {
  const actions = cloneJson(macro.actions);
  if (!actions.some((action) => action.type === MacroActionType.END)) {
    actions.push({ type: MacroActionType.END, param: 0 });
  }
  const storedCount = actions.filter((action) => action.type !== MacroActionType.END).length;
  return {
    header: {
      ...cloneJson(macro.header),
      valid: storedCount > 0 ? 1 : macro.header.valid,
      id: slot,
      actionCount: storedCount,
      dataSize: storedCount * 2,
    },
    actions,
  };
}

function storedMacroActionCount(macro: MacroData): number {
  return macro.actions.filter((action) => action.type !== MacroActionType.END).length;
}

function estimateMeowFsBytes(macros: ConfigBackupMacroEntry[]): number {
  return macros.reduce(
    (total, macro) => total + MEOWFS_ENTRY_HEADER_BYTES + storedMacroActionCount(macro.data) * MEOWFS_ACTION_BYTES,
    0,
  );
}

function collectMacroReferences(backup: ConfigBackupFile): number[] {
  const refs = new Set<number>();
  backup.config.keymap.layers.slice(0, backup.config.keymap.numLayers).forEach((layer) => {
    layer.keys.forEach((action) => {
      if (action.type === ActionType.MACRO) refs.add(action.param1);
    });
  });
  backup.config.fnKeys?.fnKeys.forEach((fn) => {
    if (fn.clickAction === FnAction.MACRO) refs.add(fn.clickParam);
    if (fn.longAction === FnAction.MACRO) refs.add(fn.longParam);
  });
  return [...refs].sort((a, b) => a - b);
}

function assertMacroSlotsRestorable(macros: ConfigBackupMacroEntry[]): void {
  const sortedSlots = macros.map((macro) => macro.slot).sort((a, b) => a - b);
  sortedSlots.forEach((slot, index) => {
    if (slot !== index) {
      throw new Error('当前宏存储不支持稀疏宏槽恢复；请先用宏包功能迁移宏数据');
    }
  });
}

async function assertMacrosFitCurrentDevice(macros: ConfigBackupMacroEntry[]): Promise<void> {
  const overview = await hidService.getMacroOverview();
  const blocking: string[] = [];

  macros.forEach((macro, index) => {
    const count = storedMacroActionCount(macro.data);
    if (count > MEOWFS_MAX_ACTIONS) {
      blocking.push(`宏 ${index + 1} 有 ${count} 个动作，超过上限 ${MEOWFS_MAX_ACTIONS}`);
    }
  });
  try {
    assertMacroSlotsRestorable(macros);
  } catch (error) {
    blocking.push(error instanceof Error ? error.message : '宏槽位不可恢复');
  }

  const fixedSlotLimit = overview.maxSlots ?? (overview.dynamic ? undefined : overview.totalSlots);
  if (fixedSlotLimit !== undefined && macros.length > fixedSlotLimit) {
    blocking.push(`配置包含 ${macros.length} 个宏，当前设备支持 ${fixedSlotLimit} 个`);
  }

  const bytesNeeded = estimateMeowFsBytes(macros);
  if (overview.fsTotal !== undefined && bytesNeeded > overview.fsTotal) {
    blocking.push(`宏数据需要 ${bytesNeeded} 字节，当前 MeowFS 容量为 ${overview.fsTotal} 字节`);
  }

  if (blocking.length) {
    throw new Error(blocking.join('; '));
  }
}

export async function createConfigBackupFromDevice(name: string): Promise<ConfigBackupFile> {
  const deviceStore = useDeviceStore();
  const info = await hidService.getSysInfo();
  const keymap = await hidService.getFullKeymap();
  const config: ConfigBackupFile['config'] = {
    keymap: cloneJson(keymap),
  };

  if (info.capabilities.rgb) {
    config.rgb = cloneJson(await hidService.getRgbConfig());
  }
  if (info.capabilities.fnKeys) {
    config.fnKeys = cloneJson(await hidService.getFnKeyConfig());
  }
  if (info.capabilities.osMode) {
    config.osMode = cloneJson(await hidService.getOsMode());
  }
  if (info.capabilities.logs) {
    config.log = cloneJson(await hidService.getLogConfig());
  }
  if (info.capabilities.macroActions || deviceStore.supportsMacroActions) {
    config.macros = await readMacros();
  }

  const backup: ConfigBackupFile = {
    schema: CONFIG_BACKUP_SCHEMA,
    schemaVersion: CONFIG_BACKUP_SCHEMA_VERSION,
    createdAt: new Date().toISOString(),
    name,
    source: 'device',
    device: deviceInfoForBackup(info),
    config,
    checksum: {
      algorithm: 'sha256',
      value: '',
    },
  };
  backup.checksum.value = await sha256(backupHashInput(backup));
  return backup;
}

export async function finalizeImportedBackup(
  backup: ConfigBackupFile,
  source: ConfigBackupFile['source'] = 'file',
): Promise<ConfigBackupFile> {
  const next = cloneJson(backup);
  next.source = source;
  next.checksum = { algorithm: 'sha256', value: '' };
  next.checksum.value = await sha256(backupHashInput(next));
  return next;
}

export async function parseConfigBackupFile(text: string): Promise<ConfigBackupFile> {
  const parsed = JSON.parse(text) as ConfigBackupFile;
  assertBackupEnvelope(parsed);
  if (parsed.checksum?.algorithm !== 'sha256' || !parsed.checksum.value) {
    throw new Error('配置文件缺少校验信息');
  }

  const expected = await sha256(backupHashInput(parsed));
  if (expected !== parsed.checksum.value) {
    throw new Error('配置文件校验失败，文件可能已损坏或被修改');
  }
  validateConfigBackupShape(parsed);
  return parsed;
}

export async function verifyConfigBackupIntegrity(backup: ConfigBackupFile): Promise<void> {
  assertBackupEnvelope(backup);
  if (backup.checksum?.algorithm !== 'sha256' || !backup.checksum.value) {
    throw new Error('配置文件缺少校验信息');
  }
  const expected = await sha256(backupHashInput(backup));
  if (expected !== backup.checksum.value) {
    throw new Error('配置文件校验失败，宏数据可能已损坏');
  }
  validateConfigBackupShape(backup);
}

export function checkConfigCompatibility(backup: ConfigBackupFile): ConfigCompatibilityResult {
  const deviceStore = useDeviceStore();
  const info = deviceStore.deviceInfo;
  const blocking: string[] = [];
  const warnings: string[] = [];

  if (!info) {
    return { ok: false, blocking: ['当前没有连接设备'], warnings };
  }
  if (backup.device.protocol !== info.protocol) {
    blocking.push(`协议不一致：文件为 ${backup.device.protocol}，当前为 ${info.protocol}`);
  }
  if (backup.device.keyboardType !== info.keyboardType) {
    blocking.push('键盘型号不一致');
  }
  if (backup.config.keymap.numLayers > info.maxLayers) {
    blocking.push(`层数超出当前设备上限：${backup.config.keymap.numLayers} > ${info.maxLayers}`);
  }
  const maxKeys = Math.max(info.maxKeys, info.actualKeyCount);
  const needsKeys = Math.max(...backup.config.keymap.layers.flatMap((layer) => [layer.keys.length]), 0);
  if (needsKeys > maxKeys) {
    blocking.push(`按键数超出当前设备上限：${needsKeys} > ${maxKeys}`);
  }
  if (backup.config.fnKeys && !info.capabilities.fnKeys) {
    blocking.push('配置包含 FN，但当前设备不支持');
  }
  if (backup.config.rgb && !info.capabilities.rgb) {
    blocking.push('配置包含 RGB，但当前设备不支持');
  }
  if (backup.config.osMode && !info.capabilities.osMode) {
    warnings.push('当前设备不支持 OS 模式，将跳过该部分');
  }
  if (backup.config.log && !info.capabilities.logs) {
    warnings.push('当前设备不支持日志配置，将跳过该部分');
  }
  if ((backup.config.macros?.length ?? 0) > 0 && !info.capabilities.macroActions) {
    blocking.push('配置包含宏数据，但当前设备不支持');
  }
  const macroRefs = collectMacroReferences(backup);
  const macroSlots = new Set((backup.config.macros ?? []).map((macro) => macro.slot));
  const danglingMacroRef = macroRefs.find((slot) => !macroSlots.has(slot));
  if (danglingMacroRef !== undefined) {
    blocking.push(`键位引用宏 ${danglingMacroRef + 1}，但配置中没有对应宏数据`);
  }
  if (backup.device.firmwareVersion !== firmwareVersion(info)) {
    warnings.push(`固件版本不同：文件 ${backup.device.firmwareVersion}，当前 ${firmwareVersion(info)}`);
  }

  return { ok: blocking.length === 0, blocking, warnings };
}

export async function applyConfigBackupToDevice(backup: ConfigBackupFile): Promise<void> {
  await verifyConfigBackupIntegrity(backup);
  const compatibility = checkConfigCompatibility(backup);
  if (!compatibility.ok) {
    throw new Error(compatibility.blocking.join('；'));
  }

  const info = await hidService.getSysInfo();
  if (backup.config.macros && info.capabilities.macroActions) {
    await assertMacrosFitCurrentDevice(backup.config.macros);
  }

  await hidService.setFullKeymap(cloneJson(backup.config.keymap));
  if (backup.config.rgb && info.capabilities.rgb) {
    await hidService.setRgbConfig(cloneJson(backup.config.rgb));
  }
  if (backup.config.fnKeys && info.capabilities.fnKeys) {
    await hidService.setFnKeyConfig(cloneJson(backup.config.fnKeys));
  }
  if (backup.config.osMode && info.capabilities.osMode) {
    await hidService.setOsMode(cloneJson(backup.config.osMode));
  }
  if (backup.config.log && info.capabilities.logs) {
    await hidService.setLogConfig(cloneJson(backup.config.log));
  }
  if (backup.config.macros && info.capabilities.macroActions) {
    await clearMacros();
    const macros = [...backup.config.macros].sort((a, b) => a.slot - b.slot);
    for (const macro of macros) {
      await hidService.setMacroData(macro.slot, normalizeMacroForSlot(macro.data, macro.slot));
    }
  }
  if (info.capabilities.explicitSave) {
    await hidService.saveConfig();
  }

  const deviceStore = useDeviceStore();
  const macroStore = useMacroStore();
  await deviceStore.refreshDeviceInfo();
  await deviceStore.refreshKeymap();
  if (deviceStore.supportsRgb) await deviceStore.refreshRgbConfig();
  if (deviceStore.supportsFnKeys) await deviceStore.refreshFnKeyConfig();
  if (deviceStore.supportsOsMode) await deviceStore.refreshOsMode();
  if (deviceStore.supportsMacroActions) {
    macroStore.invalidateCache();
    await macroStore.refreshOverview().catch(() => {});
  }
}

export function backupToDownloadName(backup: ConfigBackupFile): string {
  const safeName = backup.name
    .trim()
    .replace(/[<>:"/\\|?*\x00-\x1F]+/g, '-')
    .replace(/[. ]+$/g, '')
    .slice(0, 96) || 'keyboard-config';
  return `${safeName}.binarykeyboard-config.json`;
}
