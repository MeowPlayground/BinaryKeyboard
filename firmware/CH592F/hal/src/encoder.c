#include "encoder.h"

#if defined(KBD_LAYOUT_KNOB) && defined(KBD_HAS_ENCODER)

#include <string.h>

#ifndef ENCODER_QUEUE_SIZE
#define ENCODER_QUEUE_SIZE 16
#endif

#define STATIC_ASSERT(cond, msg) typedef char static_assert_##msg[(cond) ? 1 : -1]
STATIC_ASSERT((ENCODER_QUEUE_SIZE & (ENCODER_QUEUE_SIZE - 1)) == 0,
              encoder_queue_must_be_power_of_2);

typedef struct {
  volatile uint32_t tick_ms;
  uint8_t prev_ab;
  int8_t accum;
} encoder_ctx_t;

static const kbd_key_pin_t g_encoder_a_pin = {KBD_ENCODER_A_PORT, KBD_ENCODER_A_PIN};
static const kbd_key_pin_t g_encoder_b_pin = {KBD_ENCODER_B_PORT, KBD_ENCODER_B_PIN};

static volatile key_event_t s_encoder_queue[ENCODER_QUEUE_SIZE];
static volatile uint8_t s_encoder_wr = 0;
static volatile uint8_t s_encoder_rd = 0;
static volatile encoder_ctx_t s_encoder_ctx;

static inline void ConfigPinInputPullup(const kbd_key_pin_t *pin) {
  if (pin->port == GPIO_PORT_A) {
    GPIOA_ModeCfg(pin->pin, GPIO_ModeIN_PU);
  } else {
    GPIOB_ModeCfg(pin->pin, GPIO_ModeIN_PU);
  }
}

static inline void ConfigPinFallEdge(const kbd_key_pin_t *pin) {
  if (pin->port == GPIO_PORT_A) {
    GPIOA_ITModeCfg(pin->pin, GPIO_ITMode_FallEdge);
  } else {
    GPIOB_ITModeCfg(pin->pin, GPIO_ITMode_FallEdge);
  }
}

static inline void ConfigPinRiseEdge(const kbd_key_pin_t *pin) {
  if (pin->port == GPIO_PORT_A) {
    GPIOA_ITModeCfg(pin->pin, GPIO_ITMode_RiseEdge);
  } else {
    GPIOB_ITModeCfg(pin->pin, GPIO_ITMode_RiseEdge);
  }
}

static inline uint32_t ReadPortItFlags(gpio_port_t port) {
  return (port == GPIO_PORT_A) ? GPIOA_ReadITFlagPort() : GPIOB_ReadITFlagPort();
}

static inline void ClearPinItFlag(gpio_port_t port, uint32_t pin) {
  if (port == GPIO_PORT_A) {
    GPIOA_ClearITFlagBit(pin);
  } else {
    GPIOB_ClearITFlagBit(pin);
  }
}

static inline void EnablePinIrq(gpio_port_t port, uint32_t pin) {
  if (port == GPIO_PORT_A) {
    R16_PA_INT_EN |= (uint16_t)pin;
  } else {
    R16_PB_INT_EN |= (uint16_t)pin;
  }
}

static inline uint8_t ReadPinLevel(const kbd_key_pin_t *pin) {
  uint32_t v = (pin->port == GPIO_PORT_A) ? GPIOA_ReadPortPin(pin->pin)
                                          : GPIOB_ReadPortPin(pin->pin);
  return (v != 0) ? 1u : 0u;
}

static inline uint8_t ReadEncoderState(void) {
  return (uint8_t)((ReadPinLevel(&g_encoder_a_pin) << 1) |
                   ReadPinLevel(&g_encoder_b_pin));
}

static void ConfigEncoderEdgesFromCurrentLevel(void) {
  if (ReadPinLevel(&g_encoder_a_pin) == 0) {
    ConfigPinRiseEdge(&g_encoder_a_pin);
  } else {
    ConfigPinFallEdge(&g_encoder_a_pin);
  }

  if (ReadPinLevel(&g_encoder_b_pin) == 0) {
    ConfigPinRiseEdge(&g_encoder_b_pin);
  } else {
    ConfigPinFallEdge(&g_encoder_b_pin);
  }
}

static inline uint8_t EncoderQueueFreeSlots(void) {
  return (uint8_t)((s_encoder_rd - s_encoder_wr - 1u) & (ENCODER_QUEUE_SIZE - 1u));
}

static inline void PushEncoderEventUnchecked(uint8_t key,
                                             uint8_t type,
                                             uint32_t tick_ms) {
  s_encoder_queue[s_encoder_wr].key = key;
  s_encoder_queue[s_encoder_wr].type = type;
  s_encoder_queue[s_encoder_wr].tick_ms = tick_ms;
  s_encoder_wr = (uint8_t)((s_encoder_wr + 1u) & (ENCODER_QUEUE_SIZE - 1u));
}

static void PushEncoderTap(uint8_t key, uint32_t tick_ms) {
  if (EncoderQueueFreeSlots() < 2u) {
    return;
  }

  PushEncoderEventUnchecked(key, KEY_EVT_PRESS, tick_ms);
  PushEncoderEventUnchecked(key, KEY_EVT_RELEASE, tick_ms);
}

static void ProcessEncoderTransition(uint32_t tick_ms) {
  static const int8_t transition_table[16] = {
      0,  1, -1, 0,
     -1,  0,  0, 1,
      1,  0,  0, -1,
      0, -1,  1, 0,
  };

  uint8_t current_ab = ReadEncoderState();
  uint8_t index = (uint8_t)((s_encoder_ctx.prev_ab << 2) | current_ab);
  int8_t delta = transition_table[index];

  s_encoder_ctx.prev_ab = current_ab;

  if (delta == 0) {
    return;
  }

  s_encoder_ctx.accum += delta;
  if (s_encoder_ctx.accum >= 4) {
    PushEncoderTap(KBD_KNOB_CW_IDX, tick_ms);
    s_encoder_ctx.accum = 0;
  } else if (s_encoder_ctx.accum <= -4) {
    PushEncoderTap(KBD_KNOB_CCW_IDX, tick_ms);
    s_encoder_ctx.accum = 0;
  }
}

void Encoder_Init(void) {
  memset((void *)&s_encoder_ctx, 0, sizeof(s_encoder_ctx));
  s_encoder_rd = 0;
  s_encoder_wr = 0;

  ConfigPinInputPullup(&g_encoder_a_pin);
  ConfigPinInputPullup(&g_encoder_b_pin);

  s_encoder_ctx.prev_ab = ReadEncoderState();
  ConfigEncoderEdgesFromCurrentLevel();

  ClearPinItFlag(g_encoder_a_pin.port, g_encoder_a_pin.pin);
  ClearPinItFlag(g_encoder_b_pin.port, g_encoder_b_pin.pin);
  EnablePinIrq(g_encoder_a_pin.port, g_encoder_a_pin.pin);
  EnablePinIrq(g_encoder_b_pin.port, g_encoder_b_pin.pin);
}

uint8_t Encoder_GetEvent(key_event_t *evt) {
  if (evt == NULL) {
    return 0;
  }
  if (s_encoder_rd == s_encoder_wr) {
    return 0;
  }
  *evt = s_encoder_queue[s_encoder_rd];
  s_encoder_rd = (uint8_t)((s_encoder_rd + 1) & (ENCODER_QUEUE_SIZE - 1));
  return 1;
}

uint8_t Encoder_HandlePortIrq(gpio_port_t port) {
  if (port != g_encoder_a_pin.port && port != g_encoder_b_pin.port) {
    return 0;
  }

  uint16_t enabled = (port == GPIO_PORT_A) ? R16_PA_INT_EN : R16_PB_INT_EN;
  uint32_t interested = 0;
  if (g_encoder_a_pin.port == port) {
    interested |= g_encoder_a_pin.pin;
  }
  if (g_encoder_b_pin.port == port) {
    interested |= g_encoder_b_pin.pin;
  }

  uint32_t flags = ReadPortItFlags(port) & enabled & interested;
  if (flags == 0) {
    return 0;
  }

  if (flags & g_encoder_a_pin.pin) {
    ClearPinItFlag(g_encoder_a_pin.port, g_encoder_a_pin.pin);
  }
  if (flags & g_encoder_b_pin.pin) {
    ClearPinItFlag(g_encoder_b_pin.port, g_encoder_b_pin.pin);
  }

  ProcessEncoderTransition(s_encoder_ctx.tick_ms);
  ConfigEncoderEdgesFromCurrentLevel();
  return 1;
}

void Encoder_TimerTick1ms(void) {
  s_encoder_ctx.tick_ms++;
}

void Encoder_EnterSleep(void) {}

void Encoder_ExitSleep(void) {
  s_encoder_ctx.prev_ab = ReadEncoderState();
  s_encoder_ctx.accum = 0;
  ConfigEncoderEdgesFromCurrentLevel();
  ClearPinItFlag(g_encoder_a_pin.port, g_encoder_a_pin.pin);
  ClearPinItFlag(g_encoder_b_pin.port, g_encoder_b_pin.pin);
  EnablePinIrq(g_encoder_a_pin.port, g_encoder_a_pin.pin);
  EnablePinIrq(g_encoder_b_pin.port, g_encoder_b_pin.pin);
}

#else

void Encoder_Init(void) {}
uint8_t Encoder_GetEvent(key_event_t *evt) {
  (void)evt;
  return 0;
}
uint8_t Encoder_HandlePortIrq(gpio_port_t port) {
  (void)port;
  return 0;
}
void Encoder_TimerTick1ms(void) {}
void Encoder_EnterSleep(void) {}
void Encoder_ExitSleep(void) {}

#endif
