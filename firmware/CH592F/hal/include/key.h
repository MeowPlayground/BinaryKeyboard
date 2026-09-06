/**
 * @file    key.h
 * @brief   键盘按键驱动接口（GPIO 边沿中断 + 1ms 定时器锁定去抖 + 事件队列）
 * @details
 * 本模块将按键输入抽象为“事件队列”：
 * - 普通按键（K1..Kn）：产生 PRESS / RELEASE 事件（是否上报 RELEASE 可配置）
 * - FN 按键（FN1..FNn）：短按在松开时产生 CLICK，长按达到阈值时立即产生 LONG 事件
 * - BOOT 键：仅提供原始电平读取（不使用中断）
 *
 * 典型使用：
 * @code
 * Key_Init();
 * while (1) {
 *     key_event_t e;
 *     if (Key_GetEvent(&e)) {
 *         // 处理普通按键事件
 *     }
 *
 *     fnkey_event_t fe;
 *     if (FnKey_GetEvent(&fe)) {
 *         // 处理 FN CLICK/LONG
 *     }
 * }
 * @endcode
 *
 * @note
 * - GPIO 输入为上拉模式：按下=0，松开=1（Active-Low）
 * - 事件队列满时优先保留 RELEASE，避免主机卡在按下状态
 */

#ifndef KBD_KEY_H_
#define KBD_KEY_H_

#include <stdint.h>
#include "kbd_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup KBD_KEY 键盘按键驱动（key）
 * @brief GPIO 边沿中断 + 1ms 定时器锁定去抖 + 环形队列上报事件
 * @{
 */

/* ============================================================================
 * Configuration
 * ============================================================================
 */

/**
 * @def KEY_LOCKOUT_MS
 * @brief 普通按键锁定去抖时间（毫秒）。
 * @details
 * 每次边沿中断触发后，该按键会进入 lockout 状态：
 * - 禁用该引脚中断
 * - 由 1ms 定时器递减 lock_ms
 * - lock_ms 归零后清中断标志并重新使能引脚中断
 */
#ifndef KEY_LOCKOUT_MS
#define KEY_LOCKOUT_MS 10
#endif

/**
 * @def FN_LOCKOUT_MS
 * @brief FN 按键锁定去抖时间（毫秒）。
 */
#ifndef FN_LOCKOUT_MS
#define FN_LOCKOUT_MS 15
#endif

/**
 * @def FN_LONG_PRESS_MS
 * @brief FN 长按阈值（毫秒），达到阈值后立即产生 LONG 事件。
 */
#ifndef FN_LONG_PRESS_MS
#define FN_LONG_PRESS_MS 800
#endif

/**
 * @def KEY_ENABLE_RELEASE_EVENT
 * @brief 是否为普通按键上报 RELEASE 事件（1=上报，0=不上报）。
 * @note
 * 若关闭 RELEASE 事件，则普通键只会上报 PRESS，且内部 expect 不会切到 release；
 * 此时 Key_IsDown() 将长期保持为 1（按下后不再被 release 更新），这通常是“只关心触发”的场景。
 */
#ifndef KEY_ENABLE_RELEASE_EVENT
#define KEY_ENABLE_RELEASE_EVENT 1
#endif

/**
 * @def KEY_QUEUE_SIZE
 * @brief 普通按键事件队列容量（必须为 2 的幂）。
 */
#ifndef KEY_QUEUE_SIZE
#define KEY_QUEUE_SIZE 16
#endif

/**
 * @def FNKEY_QUEUE_SIZE
 * @brief FN 按键事件队列容量（必须为 2 的幂）。
 */
#ifndef FNKEY_QUEUE_SIZE
#define FNKEY_QUEUE_SIZE 8
#endif

/**
 * @def KEY_IRQ_PRIORITY
 * @brief GPIO 中断优先级（平台相关）。
 */
#ifndef KEY_IRQ_PRIORITY
#define KEY_IRQ_PRIORITY 3
#endif

/**
 * @def TIMER_IRQ_PRIORITY
 * @brief 1ms 定时器中断优先级（平台相关）。
 */
#ifndef TIMER_IRQ_PRIORITY
#define TIMER_IRQ_PRIORITY 2
#endif

/* ============================================================================
 * Types
 * ============================================================================
 */

/**
 * @enum key_evt_type_t
 * @brief 普通按键事件类型。
 */
typedef enum {
  KEY_EVT_PRESS   = 1, /**< 按下事件（下降沿触发，Active-Low） */
  KEY_EVT_RELEASE = 2, /**< 松开事件（上升沿触发） */
} key_evt_type_t;

/**
 * @struct key_event_t
 * @brief 普通按键事件结构。
 */
typedef struct {
  uint8_t  key;      /**< 逻辑按键索引（0..KBD_TOTAL_KEYS-1） */
  uint8_t  type;     /**< 事件类型，见 @ref key_evt_type_t */
  uint32_t tick_ms;  /**< 事件产生时的毫秒计数（自 Key_Init() 起） */
} key_event_t;

/**
 * @enum fnkey_id_t
 * @brief FN 键 ID。
 * @note 这里示例为 2 个 FN 键（FNKEY_1 / FNKEY_2），由 KBD_FN_NUM_KEYS 决定实际数量。
 */
typedef enum {
  FNKEY_1 = 0, /**< FN1 */
  FNKEY_2 = 1, /**< FN2 */
} fnkey_id_t;

/**
 * @enum fnkey_evt_type_t
 * @brief FN 键事件类型。
 */
typedef enum {
  FNKEY_EVT_CLICK = 1, /**< 短按（未达到该 FN 的阈值） */
  FNKEY_EVT_LONG  = 2, /**< 长按（达到该 FN 的阈值） */
} fnkey_evt_type_t;

/**
 * @struct fnkey_event_t
 * @brief FN 键事件结构。
 */
typedef struct {
  uint8_t  id;       /**< FN 键 ID（0..KBD_FN_NUM_KEYS-1），对应 g_fn_pins[] */
  uint8_t  type;     /**< 事件类型，见 @ref fnkey_evt_type_t */
  uint32_t tick_ms;  /**< 事件产生时的毫秒计数（自 Key_Init() 起） */
} fnkey_event_t;

/* ============================================================================
 * API
 * ============================================================================
 */

/**
 * @brief 初始化按键驱动。
 * @details
 * - 配置 BOOT / 普通键 / FN 键为上拉输入
 * - 配置 GPIO 边沿中断（普通键根据当前电平决定先等下降沿还是上升沿；FN 键强制先等下降沿）
 * - 初始化队列和上下文（ctx）
 * - 启动 1ms 定时器，用于：
 *   - 提供 tick_ms 时间戳
 *   - lockout 倒计时到 0 时重新打开引脚中断
 */
void Key_Init(void);

/**
 * @brief 读取一个普通按键事件（出队）。
 * @param[out] evt 事件输出指针（不可为 NULL）。
 * @return 1 表示成功读到一个事件；0 表示队列为空或参数无效。
 * @note
 * - 队列为环形队列：ISR 侧 Push，主循环侧 Pop
 * - 队列满时 PRESS 会被丢弃；RELEASE 会丢弃最旧事件后入队
 */
uint8_t Key_GetEvent(key_event_t *evt);

/**
 * @brief 开始 DEEP 唤醒恢复阶段。
 * @details 唤醒键和重连期间的输入只用于恢复连接，不会作为 HID 操作执行。
 */
void Key_BeginDeepWakeRecovery(uint32_t gpioa_flags, uint32_t gpiob_flags);

/** @brief HID 恢复时结束深睡事件缓冲阶段。 */
void Key_ServiceDeepWakeKeys(uint8_t transport_ready);

/** @brief 深睡唤醒事件是否仍在等待 HID 连接恢复。 */
uint8_t Key_IsDeepWakeRecoveryPending(void);

/** @brief HID 就绪后是否需要丢弃恢复期事件并同步全零键盘报告。 */
uint8_t Key_IsDeepWakeSyncPending(void);

/** @brief 全零键盘报告成功同步后结束 DEEP 恢复阶段。 */
void Key_FinishDeepWakeSync(void);

/**
 * @brief 查询普通按键当前是否处于按下状态。
 * @param key_index 逻辑按键索引
 * @return 1=按下，0=松开，-1=索引非法
 * @note
 * 若 KEY_ENABLE_RELEASE_EVENT=0，则 is_down 在按下后不会被 release 更新。
 * 仅对普通按键扫描链路中的键有效；旋钮转动等虚拟键返回 -1。
 */
int8_t  Key_IsDown(uint8_t key_index);

/**
 * @brief 读取一个 FN 按键事件（出队）。
 * @param[out] evt 事件输出指针（不可为 NULL）。
 * @return 1 表示成功读到一个事件；0 表示队列为空或参数无效。
 * @details
 * LONG 在按住达到阈值时产生；若未达到阈值，则在松开边沿产生 CLICK。
 */
uint8_t FnKey_GetEvent(fnkey_event_t *evt);

/**
 * @brief 设置单个 FN 键的长按阈值。
 * @param id FN 键索引
 * @param threshold_ms 阈值（毫秒），0 表示使用编译期默认值
 */
void FnKey_SetLongPressThreshold(uint8_t id, uint16_t threshold_ms);

/**
 * @brief 查询 FN 按键当前是否处于按下状态。
 * @param id FN 键索引（0..KBD_FN_NUM_KEYS-1）
 * @return 1=按下，0=松开，-1=索引非法
 */
int8_t  FnKey_IsDown(uint8_t id);

/**
 * @brief 读取 BOOT 键是否按下（原始电平读取，不使用中断）。
 * @return 1=按下，0=松开
 * @note Active-Low：按下为 0，松开为 1。
 */
int8_t BootKey_IsPressed(void);

/**
 * @brief 进入低功耗前的钩子（可选）。
 * @details 当前实现为：停止 1ms 定时器，以降低功耗/避免不必要中断。
 */
void Key_EnterSleep(void);

/**
 * @brief 退出低功耗后的钩子（可选）。
 * @details 当前实现为：重新启动 1ms 定时器。
 */
void Key_ExitSleep(void);

/**
 * @brief 配置深度休眠唤醒：所有按键引脚设置为低电平触发 + 打开 GPIO 唤醒源。
 * @note 进入 LowPower_Shutdown() / LowPower_Sleep() 前调用；
 *       任意按键 / FN 键按下（低电平）都会唤醒 MCU。
 */
void Key_ConfigDeepSleepWakeup(void);

/** @} */ /* end of KBD_KEY */

#ifdef __cplusplus
}
#endif

#endif /* KBD_KEY_H_ */
