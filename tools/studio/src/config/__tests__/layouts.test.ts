import { describe, expect, it } from 'vitest';

import { LAYOUT_KNOB } from '../layouts';

describe('KNOB layout protocol indices', () => {
  it('matches the CH592F CW, CCW, and click slots', () => {
    const indexByType = Object.fromEntries(
      LAYOUT_KNOB.keys.map((key) => [key.type, key.index]),
    );

    expect(indexByType['encoder-cw']).toBe(4);
    expect(indexByType['encoder-ccw']).toBe(5);
    expect(indexByType['encoder-press']).toBe(6);
  });
});
