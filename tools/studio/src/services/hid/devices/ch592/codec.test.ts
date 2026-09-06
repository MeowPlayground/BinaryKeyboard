import { describe, expect, it } from 'vitest';
import { Ch592Codec } from './codec';
import { createDefaultRgbConfig } from '@/types/protocol';

function rgbResponse(payloadLength: number, seamlessWake = 1): DataView {
  const bytes = new Uint8Array(64);
  bytes[2] = payloadLength;
  bytes[3] = 0;
  bytes[4] = 1;
  bytes[5] = 5;
  bytes[6] = 64;
  bytes[7] = 128;
  bytes[8] = 1;
  bytes[9] = 2;
  bytes[10] = 3;
  bytes[11] = 1;
  bytes[12] = 64;
  bytes[13] = 0;
  bytes[14] = 1;
  bytes[15] = 1;
  bytes[16] = seamlessWake;
  return new DataView(bytes.buffer);
}

describe('CH592 seamless wake RGB protocol extension', () => {
  it('reads and writes the 13th setting byte with new firmware', () => {
    const codec = new Ch592Codec();
    const config = codec.parseRgbConfig(rgbResponse(14, 0));

    expect(config.seamlessWakeEnabled).toBe(false);
    config.seamlessWakeEnabled = true;
    const payload = codec.buildSetRgbPayload(config);
    expect(payload).toHaveLength(13);
    expect(payload[12]).toBe(1);
  });

  it('keeps the legacy 12-byte request for old firmware', () => {
    const codec = new Ch592Codec();
    const config = codec.parseRgbConfig(rgbResponse(13));

    expect(config.seamlessWakeEnabled).toBe(true);
    config.seamlessWakeEnabled = false;
    expect(codec.buildSetRgbPayload(config)).toHaveLength(12);
  });

  it('defaults seamless wake to enabled before connecting', () => {
    expect(createDefaultRgbConfig().seamlessWakeEnabled).toBe(true);
  });
});
