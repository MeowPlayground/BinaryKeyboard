#ifndef KBD_CONFIG_H
#define KBD_CONFIG_H

#include "CH59x_common.h"

/*============================================================================*/
/**
 * @defgroup KBD_Type 键盘类型定义
 * @brief 支持本次发布的键盘外形
 * @{
 */

/**
 * @brief 键盘类型枚举
 */
typedef enum
{
  KBD_TYPE_5KEYS = 1, /**< 五键款: 5 键 */
  KBD_TYPE_KNOB = 2,  /**< 旋钮款: 4 键 + 1 旋钮 (等效 7 键) */
} kbd_type_t;

/**
 * @brief 各类型键盘的按键数量
 */
#define KBD_KEYS_5KEYS 5u /**< 五键款按键数 */
#define KBD_KEYS_KNOB 7u  /**< 旋钮款按键数 (4 普通键 + 3 旋钮动作) */

/**
 * @brief 旋钮动作索引 (用于旋钮款)
 * @note 旋钮动作映射到虚拟按键索引
 */
#define KBD_KNOB_CW_IDX 4u    /**< 顺时针旋转 */
#define KBD_KNOB_CCW_IDX 5u   /**< 逆时针旋转 */
#define KBD_KNOB_CLICK_IDX 6u /**< 旋钮按下 */

/** @} */

/*============================================================================*/
/**
 * @defgroup KBD_Keyboard 键盘型号选择
 * @brief 编译时选择键盘型号 (只能启用一个)
 * @{
 */

#if defined(KBD_LAYOUT_5KEY) && defined(KBD_LAYOUT_KNOB)
#error "Only one keyboard may be enabled: KBD_LAYOUT_5KEY or KBD_LAYOUT_KNOB."
#endif

#if !defined(KBD_LAYOUT_5KEY) && !defined(KBD_LAYOUT_KNOB)
#error "Define KBD_LAYOUT_5KEY or KBD_LAYOUT_KNOB in CMake/MRS preprocessor settings before building."
#endif

/** @} */

/*============================================================================*/
/**
 * @defgroup KBD_GPIO GPIO 类型定义
 * @{
 */

typedef enum
{
  GPIO_PORT_A = 0,
  GPIO_PORT_B = 1,
} gpio_port_t;

typedef struct
{
  gpio_port_t port;
  uint32_t pin; /**< 位掩码：GPIO_Pin_x */
} kbd_key_pin_t;

/** @} */

/*============================================================================*/
/**
 * @defgroup KBD_Special 特殊 IO 定义
 * @{
 */

/* WS2812 数据引脚 */
#define WS2812_PORT GPIO_PORT_A
#define WS2812_PIN GPIO_Pin_10

/* WS2812 使能引脚 (PA9), 高电平上电 */
#define WS2812_EN_PORT GPIO_PORT_A
#define WS2812_EN_PIN GPIO_Pin_9
#define WS2812_EN_ACTIVE_HIGH 1

/* WS2812 LED 配置：仅指示灯模式 (注释掉则启用按键灯) */
// #define WS2812_INDICATOR_ONLY

/* 指示灯最低亮度 (5%)，用户不可完全关闭 */
#define KBD_INDICATOR_MIN_BRIGHTNESS 13

/* TP4054 充电状态引脚: PA13 (开漏输出, 低=充电中, 高阻=未充电) */
#define KBD_HAS_CHARGE_DET 1
#define KBD_CHG_PORT GPIO_PORT_A
#define KBD_CHG_PIN GPIO_Pin_13

/* VBAT 电压采样: 两个 100K 电阻分压 (1/2), 经 PMOS 使能 */
/* PA14 (AIN4): ADC 输入 */
/* PA15: 分压电路使能 (高电平有效) */
#define KBD_VBAT_ADC_CH CH_EXTIN_4
#define KBD_VBAT_ADC_PIN GPIO_Pin_14
#define KBD_VBAT_EN_PIN GPIO_Pin_15

/* Battery divider is powered only during a requested sample window. */
#define KBD_VBAT_DIVIDER_ALWAYS_ON 0

/* CH592 VINTA ADC reference and the board's 100K/100K VBAT divider. */
#define KBD_ADC_VREF_MV 1050u
#define KBD_VBAT_DIVIDER_NUM 2u
#define KBD_VBAT_DIVIDER_DEN 1u

/** @} */

/*============================================================================*/
/**
 * @defgroup KBD_FnKeys 功能键定义
 * @brief FN 键只产生下降沿事件，支持短按/长按
 * @{
 */

#define KBD_FN_NUM_KEYS 2u

/* BOOT 按键: PB22 (层切换修饰键: BOOT + K1..Kn -> Layer 1..n) */
#define KBD_FN_BOOT_PORT GPIO_PORT_B
#define KBD_FN_BOOT_PIN GPIO_Pin_22

/* FN1: PA4 */
#define KBD_FN1_PORT GPIO_PORT_A
#define KBD_FN1_PIN GPIO_Pin_4

/* FN2: PA5 */
#define KBD_FN2_PORT GPIO_PORT_A
#define KBD_FN2_PIN GPIO_Pin_5

/** @} */

/*============================================================================*/
/**
 * @defgroup KBD_Keys 普通按键定义
 * @brief 根据键盘型号定义不同的引脚映射
 * @{
 */

#if defined(KBD_LAYOUT_5KEY)
/*---------------------------------------------------------------------------*/
/* 五键款: 5 键, 5 层 */
/*---------------------------------------------------------------------------*/
#define KBD_CURRENT_TYPE KBD_TYPE_5KEYS
#define KBD_NUM_KEYS KBD_KEYS_5KEYS
#define KBD_DEFAULT_LAYERS 5u /**< 默认层数 = 按键数 */

#define KBD_K1_PORT GPIO_PORT_B
#define KBD_K1_PIN GPIO_Pin_4

#define KBD_K2_PORT GPIO_PORT_B
#define KBD_K2_PIN GPIO_Pin_7

#define KBD_K3_PORT GPIO_PORT_A
#define KBD_K3_PIN GPIO_Pin_12

#define KBD_K4_PORT GPIO_PORT_B
#define KBD_K4_PIN GPIO_Pin_12

#define KBD_K5_PORT GPIO_PORT_A
#define KBD_K5_PIN GPIO_Pin_8

#elif defined(KBD_LAYOUT_KNOB)
/*---------------------------------------------------------------------------*/
/* 旋钮款: 4 普通键 + 1 旋钮, 4 层 (层数与 RGB 数量一致) */
/*---------------------------------------------------------------------------*/
#define KBD_CURRENT_TYPE KBD_TYPE_KNOB
#define KBD_NUM_KEYS 4u              /**< 实际物理普通键数量 */
#define KBD_TOTAL_KEYS KBD_KEYS_KNOB /**< 总虚拟键数 (含旋钮) */
#define KBD_DEFAULT_LAYERS 4u        /**< 默认层数 = 4 个 RGB / 4 个层位 */
#define KBD_HAS_ENCODER 1            /**< 启用编码器支持 */

/* 普通按键 */
#define KBD_K1_PORT GPIO_PORT_B
#define KBD_K1_PIN GPIO_Pin_7

#define KBD_K2_PORT GPIO_PORT_A
#define KBD_K2_PIN GPIO_Pin_12

#define KBD_K3_PORT GPIO_PORT_B
#define KBD_K3_PIN GPIO_Pin_12

#define KBD_K4_PORT GPIO_PORT_A
#define KBD_K4_PIN GPIO_Pin_8

/* 旋钮编码器引脚 */
#define KBD_ENCODER_A_PORT GPIO_PORT_B
#define KBD_ENCODER_A_PIN GPIO_Pin_14

#define KBD_ENCODER_B_PORT GPIO_PORT_B
#define KBD_ENCODER_B_PIN GPIO_Pin_15

/* 旋钮按键 (可选，复用 FN 或独立) */
#define KBD_ENCODER_BTN_PORT GPIO_PORT_B
#define KBD_ENCODER_BTN_PIN GPIO_Pin_4

#else
#error \
    "请通过 CMake 或 MRS 预处理宏选择一个键盘型号 (KBD_LAYOUT_5KEY / KBD_LAYOUT_KNOB)"
#endif

/* 统一的虚拟按键总数 (用于按键映射) */
#ifndef KBD_TOTAL_KEYS
#define KBD_TOTAL_KEYS KBD_NUM_KEYS
#endif

/** @} */

/*============================================================================*/
/**
 * @defgroup KBD_RGB_Map RGB LED 映射配置
 * @brief 定义按键索引到 WS2812 LED 索引的映射关系
 * @note WS2812 索引 0 为指示灯，1-N 为按键 RGB
 * @{
 */

/**
 * @brief 逻辑按键到物理 WS2812 LED 的映射
 * @details PCB 上 WS2812 LED 串联顺序与逻辑按键编号不一定一致。
 *          此表将逻辑按键索引 (0~N-1) 映射到物理 LED 索引 (0~N-1)。
 *          物理 LED 索引 0 对应 WS2812 链中第 2 个 LED (索引 1 = 第一个按键灯)。
 *
 * 例: {2,1,0,3,4} 表示逻辑键 0 → 物理 LED 2, 逻辑键 1 → 物理 LED 1, ...
 */
#if defined(KBD_LAYOUT_5KEY)
#define KBD_LOGICAL_TO_PHYSICAL_MAP {2, 3, 4, 1, 0}
#define KBD_LAYER_TO_KEY_MAP {0, 1, 2, 3, 4}
#elif defined(KBD_LAYOUT_KNOB)
/* PCB chain order: swap the K1/K2 and K3/K4 logical pairs. */
#define KBD_LOGICAL_TO_PHYSICAL_MAP {2, 3, 1, 0}
#define KBD_LAYER_TO_KEY_MAP {0, 1, 2, 3}
#endif

/**
 * @brief 层切换闪烁颜色配置 (RGB格式)
 * @details 每层对应的颜色 [R, G, B], 范围 0-255
 *
 * 默认配色方案：
 * - 层0: 蓝色 (0, 100, 255)
 * - 层1: 绿色 (0, 255, 100)
 * - 层2: 黄色 (255, 200, 0)
 * - 层3: 紫色 (200, 0, 255)
 * - 层4: 红色 (255, 50, 50)
 */
#define KBD_LAYER_COLORS {         \
    {0, 100, 255}, /* 层0: 蓝色 */ \
    {0, 255, 100}, /* 层1: 绿色 */ \
    {255, 200, 0}, /* 层2: 黄色 */ \
    {200, 0, 255}, /* 层3: 紫色 */ \
    {255, 50, 50}, /* 层4: 红色 */ \
}

/**
 * @brief 层切换闪烁次数 (固定值)
 */
#define KBD_LAYER_FLASH_BLINKS 3

/** @} */

/*============================================================================*/
/**
 * @defgroup KBD_Utils 工具函数
 * @{
 */

/**
 * @brief 获取当前键盘类型
 * @return 键盘类型枚举值
 */
static inline kbd_type_t KBD_GetType(void) { return KBD_CURRENT_TYPE; }

/**
 * @brief 获取当前键盘的总键位数 (用于映射)
 * @return 键位数量
 */
static inline uint8_t KBD_GetTotalKeyCount(void) { return KBD_TOTAL_KEYS; }

/**
 * @brief 获取当前键盘的物理按键数
 * @return 物理按键数量
 */
static inline uint8_t KBD_GetPhysicalKeyCount(void) { return KBD_NUM_KEYS; }

/**
 * @brief 获取默认层数
 * @return 当前机型默认层数
 */
static inline uint8_t KBD_GetDefaultLayers(void) { return KBD_DEFAULT_LAYERS; }

/** @} */

#endif /* KBD_CONFIG_H */
