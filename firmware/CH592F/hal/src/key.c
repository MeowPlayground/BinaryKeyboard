/**
 * @file    key.c
 * @brief   键盘按键驱动实现（GPIO 边沿中断 + 1ms 定时器锁定去抖 + 事件队列）
 *
 * @details
 * 一个“**中断驱动**”的按键模块，核心目标：
 * 1) 低 CPU 占用：按键变化由 GPIO 边沿中断触发，无需周期扫描；
 * 2) 去抖可靠：采用“锁定去抖（lockout）”——每次边沿后禁用该引脚中断一段时间；
 * 3) 上层易用：ISR 中只做轻量处理并将事件写入环形队列，上层轮询出队消费。
 *
 * ---------------------------------------------------------------------------
 * 1. 信号假设（硬件/电平）
 * ---------------------------------------------------------------------------
 * - 全部按键均配置为“上拉输入”：
 *     - 松开：GPIO 读为 1
 *     - 按下：GPIO 被拉低读为 0  （Active-Low）
 *
 * ---------------------------------------------------------------------------
 * 2. 数据流（从硬件到应用层）
 * ---------------------------------------------------------------------------
 * @verbatim
 *
 *                      +--------------------------------------------+
 *                      | GPIO input (Pull-up, Active-Low)           |
 *                      +----------------------+---------------------+
 *                                             | level change (falling / rising edge IRQ)
 *                                             |
 *                      +----------------------v---------------------+
 *                      | GPIOA / GPIOB_IRQHandler()                  |
 *                      |  - read port interrupt flags                |
 *                      |  - filter by enabled mask                   |
 *                      |  - map pin -> ctx                           |
 *                      +----------------------+---------------------+
 *                                             |
 *                                             | call
 *                      +----------------------v---------------------+
 *                      | HandleNormalKeyEdge(idx)                    |
 *                      |  - check lock_ms                            |
 *                      |  - decide press/release by expect edge      |
 *                      |  - PushKeyEvent() into ring queue           |
 *                      |  - configure next edge                      |
 *                      |  - set lock_ms and DisablePinIrq()          |
 *                      +----------------------+---------------------+
 *                                             |
 *                                             | OR (FN key)
 *                      +----------------------v---------------------+
 *                      | HandleFnKeyEdge(id)                         |
 *                      |  - press  : store press_tick                |
 *                      |  - release: dur = now - press_tick          |
 *                      |             -> enqueue CLICK / LONG         |
 *                      |  - set lock_ms and DisablePinIrq()          |
 *                      +----------------------+---------------------+
 *                                             |
 *                           (debounce unlock handled by 1ms timer)
 *                                             |
 *                      +----------------------v---------------------+
 *                      | TMR0_IRQHandler() (1ms)                     |
 *                      |  - s_tick_ms++                              |
 *                      |  - iterate all key/fn ctx                   |
 *                      |      lock_ms-- -> when reaches 0:           |
 *                      |        ClearPinIntFlag()                    |
 *                      |        EnablePinIrq()                       |
 *                      +----------------------+---------------------+
 *                                             |
 *                                             | polled by application
 *                                             v
 *                      +--------------------------------------------+
 *                      | Key_GetEvent() / FnKey_GetEvent()           |
 *                      |  - dequeue from ring buffer                 |
 *                      +--------------------------------------------+
 *
 * @endverbatim
 *
 * ---------------------------------------------------------------------------
 * 3. 状态机（边沿 + expect）
 * ---------------------------------------------------------------------------
 * 普通键（Normal key）：
 * - expect = 0：期待“按下下降沿”（fall edge）
 * - expect = 1：期待“松开上升沿”（rise edge）
 *
 * @verbatim
 * Normal key（KEY_ENABLE_RELEASE_EVENT=1）
 *
 *     expect=0(等下降沿) ──fall──▶ 记录PRESS, is_down=1, expect=1, 配置上升沿
 *              ▲                                           │
 *              │                                           rise
 *              └────────────── 记录RELEASE, is_down=0, expect=0, 配置下降沿 ◀──
 *
 * 每次进入边沿处理都会：
 *   - lock_ms = KEY_LOCKOUT_MS
 *   - DisablePinIrq() 进入锁定
 *   - 1ms 定时器递减 lock_ms，归零后再 EnablePinIrq()
 * @endverbatim
 *
 * FN 键（点击/长按）：
 * - 按下沿：只记录 press_tick，不入队
 * - 松开沿：计算 dur 并入队 CLICK / LONG
 *
 * @verbatim
 * Fn key
 *   expect=0(等按下fall) ──fall──▶ press_tick=now, is_down=1, expect=1, 配置上升沿
 *              ▲                                           │
 *              │                                           rise
 *              └────── dur=now-press_tick -> CLICK/LONG 入队, is_down=0, expect=0, 配置下降沿
 * @endverbatim
 *
 * ---------------------------------------------------------------------------
 * 4. 环形队列（ring buffer）与丢弃策略
 * ---------------------------------------------------------------------------
 * - KEY_QUEUE_SIZE / FNKEY_QUEUE_SIZE 必须为 2 的幂（用 & 掩码实现回绕）
 * - 写指针 next == 读指针 表示队列满：当前实现“直接丢弃新事件”
 *
 * ---------------------------------------------------------------------------
 * 5. 并发与时序注意事项
 * ---------------------------------------------------------------------------
 * - Push 在 ISR 中执行；GetEvent 在主循环执行（单写者/单读者模型）
 * - 索引与队列元素声明为 volatile，避免编译器优化导致的可见性问题
 * - 若平台对 8-bit 读写并非原子，需在 GetEvent 时临界区保护（此处保持你的原实现不改）
 */

#include "key.h"
#include "kbd_config.h"
#include "kbd_mode.h"
#include "encoder.h"

#include <string.h>

/* ============================================================================
 * Compile-time checks
 * ============================================================================
 */

/**
 * @brief 编译期静态断言（用于约束队列长度必须为 2 的幂等）。
 * @note 触发断言时会产生负数组长度导致编译失败。
 */
#define STATIC_ASSERT(cond, msg) typedef char static_assert_##msg[(cond) ? 1 : -1]

/** @brief 普通按键队列大小必须为 2 的幂。 */
STATIC_ASSERT((KEY_QUEUE_SIZE & (KEY_QUEUE_SIZE - 1)) == 0, key_queue_must_be_power_of_2);
/** @brief FN 按键队列大小必须为 2 的幂。 */
STATIC_ASSERT((FNKEY_QUEUE_SIZE & (FNKEY_QUEUE_SIZE - 1)) == 0, fn_queue_must_be_power_of_2);

/* ============================================================================
 * Pin mapping
 * ============================================================================
 */

/**
 * @brief 普通按键引脚表。
 * @details
 * 该表由板级宏（kbd_config.h）决定具体端口与引脚。
 * 根据不同键盘型号定义不同的引脚映射。
 */
#if defined(KBD_LAYOUT_5KEY)
/*---------------------------------------------------------------------------*/
/* 五键款: 5 键                                                               */
/*---------------------------------------------------------------------------*/
#define KBD_SCAN_KEY_COUNT KBD_NUM_KEYS
static const kbd_key_pin_t g_key_pins[KBD_SCAN_KEY_COUNT] = {
    {KBD_K1_PORT, KBD_K1_PIN},
    {KBD_K2_PORT, KBD_K2_PIN},
    {KBD_K3_PORT, KBD_K3_PIN},
    {KBD_K4_PORT, KBD_K4_PIN},
    {KBD_K5_PORT, KBD_K5_PIN},
};
static const uint8_t g_key_logical_ids[KBD_SCAN_KEY_COUNT] = {0, 1, 2, 3, 4};

#elif defined(KBD_LAYOUT_KNOB)
/*---------------------------------------------------------------------------*/
/* 旋钮款: 4 主键 + 旋钮按下 (旋转由独立编码器模块处理)                         */
/*---------------------------------------------------------------------------*/
#define KBD_SCAN_KEY_COUNT 5u
static const kbd_key_pin_t g_key_pins[KBD_SCAN_KEY_COUNT] = {
    {KBD_K1_PORT, KBD_K1_PIN},
    {KBD_K2_PORT, KBD_K2_PIN},
    {KBD_K3_PORT, KBD_K3_PIN},
    {KBD_K4_PORT, KBD_K4_PIN},
    {KBD_ENCODER_BTN_PORT, KBD_ENCODER_BTN_PIN},
};
static const uint8_t g_key_logical_ids[KBD_SCAN_KEY_COUNT] = {0, 1, 2, 3, KBD_KNOB_CLICK_IDX};

#else
#error "请通过 CMake 或 MRS 预处理宏选择一个键盘型号"
#endif

/**
 * @brief FN 按键引脚表（FN1/FN2...）。
 */
static const kbd_key_pin_t g_fn_pins[KBD_FN_NUM_KEYS] = {
    {KBD_FN1_PORT, KBD_FN1_PIN},
    {KBD_FN2_PORT, KBD_FN2_PIN},
};

/**
 * @brief BOOT 键引脚（仅原始读取，不使用中断）。
 */
static const kbd_key_pin_t g_boot_pin = {KBD_FN_BOOT_PORT, KBD_FN_BOOT_PIN};

/* ============================================================================
 * Queues (ring buffers)
 * ============================================================================
 */

/**
 * @brief 普通按键事件队列（环形缓冲）。
 * @note
 * - Push: ISR
 * - Pop : 主循环
 * - 满时丢弃新 PRESS；RELEASE 会淘汰最旧事件后入队
 */
static volatile key_event_t s_key_queue[KEY_QUEUE_SIZE];
/** @brief 普通按键队列写指针。 */
static volatile uint8_t s_key_wr = 0;
/** @brief 普通按键队列读指针。 */
static volatile uint8_t s_key_rd = 0;
/** DEEP 唤醒后等待安全 HID 同步的标志。 */
static volatile uint8_t s_deep_wake_recovery_pending = 0;
/** HID ready must remain stable briefly before accepting fresh input. */
static volatile uint32_t s_deep_wake_ready_tick = 0;
static volatile uint8_t s_deep_wake_sync_pending = 0;

#define DEEP_WAKE_HID_SETTLE_MS 200u

/**
 * @brief FN 按键事件队列（环形缓冲）。
 */
static volatile fnkey_event_t s_fn_queue[FNKEY_QUEUE_SIZE];
/** @brief FN 队列写指针。 */
static volatile uint8_t s_fn_wr = 0;
/** @brief FN 队列读指针。 */
static volatile uint8_t s_fn_rd = 0;

/* ============================================================================
 * Driver tick (1ms) for timestamps and lockout countdown
 * ============================================================================
 */

/**
 * @brief 1ms tick 计数（在 TMR0_IRQHandler 中自增）。
 * @details
 * 用途：
 * - 事件时间戳 tick_ms
 * - lockout 倒计时的时间基准（每次中断减 1）
 */
static volatile uint32_t s_tick_ms = 0;

/**
 * @brief 获取当前毫秒 tick。
 * @return 自 Key_Init() 起累计的毫秒数。
 */
static inline uint32_t GetTickMs(void) { return s_tick_ms; }

/* ============================================================================
 * Contexts
 * ============================================================================
 */

/**
 * @brief 普通按键上下文（边沿 + 锁定去抖）。
 */
typedef struct
{
    volatile uint16_t lock_ms; /**< 锁定倒计时（ms）。>0 表示锁定中。 */
    volatile uint8_t is_down;  /**< 当前是否按下（1=按下，0=松开）。 */
    volatile uint8_t expect;   /**< 期待的边沿：0=按下(下降沿)，1=松开(上升沿)。 */
    volatile uint8_t suppress_action; /**< 当前唤醒按键是否仅用于唤醒。 */
} key_ctx_t;

/** @brief 普通按键上下文数组（每个键一个 ctx）。 */
static volatile key_ctx_t s_key_ctx[KBD_SCAN_KEY_COUNT];

/**
 * @brief FN 按键上下文（边沿 + 锁定去抖 + 按下时间戳）。
 * @details
 * FN 键达到阈值时产生 LONG；在阈值前松开时产生 CLICK。
 */
typedef struct
{
    volatile uint16_t lock_ms;    /**< 锁定倒计时（ms）。 */
    volatile uint8_t is_down;     /**< 当前是否按下（1=按下，0=松开）。 */
    volatile uint8_t expect;      /**< 期待边沿：0=按下(下降沿)，1=松开(上升沿)。 */
    volatile uint8_t long_sent;   /**< LONG 是否已经在阈值到达时发送。 */
    volatile uint8_t suppress_action; /**< 当前唤醒动作是否仅用于唤醒。 */
    volatile uint16_t long_press_ms; /**< 当前 FN 键的长按阈值。 */
    volatile uint32_t press_tick; /**< 按下边沿发生时的 tick（ms）。 */
} fn_ctx_t;

/** @brief FN 按键上下文数组。 */
static volatile fn_ctx_t s_fn_ctx[KBD_FN_NUM_KEYS];

/* ============================================================================
 * GPIO helpers
 * ============================================================================
 */

/**
 * @brief 将指定引脚配置为上拉输入。
 * @param pin 引脚描述（端口 + pin 位）。
 */
static inline void ConfigPinInputPullup(const kbd_key_pin_t *pin)
{
    if (pin->port == GPIO_PORT_A)
        GPIOA_ModeCfg(pin->pin, GPIO_ModeIN_PU);
    else
        GPIOB_ModeCfg(pin->pin, GPIO_ModeIN_PU);
}

/**
 * @brief 将指定引脚配置为下降沿触发中断（按下沿，Active-Low）。
 * @param pin 引脚描述。
 */
static inline void ConfigPinFallEdge(const kbd_key_pin_t *pin)
{
    if (pin->port == GPIO_PORT_A)
        GPIOA_ITModeCfg(pin->pin, GPIO_ITMode_FallEdge);
    else
        GPIOB_ITModeCfg(pin->pin, GPIO_ITMode_FallEdge);
}

/**
 * @brief 将指定引脚配置为上升沿触发中断（松开沿）。
 * @param pin 引脚描述。
 */
static inline void ConfigPinRiseEdge(const kbd_key_pin_t *pin)
{
    if (pin->port == GPIO_PORT_A)
        GPIOA_ITModeCfg(pin->pin, GPIO_ITMode_RiseEdge);
    else
        GPIOB_ITModeCfg(pin->pin, GPIO_ITMode_RiseEdge);
}

/**
 * @brief 读取端口中断标志寄存器（平台相关封装）。
 * @param port GPIO_PORT_A / GPIO_PORT_B
 * @return 对应端口的中断标志位集合。
 */
static inline uint32_t ReadPortItFlags(gpio_port_t port)
{
    return (port == GPIO_PORT_A) ? GPIOA_ReadITFlagPort() : GPIOB_ReadITFlagPort();
}

/**
 * @brief 清除指定端口的某个引脚中断标志位。
 * @param port 端口
 * @param pin  引脚位（bitmask）
 */
static inline void ClearPinItFlag(gpio_port_t port, uint32_t pin)
{
    if (port == GPIO_PORT_A)
        GPIOA_ClearITFlagBit(pin);
    else
        GPIOB_ClearITFlagBit(pin);
}

/**
 * @brief 使能指定端口的某个引脚中断。
 * @param port 端口
 * @param pin  引脚位（bitmask）
 */
static inline void EnablePinIrq(gpio_port_t port, uint32_t pin)
{
    if (port == GPIO_PORT_A)
        R16_PA_INT_EN |= (uint16_t)pin;
    else
        R16_PB_INT_EN |= (uint16_t)pin;
}

/**
 * @brief 禁用指定端口的某个引脚中断（用于 lockout 去抖）。
 * @param port 端口
 * @param pin  引脚位（bitmask）
 */
static inline void DisablePinIrq(gpio_port_t port, uint32_t pin)
{
    if (port == GPIO_PORT_A)
        R16_PA_INT_EN &= (uint16_t)(~pin);
    else
        R16_PB_INT_EN &= (uint16_t)(~pin);
}

/**
 * @brief 读取引脚电平（上拉输入）：按下=0，松开=1。
 * @param pin 引脚描述。
 * @return 1 表示高电平（松开），0 表示低电平（按下）。
 * @note Active-Low 语义：pressed=0, released=1
 */
static inline uint8_t ReadPinLevel(const kbd_key_pin_t *pin)
{
    uint32_t v = (pin->port == GPIO_PORT_A) ? GPIOA_ReadPortPin(pin->pin)
                                            : GPIOB_ReadPortPin(pin->pin);
    return (v != 0) ? 1 : 0; /* 或 return !!v; */
}

/* ============================================================================
 * Queue push
 * ============================================================================
 */

/**
 * @brief 向普通按键队列写入一个事件（ISR 内调用）。
 * @param key     按键索引
 * @param type    事件类型（KEY_EVT_PRESS/KEY_EVT_RELEASE）
 * @param tick_ms 时间戳
 * @note 队列满时优先保留 RELEASE，宁可丢一次 PRESS 也不能让主机卡键。
 */
static inline void PushKeyEvent(uint8_t key, uint8_t type, uint32_t tick_ms)
{
    uint8_t next = (uint8_t)((s_key_wr + 1) & (KEY_QUEUE_SIZE - 1));
    if (next == s_key_rd)
    {
        if (type != KEY_EVT_RELEASE)
            return;
        s_key_rd = (uint8_t)((s_key_rd + 1) & (KEY_QUEUE_SIZE - 1));
    }
    s_key_queue[s_key_wr].key = key;
    s_key_queue[s_key_wr].type = type;
    s_key_queue[s_key_wr].tick_ms = tick_ms;
    s_key_wr = next;
}

/**
 * @brief 向 FN 队列写入一个事件（ISR 内调用）。
 * @param id      FN 键索引
 * @param type    事件类型（FNKEY_EVT_CLICK/FNKEY_EVT_LONG）
 * @param tick_ms 时间戳
 * @note 队列满时丢弃新事件。
 */
static inline void PushFnEvent(uint8_t id, uint8_t type, uint32_t tick_ms)
{
    uint8_t next = (uint8_t)((s_fn_wr + 1) & (FNKEY_QUEUE_SIZE - 1));
    if (next == s_fn_rd)
        return;
    s_fn_queue[s_fn_wr].id = id;
    s_fn_queue[s_fn_wr].type = type;
    s_fn_queue[s_fn_wr].tick_ms = tick_ms;
    s_fn_wr = next;
}

/* ============================================================================
 * Timer (TMR0 1ms)
 * ============================================================================
 */

/**
 * @brief 1ms 定时器是否已启动。
 */
static volatile uint8_t s_timer_active = 0;

/**
 * @brief 启动 1ms 定时器（TMR0）。
 * @details
 * 用于：
 * - 提供 s_tick_ms 时间戳
 * - 驱动 key/fn 的 lock_ms 倒计时并在归零时重新使能引脚中断
 */
static inline void StartTimer1ms(void)
{
    if (s_timer_active)
        return;

    TMR0_TimerInit(FREQ_SYS / 1000); /* 1ms */
    TMR0_ITCfg(ENABLE, TMR0_3_IT_CYC_END);
    PFIC_SetPriority(TMR0_IRQn, TIMER_IRQ_PRIORITY);
    PFIC_EnableIRQ(TMR0_IRQn);

    s_timer_active = 1;
}

/**
 * @brief 停止 1ms 定时器（TMR0）。
 */
static inline void StopTimer1ms(void)
{
    TMR0_ITCfg(DISABLE, TMR0_3_IT_CYC_END);
    PFIC_DisableIRQ(TMR0_IRQn);
    TMR0_ClearITFlag(TMR0_3_IT_CYC_END);
    s_timer_active = 0;
}

/* ============================================================================
 * Handlers
 * ============================================================================
 */

/**
 * @brief 处理普通按键的边沿中断（press/release + lockout）。
 * @param idx 扫描按键索引
 * @details
 * - lock_ms>0：忽略（仍处于锁定去抖）
 * - expect==0：认为是按下边沿，推送 PRESS，必要时切换到 rise edge 等 release
 * - expect==1：认为是松开边沿，推送 RELEASE，并切回 fall edge 等下一次 press
 * - 处理结束后：
 *     - lock_ms = KEY_LOCKOUT_MS
 *     - DisablePinIrq()（锁定期间不再响应该引脚中断）
 */
static inline void HandleNormalKeyEdge(uint8_t idx)
{
    const kbd_key_pin_t *pin = &g_key_pins[idx];
    uint8_t logical_key = g_key_logical_ids[idx];
    if (s_key_ctx[idx].lock_ms > 0)
        return;

    uint32_t now = GetTickMs();

    if (s_key_ctx[idx].expect == 0)
    {
        s_key_ctx[idx].is_down = 1;
        if (s_deep_wake_recovery_pending)
        {
            s_key_ctx[idx].suppress_action = 1;
        }
        if (!s_key_ctx[idx].suppress_action)
        {
            PushKeyEvent(logical_key, KEY_EVT_PRESS, now);
        }

#if KEY_ENABLE_RELEASE_EVENT
        s_key_ctx[idx].expect = 1;
        ConfigPinRiseEdge(pin);
#else
        ConfigPinFallEdge(pin);
#endif
    }
    else
    {
        s_key_ctx[idx].is_down = 0;
        if (!s_key_ctx[idx].suppress_action)
        {
            PushKeyEvent(logical_key, KEY_EVT_RELEASE, now);
        }
        s_key_ctx[idx].suppress_action = 0;
        s_key_ctx[idx].expect = 0;
        ConfigPinFallEdge(pin);
    }

    s_key_ctx[idx].lock_ms = KEY_LOCKOUT_MS;
    DisablePinIrq(pin->port, pin->pin);
}

/**
 * @brief 处理 FN 按键的边沿中断（press 记录 tick，release 产生 click + lockout）。
 * @param id FN 键索引（0..KBD_FN_NUM_KEYS-1）
 * @details
 * - 按下沿：记录 press_tick，并配置上升沿等待 release
 * - 松开沿：若 LONG 尚未发送则产生 CLICK，否则只恢复等待下一次按下
 * - 每次边沿后都进入 lockout：禁用引脚中断，等待 1ms 定时器解锁
 */
static inline void HandleFnKeyEdge(uint8_t id)
{
    const kbd_key_pin_t *pin = &g_fn_pins[id];
    if (s_fn_ctx[id].lock_ms > 0)
        return;

    uint32_t now = GetTickMs();

    if (s_fn_ctx[id].expect == 0)
    {
        /* Press edge. */
        s_fn_ctx[id].is_down = 1;
        if (s_deep_wake_recovery_pending)
        {
            s_fn_ctx[id].suppress_action = 1;
        }
        s_fn_ctx[id].long_sent = 0;
        s_fn_ctx[id].press_tick = now;
        s_fn_ctx[id].expect = 1;
        ConfigPinRiseEdge(pin);
    }
    else
    {
        /* Release edge -> emit CLICK only if LONG was not already emitted. */
        s_fn_ctx[id].is_down = 0;
        s_fn_ctx[id].expect = 0;
        ConfigPinFallEdge(pin);

        if (!s_fn_ctx[id].long_sent && !s_fn_ctx[id].suppress_action)
        {
            PushFnEvent(id, FNKEY_EVT_CLICK, now);
        }
        s_fn_ctx[id].suppress_action = 0;
    }

    s_fn_ctx[id].lock_ms = FN_LOCKOUT_MS;
    DisablePinIrq(pin->port, pin->pin);
}

/* ============================================================================
 * GPIO IRQ dispatch (normal + FN)
 * ============================================================================
 */

/**
 * @brief GPIO 端口中断分发（同时覆盖普通键 + FN 键）。
 * @param port GPIO_PORT_A / GPIO_PORT_B
 * @details
 * 过程：
 * 1) 读取端口中断 flags
 * 2) 与 “当前 enable 掩码” 相与，屏蔽已 lockout 的 pin
 * 3) 遍历 g_key_pins / g_fn_pins，找到置位的 pin：
 *    - ClearPinItFlag()
 *    - 调用对应 HandleXxxEdge()
 */
static inline void HandlePortIrq(gpio_port_t port)
{
    uint32_t flags = ReadPortItFlags(port);
    uint16_t enabled = (port == GPIO_PORT_A) ? R16_PA_INT_EN : R16_PB_INT_EN;
    flags &= enabled;
    if (flags == 0)
        return;

    uint8_t waking = KBD_Mode_IsInSleep() ? 1u : 0u;
    uint8_t suppress_wake_action =
        (waking && !KBD_Mode_IsSeamlessWakeEnabled()) ? 1u : 0u;

    for (uint8_t i = 0; i < KBD_SCAN_KEY_COUNT && flags; i++)
    {
        if (g_key_pins[i].port != port)
            continue;
        uint32_t pin = g_key_pins[i].pin;
        if (flags & pin)
        {
            uint8_t level = ReadPinLevel(&g_key_pins[i]);
            uint8_t valid_edge = (s_key_ctx[i].expect == 0) ?
                                     (level == 0) : (level != 0);
            if (!valid_edge)
            {
                ClearPinItFlag(port, pin);
                flags &= ~pin;
                continue;
            }
            if (waking)
            {
                KBD_Mode_RequestWake();
            }
            if (suppress_wake_action && s_key_ctx[i].expect == 0)
            {
                s_key_ctx[i].suppress_action = 1;
            }
            ClearPinItFlag(port, pin);
            flags &= ~pin;
            HandleNormalKeyEdge(i);
        }
    }

    for (uint8_t id = 0; id < KBD_FN_NUM_KEYS && flags; id++)
    {
        if (g_fn_pins[id].port != port)
            continue;
        uint32_t pin = g_fn_pins[id].pin;
        if (flags & pin)
        {
            uint8_t level = ReadPinLevel(&g_fn_pins[id]);
            uint8_t valid_edge = (s_fn_ctx[id].expect == 0) ?
                                     (level == 0) : (level != 0);
            if (!valid_edge)
            {
                ClearPinItFlag(port, pin);
                flags &= ~pin;
                continue;
            }
            if (waking)
            {
                KBD_Mode_RequestWake();
            }
            if (suppress_wake_action && s_fn_ctx[id].expect == 0)
            {
                s_fn_ctx[id].suppress_action = 1;
            }
            ClearPinItFlag(port, pin);
            flags &= ~pin;
            HandleFnKeyEdge(id);
        }
    }

    if (Encoder_HandlePortIrq(port) && waking)
    {
        KBD_Mode_RequestWake();
    }
}

/**
 * @brief GPIOA 端口中断服务函数。
 */
__INTERRUPT __HIGH_CODE void GPIOA_IRQHandler(void) { HandlePortIrq(GPIO_PORT_A); }

/**
 * @brief GPIOB 端口中断服务函数。
 */
__INTERRUPT __HIGH_CODE void GPIOB_IRQHandler(void) { HandlePortIrq(GPIO_PORT_B); }

/* ============================================================================
 * TMR0 IRQ: tick + unlock
 * ============================================================================
 */

/**
 * @brief TMR0 中断服务函数：1ms tick + FN 长按触发 + lockout 倒计时。
 * @details
 * - 每 1ms：
 *   - s_tick_ms++
 *   - 遍历所有普通键 ctx：若 lock_ms>0 则 lock_ms--，到 0 时清 flag 并 EnablePinIrq()
 *   - 遍历所有 FN 键 ctx：同理，并在长按阈值到达时排队 LONG
 */
__INTERRUPT __HIGH_CODE void TMR0_IRQHandler(void)
{
    if (!TMR0_GetITFlag(TMR0_3_IT_CYC_END))
        return;
    TMR0_ClearITFlag(TMR0_3_IT_CYC_END);

    s_tick_ms++;

    for (uint8_t i = 0; i < KBD_SCAN_KEY_COUNT; i++)
    {
        uint16_t lm = s_key_ctx[i].lock_ms;
        if (lm == 0)
            continue;
        lm--;
        s_key_ctx[i].lock_ms = lm;
        if (lm == 0)
        {
            gpio_port_t port = g_key_pins[i].port;
            uint32_t pin = g_key_pins[i].pin;
            ClearPinItFlag(port, pin);
            uint8_t level = ReadPinLevel(&g_key_pins[i]);
            uint8_t missed_edge = (s_key_ctx[i].expect == 0) ?
                                      (level == 0) : (level != 0);
            if (missed_edge)
            {
                HandleNormalKeyEdge(i);
            }
            else
            {
                EnablePinIrq(port, pin);
            }
        }
    }

    for (uint8_t id = 0; id < KBD_FN_NUM_KEYS; id++)
    {
        if (s_fn_ctx[id].is_down && !s_fn_ctx[id].long_sent &&
            !s_fn_ctx[id].suppress_action &&
            (uint32_t)(s_tick_ms - s_fn_ctx[id].press_tick) >=
                (uint32_t)s_fn_ctx[id].long_press_ms)
        {
            s_fn_ctx[id].long_sent = 1;
            PushFnEvent(id, FNKEY_EVT_LONG, s_tick_ms);
        }

        uint16_t lm = s_fn_ctx[id].lock_ms;
        if (lm == 0)
            continue;
        lm--;
        s_fn_ctx[id].lock_ms = lm;
        if (lm == 0)
        {
            gpio_port_t port = g_fn_pins[id].port;
            uint32_t pin = g_fn_pins[id].pin;
            ClearPinItFlag(port, pin);
            uint8_t level = ReadPinLevel(&g_fn_pins[id]);
            uint8_t missed_edge = (s_fn_ctx[id].expect == 0) ?
                                      (level == 0) : (level != 0);
            if (missed_edge)
            {
                HandleFnKeyEdge(id);
            }
            else
            {
                EnablePinIrq(port, pin);
            }
        }
    }

    Encoder_TimerTick1ms();
}

/* ============================================================================
 * Public API
 * ============================================================================
 */

/**
 * @brief 从普通按键队列获取一个事件（出队）。
 * @param[out] evt 事件输出
 * @return 1=成功；0=无事件或参数无效
 */
uint8_t Key_GetEvent(key_event_t *evt)
{
    if (evt == NULL)
        return 0;
    if (s_key_rd == s_key_wr)
        return 0;
    *evt = s_key_queue[s_key_rd];
    s_key_rd = (uint8_t)((s_key_rd + 1) & (KEY_QUEUE_SIZE - 1));
    return 1;
}

/**
 * @brief 从 FN 队列获取一个事件（出队）。
 * @param[out] evt 事件输出
 * @return 1=成功；0=无事件或参数无效
 */
uint8_t FnKey_GetEvent(fnkey_event_t *evt)
{
    if (evt == NULL)
        return 0;
    if (s_fn_rd == s_fn_wr)
        return 0;
    *evt = s_fn_queue[s_fn_rd];
    s_fn_rd = (uint8_t)((s_fn_rd + 1) & (FNKEY_QUEUE_SIZE - 1));
    return 1;
}

void Key_BeginDeepWakeRecovery(uint32_t gpioa_flags, uint32_t gpiob_flags)
{
    (void)gpioa_flags;
    (void)gpiob_flags;
    s_deep_wake_recovery_pending = 1;
    s_deep_wake_ready_tick = 0;
    s_deep_wake_sync_pending = 0;

    for (uint8_t i = 0; i < KBD_SCAN_KEY_COUNT; i++)
    {
        if (s_key_ctx[i].is_down)
        {
            s_key_ctx[i].suppress_action = 1;
        }
    }
    for (uint8_t id = 0; id < KBD_FN_NUM_KEYS; id++)
    {
        if (s_fn_ctx[id].is_down)
        {
            s_fn_ctx[id].suppress_action = 1;
        }
    }
}

void Key_ServiceDeepWakeKeys(uint8_t transport_ready)
{
    if (!s_deep_wake_recovery_pending)
    {
        return;
    }

    if (!transport_ready)
    {
        s_deep_wake_ready_tick = 0;
        return;
    }

    if (s_deep_wake_ready_tick == 0u)
    {
        s_deep_wake_ready_tick = GetTickMs();
        return;
    }

    if ((uint32_t)(GetTickMs() - s_deep_wake_ready_tick) >=
        DEEP_WAKE_HID_SETTLE_MS)
    {
        s_deep_wake_recovery_pending = 0;
        s_deep_wake_ready_tick = 0;
        s_deep_wake_sync_pending = 1;
    }
}

uint8_t Key_IsDeepWakeRecoveryPending(void)
{
    return s_deep_wake_recovery_pending;
}

uint8_t Key_IsDeepWakeSyncPending(void)
{
    return s_deep_wake_sync_pending;
}

void Key_FinishDeepWakeSync(void)
{
    s_deep_wake_sync_pending = 0;
}

void FnKey_SetLongPressThreshold(uint8_t id, uint16_t threshold_ms)
{
    if (id >= KBD_FN_NUM_KEYS)
        return;

    s_fn_ctx[id].long_press_ms =
        (threshold_ms == 0u) ? (uint16_t)FN_LONG_PRESS_MS : threshold_ms;
}

/**
 * @brief 查询普通按键是否按下。
 * @param key_index 按键索引
 * @return 1=按下；0=松开；-1=非法索引
 */
int8_t Key_IsDown(uint8_t key_index)
{
    for (uint8_t i = 0; i < KBD_SCAN_KEY_COUNT; i++)
    {
        if (g_key_logical_ids[i] == key_index)
        {
            return s_key_ctx[i].is_down ? 1 : 0;
        }
    }
    return -1;
}

/**
 * @brief 查询 FN 按键是否按下。
 * @param id FN 键索引
 * @return 1=按下；0=松开；-1=非法索引
 */
int8_t FnKey_IsDown(uint8_t id)
{
    if (id >= KBD_FN_NUM_KEYS)
        return -1;
    return s_fn_ctx[id].is_down ? 1 : 0;
}

/**
 * @brief BOOT 键是否按下（原始读取，Active-Low）。
 * @return 1=按下；0=松开
 */
int8_t BootKey_IsPressed(void)
{
    /* Active-low raw read. */
    return (ReadPinLevel(&g_boot_pin) == 0) ? 1 : 0;
}

/**
 * @brief 初始化按键驱动（见 key.h 文档）。
 */
void Key_Init(void)
{
    memset((void *)s_key_ctx, 0, sizeof(s_key_ctx));
    memset((void *)s_fn_ctx, 0, sizeof(s_fn_ctx));

    s_key_rd = s_key_wr = 0;
    s_fn_rd = s_fn_wr = 0;
    s_tick_ms = 0;
    s_deep_wake_recovery_pending = 0;
    s_deep_wake_ready_tick = 0;
    s_deep_wake_sync_pending = 0;

    /* BOOT: input only. */
    ConfigPinInputPullup(&g_boot_pin);

    /* Normal keys: input + falling/rising edge depends on current level. */
    for (uint8_t i = 0; i < KBD_SCAN_KEY_COUNT; i++)
    {
        const kbd_key_pin_t *pin = &g_key_pins[i];
        ConfigPinInputPullup(pin);

        uint8_t level = ReadPinLevel(pin);
        s_key_ctx[i].is_down = (level == 0) ? 1 : 0;
        s_key_ctx[i].lock_ms = 0;

        /* 若初始化时已按下(level=0)，下一次应期待 release(上升沿)；否则期待 press(下降沿) */
        s_key_ctx[i].expect = (level == 0) ? 1 : 0;
        if (level == 0)
            ConfigPinRiseEdge(pin);
        else
            ConfigPinFallEdge(pin);

        ClearPinItFlag(pin->port, pin->pin);
        EnablePinIrq(pin->port, pin->pin);
    }

    /* FN keys: initialize the expected edge from the actual pin level. */
    for (uint8_t id = 0; id < KBD_FN_NUM_KEYS; id++)
    {
        const kbd_key_pin_t *pin = &g_fn_pins[id];
        ConfigPinInputPullup(pin);

        /* 可选：读两次/丢一次，等上拉稳定 */
        (void)ReadPinLevel(pin);
        uint8_t level = ReadPinLevel(pin);

        s_fn_ctx[id].is_down = (level == 0) ? 1 : 0;
        s_fn_ctx[id].lock_ms = 0;
        s_fn_ctx[id].long_sent = (level == 0) ? 1 : 0;
        s_fn_ctx[id].suppress_action = (level == 0) ? 1 : 0;
        s_fn_ctx[id].long_press_ms = FN_LONG_PRESS_MS;

        s_fn_ctx[id].expect = (level == 0) ? 1 : 0;
        s_fn_ctx[id].press_tick = 0;
        if (level == 0)
            ConfigPinRiseEdge(pin);
        else
            ConfigPinFallEdge(pin);

        ClearPinItFlag(pin->port, pin->pin);
        EnablePinIrq(pin->port, pin->pin);
    }

    PFIC_SetPriority(GPIO_A_IRQn, KEY_IRQ_PRIORITY);
    PFIC_SetPriority(GPIO_B_IRQn, KEY_IRQ_PRIORITY);
    PFIC_EnableIRQ(GPIO_A_IRQn);
    PFIC_EnableIRQ(GPIO_B_IRQn);

    StartTimer1ms();
}

/**
 * @brief 进入低功耗：停止 1ms 定时器，解除所有 lockout 并重新使能引脚 IRQ。
 * @note 1ms 定时器停止后 lock_ms 无法递减，若不强制解锁会导致处于锁定中的键无法唤醒系统。
 *       30s+ 空闲进入休眠时硬件上不存在抖动，可安全强制解锁。
 */
void Key_EnterSleep(void)
{
    Encoder_EnterSleep();

    for (uint8_t i = 0; i < KBD_SCAN_KEY_COUNT; i++)
    {
        const kbd_key_pin_t *pin = &g_key_pins[i];
        s_key_ctx[i].lock_ms = 0;
        ClearPinItFlag(pin->port, pin->pin);
        EnablePinIrq(pin->port, pin->pin);
    }
    for (uint8_t id = 0; id < KBD_FN_NUM_KEYS; id++)
    {
        const kbd_key_pin_t *pin = &g_fn_pins[id];
        s_fn_ctx[id].lock_ms = 0;
        ClearPinItFlag(pin->port, pin->pin);
        EnablePinIrq(pin->port, pin->pin);
    }

    if (s_timer_active)
        StopTimer1ms();

    /* BLE LIGHT uses LowPower_Sleep between radio events. Keep GPIO as a
     * wake source so a key press wakes immediately instead of waiting for
     * the next connection event. */
    PWR_PeriphWakeUpCfg(ENABLE, RB_SLP_GPIO_WAKE, Long_Delay);
}

/**
 * @brief 退出低功耗：启动 1ms 定时器。
 */
void Key_ExitSleep(void)
{
    PWR_PeriphWakeUpCfg(DISABLE, RB_SLP_GPIO_WAKE, Long_Delay);
    Encoder_ExitSleep();
    if (!s_timer_active)
        StartTimer1ms();
}

/**
 * @brief 配置深度休眠唤醒：所有按键引脚切换到低电平触发 + 打开 GPIO 唤醒源。
 * @note 调用前应保证按键已松开（否则进入休眠瞬间立即唤醒）。
 *       深休眠使用 LowPower_Shutdown() 时，唤醒等价于复位，事件状态不会保留。
 */
void Key_ConfigDeepSleepWakeup(void)
{
    for (uint8_t i = 0; i < KBD_SCAN_KEY_COUNT; i++)
    {
        const kbd_key_pin_t *pin = &g_key_pins[i];
        if (pin->port == GPIO_PORT_A)
            GPIOA_ITModeCfg(pin->pin, GPIO_ITMode_LowLevel);
        else
            GPIOB_ITModeCfg(pin->pin, GPIO_ITMode_LowLevel);
        ClearPinItFlag(pin->port, pin->pin);
        EnablePinIrq(pin->port, pin->pin);
    }
    for (uint8_t id = 0; id < KBD_FN_NUM_KEYS; id++)
    {
        const kbd_key_pin_t *pin = &g_fn_pins[id];
        if (pin->port == GPIO_PORT_A)
            GPIOA_ITModeCfg(pin->pin, GPIO_ITMode_LowLevel);
        else
            GPIOB_ITModeCfg(pin->pin, GPIO_ITMode_LowLevel);
        ClearPinItFlag(pin->port, pin->pin);
        EnablePinIrq(pin->port, pin->pin);
    }

    PWR_PeriphWakeUpCfg(ENABLE, RB_SLP_GPIO_WAKE, Long_Delay);
}
