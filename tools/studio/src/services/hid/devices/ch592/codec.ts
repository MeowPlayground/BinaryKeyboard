import {
  CH592_CAPABILITIES,
  Command,
  DeviceProtocol,
  FRAME_SIZE,
  MAX_FN_KEYS,
  MAX_KEYS,
  MacroActionType,
  OsMode,
  ResponseCode,
  createEmptyAction,
  createEmptyKeymap,
  type DeviceInfo,
  type DeviceStatus,
  type FnKeyConfig,
  type FnKeyEntry,
  type KeyAction,
  type LayerConfig,
  type LogConfig,
  type KeymapConfig,
  type MacroAction,
  type MacroData,
  type MacroHeader,
  type MacroOverview,
  type OsModeConfig,
  type RgbConfig,
} from '@/types/protocol';
import { FIRMWARE_VERSION_META } from '@/generated/versionConfig';
import { parseLogFrame, parseReceiveFrame, parseSendFrame } from '@/utils/protocolParser';
import type { BatteryInfo, HidOptionalOperations } from '../../common/types';
import type {
  CodecInboundPacket,
  CodecTransport,
  DeviceCodec,
  TerminalEntryDraft,
} from '../../common/codecTypes';

const RESP_HEADER_SIZE = 3;
const CH592_MEOWFS_HEADER_SIZE = 2;
const CH592_MEOWFS_ACTION_SIZE = 2;
const CH592_MEOWFS_MAX_ACTIONS = 255;
const CH592_MEOWFS_APPEND_SLOTS = 1;
const CH592_MEOWFS_READ_CHUNK = 59;
const CH592_MEOWFS_WRITE_CHUNK = 58;

enum Ch592MacroSetSub {
  ERASE = 0,
  WRITE = 1,
}

interface MeowFsMacroEntry {
  actionCount: number;
  actions: MacroAction[];
}

interface MeowFsCache {
  fsTotal: number;
  fsFree: number;
  pageSize: number;
  macros: MeowFsMacroEntry[];
}

export class Ch592Codec implements DeviceCodec<DataView> {
  readonly protocol = DeviceProtocol.CH592;
  readonly protocolLabel = 'CH592F HID';
  readonly capabilities = CH592_CAPABILITIES;
  readonly chipFamily = FIRMWARE_VERSION_META.CH592F.chipFamily;
  private meowfsCache: MeowFsCache | null = null;
  private supportsSeamlessWake = false;

  resetState(): void {
    this.meowfsCache = null;
    this.supportsSeamlessWake = false;
  }

  getOptionalOperations(transport: CodecTransport<DataView>): HidOptionalOperations {
    return {
      getRgbConfig: () => this.getRgbConfig(transport),
      setRgbConfig: (config) => this.setRgbConfig(transport, config),
      getFnKeyConfig: () => this.getFnKeyConfig(transport),
      setFnKeyConfig: (config) => this.setFnKeyConfig(transport, config),
      getOsMode: () => this.getOsMode(transport),
      setOsMode: (config) => this.setOsMode(transport, config),
      saveConfig: () => this.runOkCommand(transport, Command.CFG_SAVE, 'CFG_SAVE'),
      loadConfig: () => this.runOkCommand(transport, Command.CFG_LOAD, 'CFG_LOAD'),
      resetConfig: () => this.runOkCommand(transport, Command.CFG_RESET, 'CFG_RESET'),
      getBattery: () => this.getBattery(transport),
      getLogConfig: () => this.getLogConfig(transport),
      setLogConfig: (config) => this.setLogConfig(transport, config),
      getMacroOverview: () => this.getMacroOverview(transport),
      getMacroInfo: (slot) => this.getMacroInfo(transport, slot),
      getMacroData: (slot) => this.getMacroData(transport, slot),
      setMacroData: (slot, macro) => this.setMacroData(transport, slot, macro),
      deleteMacro: (slot) => this.deleteMacro(transport, slot),
    };
  }

  buildCommandFrame(cmd: Command, sub = 0, data: Uint8Array = new Uint8Array(0)): Uint8Array {
    if (data.length > FRAME_SIZE - 3) {
      throw new Error(`命令数据过长: ${data.length} > ${FRAME_SIZE - 3}`);
    }

    const frame = new Uint8Array(FRAME_SIZE);
    frame[0] = cmd;
    frame[1] = sub;
    frame[2] = data.length;
    frame.set(data, 3);
    return frame;
  }

  describeOutgoingFrame(frame: Uint8Array): TerminalEntryDraft {
    const parsed = parseSendFrame(frame);
    return {
      direction: 'send',
      level: 'info',
      command: parsed.command,
      cmdHex: parsed.cmdHex,
      sub: parsed.sub,
      dataLen: parsed.dataLen,
      rawHex: parsed.rawHex,
      parsed: parsed.parsed,
    };
  }

  parseIncomingPacket(frame: Uint8Array): CodecInboundPacket<DataView> {
    if (frame[0] === Command.LOG) {
      const parsed = parseLogFrame(frame);
      return {
        kind: 'event',
        entry: {
          direction: 'device',
          level: 'info',
          command: parsed.command,
          cmdHex: parsed.cmdHex,
          sub: parsed.sub,
          dataLen: parsed.dataLen,
          rawHex: parsed.rawHex,
          parsed: parsed.parsed,
          category: parsed.category,
        },
      };
    }

    const parsed = parseReceiveFrame(frame);
    return {
      kind: 'response',
      entry: {
        direction: 'receive',
        level: parsed.isError ? 'error' : 'success',
        command: parsed.command,
        cmdHex: parsed.cmdHex,
        sub: parsed.sub,
        dataLen: parsed.dataLen,
        rawHex: parsed.rawHex,
        parsed: parsed.parsed,
        statusCode: parsed.statusCode,
      },
      response: new DataView(frame.buffer, frame.byteOffset, frame.byteLength),
    };
  }

  parseSysInfo(resp: DataView): DeviceInfo {
    const d = this.expectOk(resp, 'SYS_INFO');
    return {
      vendorId: (resp.getUint8(d + 1) << 8) | resp.getUint8(d + 2),
      productId: (resp.getUint8(d + 3) << 8) | resp.getUint8(d + 4),
      chipFamily: this.chipFamily,
      versionMajor: resp.getUint8(d + 5),
      versionMinor: resp.getUint8(d + 6),
      versionPatch: resp.getUint8(d + 7),
      maxLayers: resp.getUint8(d + 8),
      maxKeys: resp.getUint8(d + 9),
      macroSlots: resp.getUint8(d + 10),
      keyboardType: resp.getUint8(d + 11),
      actualKeyCount: resp.getUint8(d + 12),
      fnKeyCount: resp.getUint8(d + 13),
      protocol: this.protocol,
      protocolLabel: this.protocolLabel,
      capabilities: this.capabilities,
    };
  }

  parseSysStatus(resp: DataView): DeviceStatus {
    const d = this.expectOk(resp, 'SYS_STATUS');
    const status: DeviceStatus = {
      workMode: resp.getUint8(d + 1),
      connectionState: resp.getUint8(d + 2),
      currentLayer: resp.getUint8(d + 3),
      batteryLevel: resp.getUint8(d + 4),
      isCharging: resp.getUint8(d + 5) !== 0,
    };

    if (resp.getUint8(2) >= 9) {
      status.adcRaw = resp.getUint16(d + 6, true);
      status.chargePinRaw = resp.getUint8(d + 8);
    }

    return status;
  }

  parseKeymap(resp: DataView): { numLayers: number; currentLayer: number; defaultLayer: number; layer: LayerConfig } {
    const d = this.expectOk(resp, 'KEYMAP_GET');
    const numLayers = resp.getUint8(d + 1);
    const currentLayer = resp.getUint8(d + 2);
    const defaultLayer = resp.getUint8(d + 3);
    const keys: KeyAction[] = [];

    for (let i = 0; i < MAX_KEYS; i++) {
      const offset = d + 4 + i * 4;
      keys.push({
        type: resp.getUint8(offset),
        modifier: resp.getUint8(offset + 1),
        param1: resp.getUint8(offset + 2),
        param2: resp.getUint8(offset + 3),
      });
    }

    return { numLayers, currentLayer, defaultLayer, layer: { keys } };
  }

  buildSetKeymapPayload(numLayers: number, defaultLayer: number, layer: LayerConfig): Uint8Array {
    const data = new Uint8Array(35);
    data[0] = numLayers;
    data[1] = 0;
    data[2] = defaultLayer;

    for (let i = 0; i < MAX_KEYS; i++) {
      const key = layer.keys[i] || createEmptyAction();
      const offset = 3 + i * 4;
      data[offset] = key.type;
      data[offset + 1] = key.modifier;
      data[offset + 2] = key.param1;
      data[offset + 3] = key.param2;
    }

    return data;
  }

  parseRgbConfig(resp: DataView): RgbConfig {
    const d = this.expectOk(resp, 'RGB_GET');
    this.supportsSeamlessWake = resp.getUint8(2) >= 14;
    return {
      enabled: resp.getUint8(d + 1) !== 0,
      mode: resp.getUint8(d + 2),
      brightness: resp.getUint8(d + 3),
      speed: resp.getUint8(d + 4),
      colorR: resp.getUint8(d + 5),
      colorG: resp.getUint8(d + 6),
      colorB: resp.getUint8(d + 7),
      indicatorEnabled: resp.getUint8(d + 8) !== 0,
      indicatorBrightness: resp.getUint8(d + 9),
      pressEffect: resp.getUint8(d + 10),
      lightSleepMin: resp.getUint8(d + 11),
      deepSleepMin: resp.getUint8(d + 12),
      seamlessWakeEnabled: this.supportsSeamlessWake
        ? resp.getUint8(d + 13) !== 0
        : true,
    };
  }

  buildSetRgbPayload(config: RgbConfig): Uint8Array {
    const data = new Uint8Array(this.supportsSeamlessWake ? 13 : 12);
    data[0] = config.enabled ? 1 : 0;
    data[1] = config.mode;
    data[2] = config.brightness;
    data[3] = config.speed;
    data[4] = config.colorR;
    data[5] = config.colorG;
    data[6] = config.colorB;
    data[7] = config.indicatorEnabled ? 1 : 0;
    data[8] = config.indicatorBrightness;
    data[9] = config.pressEffect;
    data[10] = config.lightSleepMin!;
    data[11] = config.deepSleepMin!;
    if (this.supportsSeamlessWake) {
      data[12] = config.seamlessWakeEnabled === false ? 0 : 1;
    }
    return data;
  }

  parseFnKeyConfig(resp: DataView): FnKeyConfig {
    const d = this.expectOk(resp, 'FNKEY_GET');
    const fnKeys: FnKeyEntry[] = [];

    for (let i = 0; i < MAX_FN_KEYS; i++) {
      const offset = d + 1 + i * 8;
      fnKeys.push({
        clickAction: resp.getUint8(offset),
        clickParam: resp.getUint8(offset + 1),
        longAction: resp.getUint8(offset + 2),
        longParam: resp.getUint8(offset + 3),
        longPressMs: resp.getUint16(offset + 4, true),
      });
    }

    return { fnKeys };
  }

  buildSetFnKeyPayload(config: FnKeyConfig): Uint8Array {
    const data = new Uint8Array(32);
    for (let i = 0; i < MAX_FN_KEYS; i++) {
      const fn = config.fnKeys[i];
      const offset = i * 8;
      data[offset] = fn.clickAction;
      data[offset + 1] = fn.clickParam;
      data[offset + 2] = fn.longAction;
      data[offset + 3] = fn.longParam;
      data[offset + 4] = fn.longPressMs & 0xff;
      data[offset + 5] = (fn.longPressMs >> 8) & 0xff;
    }
    return data;
  }

  parseBatteryInfo(resp: DataView): BatteryInfo {
    const d = this.expectOk(resp, 'BATTERY');
    const info: BatteryInfo = {
      level: resp.getUint8(d + 1),
      isCharging: resp.getUint8(d + 2) !== 0,
      voltage: resp.getUint16(d + 3, true) / 1000,
    };

    if (resp.getUint8(2) >= 8) {
      info.adcRaw = resp.getUint16(d + 5, true);
      info.chargePinRaw = resp.getUint8(d + 7);
    }

    return info;
  }

  parseLogConfig(resp: DataView): LogConfig {
    const d = this.expectOk(resp, 'LOG_GET');
    return {
      enabled: resp.getUint8(d + 1) !== 0,
    };
  }

  buildSetLogConfigPayload(config: LogConfig): Uint8Array {
    return new Uint8Array([config.enabled ? 1 : 0]);
  }

  parseOsModeConfig(resp: DataView): OsModeConfig {
    const d = this.expectOk(resp, 'CFG_OS_GET');
    const mode = resp.getUint8(d + 1) === OsMode.MAC ? OsMode.MAC : OsMode.WIN;
    return { mode };
  }

  buildSetOsModePayload(config: OsModeConfig): Uint8Array {
    return new Uint8Array([config.mode === OsMode.MAC ? OsMode.MAC : OsMode.WIN]);
  }

  expectOk(resp: DataView, commandName: string): number {
    const status = resp.getUint8(RESP_HEADER_SIZE);
    if (status !== ResponseCode.OK) {
      throw new Error(`${commandName} 失败: 0x${status.toString(16)}`);
    }
    return RESP_HEADER_SIZE;
  }

  buildFullKeymap(firstLayer: { numLayers: number; currentLayer: number; defaultLayer: number; layer: LayerConfig }, layers: LayerConfig[]): KeymapConfig {
    return {
      numLayers: firstLayer.numLayers,
      currentLayer: firstLayer.currentLayer,
      defaultLayer: firstLayer.defaultLayer,
      layers,
    };
  }

  async getSysInfo(transport: CodecTransport<DataView>): Promise<DeviceInfo> {
    const resp = await this.sendCommand(transport, Command.SYS_INFO);
    return this.parseSysInfo(resp);
  }

  async getSysStatus(transport: CodecTransport<DataView>): Promise<DeviceStatus> {
    const resp = await this.sendCommand(transport, Command.SYS_STATUS);
    return this.parseSysStatus(resp);
  }

  async getFullKeymap(transport: CodecTransport<DataView>): Promise<KeymapConfig> {
    const first = await this.getKeymap(transport, 0);
    const layers = createEmptyKeymap().layers;
    layers[0] = first.layer;

    for (let i = 1; i < first.numLayers; i++) {
      const result = await this.getKeymap(transport, i);
      layers[i] = result.layer;
    }

    return this.buildFullKeymap(first, layers);
  }

  async setFullKeymap(transport: CodecTransport<DataView>, config: KeymapConfig): Promise<void> {
    for (let i = 0; i < config.numLayers; i++) {
      const data = this.buildSetKeymapPayload(config.numLayers, config.defaultLayer, config.layers[i]);
      const resp = await this.sendCommand(transport, Command.KEYMAP_SET, i, data);
      this.expectOk(resp, 'KEYMAP_SET');
    }
  }

  private async sendCommand(
    transport: CodecTransport<DataView>,
    cmd: Command,
    sub = 0,
    data: Uint8Array = new Uint8Array(0),
    timeout = 3000,
  ): Promise<DataView> {
    return transport.sendAndWait(this.buildCommandFrame(cmd, sub, data), {
      timeout,
      timeoutLabel: '命令响应超时',
    });
  }

  private async getKeymap(
    transport: CodecTransport<DataView>,
    layerIndex: number,
  ): Promise<{ numLayers: number; currentLayer: number; defaultLayer: number; layer: KeymapConfig['layers'][number] }> {
    const resp = await this.sendCommand(transport, Command.KEYMAP_GET, layerIndex);
    return this.parseKeymap(resp);
  }

  private async getRgbConfig(transport: CodecTransport<DataView>): Promise<RgbConfig> {
    const resp = await this.sendCommand(transport, Command.RGB_GET);
    return this.parseRgbConfig(resp);
  }

  private async setRgbConfig(transport: CodecTransport<DataView>, config: RgbConfig): Promise<void> {
    const resp = await this.sendCommand(transport, Command.RGB_SET, 0, this.buildSetRgbPayload(config));
    this.expectOk(resp, 'RGB_SET');
  }

  private async getFnKeyConfig(transport: CodecTransport<DataView>): Promise<FnKeyConfig> {
    const resp = await this.sendCommand(transport, Command.FNKEY_GET);
    return this.parseFnKeyConfig(resp);
  }

  private async setFnKeyConfig(transport: CodecTransport<DataView>, config: FnKeyConfig): Promise<void> {
    const resp = await this.sendCommand(transport, Command.FNKEY_SET, 0, this.buildSetFnKeyPayload(config));
    this.expectOk(resp, 'FNKEY_SET');
  }

  private async getOsMode(transport: CodecTransport<DataView>): Promise<OsModeConfig> {
    const resp = await this.sendCommand(transport, Command.CFG_OS_GET);
    return this.parseOsModeConfig(resp);
  }

  private async setOsMode(transport: CodecTransport<DataView>, config: OsModeConfig): Promise<void> {
    const resp = await this.sendCommand(transport, Command.CFG_OS_SET, 0, this.buildSetOsModePayload(config));
    this.expectOk(resp, 'CFG_OS_SET');
  }

  private async getBattery(transport: CodecTransport<DataView>): Promise<BatteryInfo> {
    const resp = await this.sendCommand(transport, Command.BATTERY);
    return this.parseBatteryInfo(resp);
  }

  private async getLogConfig(transport: CodecTransport<DataView>): Promise<LogConfig> {
    const resp = await this.sendCommand(transport, Command.LOG_GET);
    return this.parseLogConfig(resp);
  }

  private async setLogConfig(transport: CodecTransport<DataView>, config: LogConfig): Promise<void> {
    const resp = await this.sendCommand(transport, Command.LOG_SET, 0, this.buildSetLogConfigPayload(config));
    this.expectOk(resp, 'LOG_SET');
  }

  private async runOkCommand(
    transport: CodecTransport<DataView>,
    cmd: Command,
    commandName: string,
  ): Promise<void> {
    const resp = await this.sendCommand(transport, cmd);
    this.expectOk(resp, commandName);
  }

  // ==========================================================================
  // 宏操作
  // ==========================================================================

  private async getMacroOverview(transport: CodecTransport<DataView>): Promise<MacroOverview> {
    this.meowfsCache = null;
    const cache = await this.ensureMeowFsCache(transport);
    const count = cache.macros.length;
    const totalSlots = count + CH592_MEOWFS_APPEND_SLOTS;
    return {
      totalSlots,
      usedCount: count,
      slotValid: [
        ...new Array(count).fill(true),
        ...new Array(totalSlots - count).fill(false),
      ],
      dynamic: true,
      fsTotal: cache.fsTotal,
      fsFree: cache.fsFree,
    };
  }

  private async getMacroInfo(transport: CodecTransport<DataView>, slot: number): Promise<MacroHeader> {
    this.validateMacroIndex(slot);
    const cache = await this.ensureMeowFsCache(transport);
    if (slot >= cache.macros.length) {
      return { valid: 0, id: slot, actionCount: 0, dataSize: 0, name: '' };
    }

    const macro = cache.macros[slot];
    return {
      valid: 1,
      id: slot,
      actionCount: macro.actionCount,
      dataSize: macro.actionCount * CH592_MEOWFS_ACTION_SIZE,
      name: '',
    };
  }

  private async getMacroData(transport: CodecTransport<DataView>, slot: number): Promise<MacroData> {
    this.validateMacroIndex(slot);
    const cache = await this.ensureMeowFsCache(transport);
    if (slot >= cache.macros.length) {
      return {
        header: { valid: 0, id: slot, actionCount: 0, dataSize: 0, name: '' },
        actions: [{ type: MacroActionType.END, param: 0 }],
      };
    }

    const macro = cache.macros[slot];
    const actions = macro.actions.map((action) => ({ ...action }));
    actions.push({ type: MacroActionType.END, param: 0 });
    return {
      header: {
        valid: 1,
        id: slot,
        actionCount: macro.actionCount,
        dataSize: macro.actionCount * CH592_MEOWFS_ACTION_SIZE,
        name: '',
      },
      actions,
    };
  }

  private async setMacroData(
    transport: CodecTransport<DataView>,
    slot: number,
    macro: MacroData,
  ): Promise<void> {
    this.validateMacroIndex(slot);
    const cache = await this.ensureMeowFsCache(transport);
    const macros = cache.macros.map((entry) => ({
      actionCount: entry.actionCount,
      actions: entry.actions.map((action) => ({ ...action })),
    }));

    const actionsNoEnd = macro.actions.filter((action) => action.type !== MacroActionType.END);
    if (actionsNoEnd.length > CH592_MEOWFS_MAX_ACTIONS) {
      throw new Error(`动作数 ${actionsNoEnd.length} 超过上限 ${CH592_MEOWFS_MAX_ACTIONS}`);
    }

    const newEntry: MeowFsMacroEntry = {
      actionCount: actionsNoEnd.length,
      actions: actionsNoEnd,
    };

    if (slot < macros.length) {
      macros[slot] = newEntry;
    } else if (slot === macros.length) {
      macros.push(newEntry);
    } else {
      throw new Error(`无效的宏索引 ${slot}`);
    }

    const serialized = this.serializeMacros(macros);
    if (serialized.length > cache.fsTotal) {
      throw new Error(`宏数据总计 ${serialized.length} 字节，超过 MeowFS 容量 ${cache.fsTotal} 字节`);
    }

    await this.eraseAllFs(transport);
    await this.writeFsChunked(transport, 0, serialized);
    this.meowfsCache = null;
  }

  private async deleteMacro(transport: CodecTransport<DataView>, slot: number): Promise<void> {
    this.validateMacroIndex(slot);
    const cache = await this.ensureMeowFsCache(transport);
    if (slot >= cache.macros.length) {
      throw new Error(`宏索引 ${slot} 不存在`);
    }
    const resp = await this.sendCommand(transport, Command.MACRO_DEL, slot);
    this.expectOk(resp, 'MACRO_DEL');
    this.meowfsCache = null;
  }

  private validateMacroIndex(slot: number): void {
    if (!Number.isInteger(slot) || slot < 0) {
      throw new Error(`无效的宏索引 ${slot}`);
    }
  }

  private async readFsChunked(
    transport: CodecTransport<DataView>,
    offset: number,
    length: number,
  ): Promise<Uint8Array> {
    const result = new Uint8Array(length);
    let pos = 0;

    while (pos < length) {
      const chunkLen = Math.min(CH592_MEOWFS_READ_CHUNK, length - pos);
      const absOffset = offset + pos;
      const resp = await this.sendCommand(
        transport,
        Command.MACRO_GET,
        0,
        new Uint8Array([
          (absOffset >> 8) & 0xff,
          absOffset & 0xff,
          chunkLen,
        ]),
      );
      const d = this.expectOk(resp, 'MACRO_GET');
      const readLen = resp.getUint8(d + 1);
      if (readLen === 0 || readLen > chunkLen || d + 2 + readLen > resp.byteLength) {
        throw new Error(`MACRO_GET 返回长度无效: ${readLen}/${chunkLen}`);
      }
      for (let i = 0; i < readLen; i++) {
        result[pos + i] = resp.getUint8(d + 2 + i);
      }
      pos += readLen;
    }

    return result;
  }

  private async writeFsChunked(
    transport: CodecTransport<DataView>,
    offset: number,
    data: Uint8Array,
  ): Promise<void> {
    let pos = 0;

    while (pos < data.length) {
      const chunkLen = Math.min(CH592_MEOWFS_WRITE_CHUNK, data.length - pos);
      const absOffset = offset + pos;
      const payload = new Uint8Array(3 + chunkLen);
      payload[0] = (absOffset >> 8) & 0xff;
      payload[1] = absOffset & 0xff;
      payload[2] = chunkLen;
      payload.set(data.subarray(pos, pos + chunkLen), 3);

      const resp = await this.sendCommand(
        transport,
        Command.MACRO_SET,
        Ch592MacroSetSub.WRITE,
        payload,
      );
      this.expectOk(resp, 'MACRO_SET WRITE');
      pos += chunkLen;
    }
  }

  private async eraseAllFs(transport: CodecTransport<DataView>): Promise<void> {
    const resp = await this.sendCommand(
      transport,
      Command.MACRO_SET,
      Ch592MacroSetSub.ERASE,
      new Uint8Array([0xff]),
    );
    this.expectOk(resp, 'MACRO_SET ERASE');
  }

  private parseFsData(raw: Uint8Array): MeowFsMacroEntry[] {
    const macros: MeowFsMacroEntry[] = [];
    let pos = 0;

    while (pos + CH592_MEOWFS_HEADER_SIZE <= raw.length) {
      const marker = raw[pos];
      if (marker === 0xff) {
        break;
      }

      const actionCount = raw[pos + 1];
      const entrySize =
        CH592_MEOWFS_HEADER_SIZE + actionCount * CH592_MEOWFS_ACTION_SIZE;
      if (pos + entrySize > raw.length) {
        break;
      }

      if (marker === 0xaa) {
        const actions: MacroAction[] = [];
        for (let i = 0; i < actionCount; i++) {
          actions.push({
            type: raw[pos + CH592_MEOWFS_HEADER_SIZE + i * 2] as MacroActionType,
            param: raw[pos + CH592_MEOWFS_HEADER_SIZE + i * 2 + 1],
          });
        }
        macros.push({ actionCount, actions });
      }

      pos += entrySize;
    }

    return macros;
  }

  private serializeMacros(macros: MeowFsMacroEntry[]): Uint8Array {
    let totalSize = 0;
    for (const macro of macros) {
      totalSize +=
        CH592_MEOWFS_HEADER_SIZE +
        macro.actionCount * CH592_MEOWFS_ACTION_SIZE;
    }

    const buf = new Uint8Array(totalSize);
    let pos = 0;
    for (const macro of macros) {
      buf[pos] = 0xaa;
      buf[pos + 1] = macro.actionCount;
      for (let i = 0; i < macro.actionCount; i++) {
        buf[pos + CH592_MEOWFS_HEADER_SIZE + i * 2] = macro.actions[i].type;
        buf[pos + CH592_MEOWFS_HEADER_SIZE + i * 2 + 1] = macro.actions[i].param;
      }
      pos +=
        CH592_MEOWFS_HEADER_SIZE +
        macro.actionCount * CH592_MEOWFS_ACTION_SIZE;
    }

    return buf;
  }

  private async ensureMeowFsCache(
    transport: CodecTransport<DataView>,
  ): Promise<MeowFsCache> {
    if (this.meowfsCache) {
      return this.meowfsCache;
    }

    const resp = await this.sendCommand(transport, Command.MACRO_INFO, 0);
    const d = this.expectOk(resp, 'MACRO_INFO');
    const fsTotal = resp.getUint16(d + 1, false);
    const pageSize = resp.getUint16(d + 3, false);
    const macroCount = resp.getUint8(d + 5);
    const fsFree = resp.getUint16(d + 6, false);
    if (fsTotal === 0 || fsFree > fsTotal || pageSize === 0 || pageSize > fsTotal) {
      throw new Error(`MACRO_INFO 文件系统元数据无效: total=${fsTotal}, free=${fsFree}, page=${pageSize}`);
    }
    const usedBytes = fsTotal - fsFree;
    if (macroCount * CH592_MEOWFS_HEADER_SIZE > usedBytes) {
      throw new Error(`MACRO_INFO 宏数量与已用空间不一致: ${macroCount}/${usedBytes}`);
    }

    let macros: MeowFsMacroEntry[] = [];
    if (macroCount > 0 && usedBytes > 0) {
      const raw = await this.readFsChunked(transport, 0, usedBytes);
      macros = this.parseFsData(raw);
      if (macros.length !== macroCount) {
        throw new Error(`MeowFS 宏目录损坏: expected=${macroCount}, actual=${macros.length}`);
      }
    }

    this.meowfsCache = { fsTotal, fsFree, pageSize, macros };
    return this.meowfsCache;
  }
}
