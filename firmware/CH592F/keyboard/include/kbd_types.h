/**
 * @file    kbd_types.h
 * @brief   MeowKeyboard 核心数据类型定义
 * @author  MeowKJ
 * @version V1.0.0
 * @date    2024-11-07
 *
 * @details
 * 本文件定义了 MeowKeyboard 配置系统的所有核心数据结构，包括：
 * - 按键动作类型与映射结构
 * - 多层键盘映射配置
 * - FN 功能键配置
 * - 宏定义与存储结构
 * - RGB 灯效配置
 * - USB HID 通讯协议
 *
 * @note    所有结构体使用 packed 属性以确保内存布局一致性
 *
 * @copyright Copyright (c) 2024 MeowKJ. All rights reserved.
 */

#ifndef __KBD_TYPES_H
#define __KBD_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include "bk_version_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @defgroup KBD_Constants 常量定义
   * @brief 系统级常量与限制
   * @{
   */

#define KBD_CONFIG_MAGIC 0x4D454F57             /**< 配置魔数 "MEOW" */
#define KBD_CONFIG_LAYOUT_ID 0x0103             /**< 本地 DataFlash 布局版本 */
#define KBD_CONFIG_VERSION KBD_CONFIG_LAYOUT_ID /**< 配置版本 */

#define KBD_MAX_LAYERS 5  /**< 配置结构层数组容量上限 */
#define KBD_MAX_KEYS 8    /**< 单层最大按键数 (支持所有类型) */
#define KBD_MAX_FN_KEYS 4 /**< 最大 FN 键数 */

#define KBD_MACRO_SLOTS 255        /**< 宏逻辑索引上限 (param1 为 uint8_t) */
#define KBD_MACRO_MAX_SIZE 8192    /**< MeowFS 总容量 (8KB) */
#define KBD_MACRO_MAX_ACTIONS 255  /**< 单个宏最大动作数 */
#define KBD_MACRO_VALID_MAGIC 0xAA /**< 宏有效标记 */

#define KBD_SEAMLESS_WAKE_ENABLED  0xA5u
#define KBD_SEAMLESS_WAKE_DISABLED 0x5Au

/**
 * @brief 设备信息常量
 */
#define KBD_VENDOR_ID 0x413D                        /**< USB Vendor ID */
#define KBD_PRODUCT_ID 0x2107                       /**< USB Product ID */
#define KBD_VERSION_MAJOR BK_FIRMWARE_VERSION_MAJOR /**< 固件主版本 */
#define KBD_VERSION_MINOR BK_FIRMWARE_VERSION_MINOR /**< 固件次版本 */
#define KBD_VERSION_PATCH BK_FIRMWARE_VERSION_PATCH /**< 固件补丁版本 */
/** 主.次 版本 BCD 16 位，供 USB bcdDevice、BLE PnP 等使用，改主/次版本后自动同步 */
#define KBD_VERSION_BCD16 ((KBD_VERSION_MAJOR << 8) | KBD_VERSION_MINOR)

  /** @} */ /* end of KBD_Constants */

  /**
   * @defgroup KBD_Flash DataFlash 地址布局
   * @brief CH592F DataFlash (32KB) 存储分配
   * @{
   */

#define KBD_FLASH_BASE 0x00000        /**< DataFlash 基址 */
#define KBD_FLASH_HEADER 0x00000      /**< 配置头偏移 (32B, 槽位内偏移) */
#define KBD_FLASH_SYSTEM 0x00100      /**< 系统配置偏移 (64B, 槽位内偏移) */
#define KBD_FLASH_KEYMAP 0x00200      /**< 按键映射偏移 (164B, 槽位内偏移) */
#define KBD_FLASH_FNKEY 0x00300       /**< FN 键配置偏移 (32B, 槽位内偏移) */
#define KBD_FLASH_RGB 0x00340         /**< RGB 配置偏移 (32B, 槽位内偏移) */
#define KBD_FLASH_RESERVED 0x00400    /**< 单配置槽大小 / 下一页起始 (1KB) */
#define KBD_FLASH_MACRO_BASE 0x01000  /**< 宏数据区起始（0x0000~0x0BFF 配置槽；0x0C00~0x0FFF runtime 热数据） */
#define KBD_FLASH_MACRO_SIZE 0x02000  /**< MeowFS 宏数据区大小 (8KB) */
#define KBD_FLASH_MACRO_PAGE 0x00100  /**< MeowFS 页大小 (256B) */
#define KBD_FLASH_MACRO_HEADER 0x0002 /**< MeowFS 头部大小 (marker + count) */

  /** @} */ /* end of KBD_Flash */

  /*============================================================================*/
  /**
   * @defgroup KBD_ActionTypes 按键动作类型
   * @brief 定义按键可执行的各类动作
   * @{
   */

  /**
   * @brief 按键动作类型枚举
   *
   * 每个物理按键可以映射到以下任意一种动作类型
   */
  typedef enum
  {
    KBD_ACTION_NONE = 0x00,        /**< 无动作 */
    KBD_ACTION_KEYBOARD = 0x01,    /**< 键盘按键 (修饰键 + 键码) */
    KBD_ACTION_MOUSE_BTN = 0x02,   /**< 鼠标按键 */
    KBD_ACTION_MOUSE_WHEEL = 0x03, /**< 鼠标滚轮 */
    KBD_ACTION_CONSUMER = 0x04,    /**< 多媒体控制键 */
    KBD_ACTION_MACRO = 0x05,       /**< 执行宏 */
    KBD_ACTION_LAYER = 0x06,       /**< 层切换 */
  } kbd_action_type_t;

  /**
   * @brief 鼠标滚轮方向
   */
  typedef enum
  {
    KBD_WHEEL_NONE = 0,  /**< 无动作 */
    KBD_WHEEL_UP = 1,    /**< 向上滚动一格 */
    KBD_WHEEL_DOWN = 2,  /**< 向下滚动一格 */
    KBD_WHEEL_CLICK = 3, /**< 中键点击 */
  } kbd_wheel_dir_t;

  /**
   * @brief 鼠标按键掩码
   */
  typedef enum
  {
    KBD_MOUSE_LEFT = 0x01,    /**< 左键 */
    KBD_MOUSE_RIGHT = 0x02,   /**< 右键 */
    KBD_MOUSE_MIDDLE = 0x04,  /**< 中键 */
    KBD_MOUSE_BACK = 0x08,    /**< 后退键 */
    KBD_MOUSE_FORWARD = 0x10, /**< 前进键 */
  } kbd_mouse_btn_t;

  /**
   * @brief 层操作类型
   */
  typedef enum
  {
    KBD_LAYER_MOMENTARY = 0, /**< 按住时切换，松开恢复 */
    KBD_LAYER_TOGGLE = 1,    /**< 切换开关 */
    KBD_LAYER_SET = 2,       /**< 直接设置为指定层 */
  } kbd_layer_op_t;

  /** @} */ /* end of KBD_ActionTypes */

  /*============================================================================*/
  /**
   * @defgroup KBD_KeyMapping 按键映射结构
   * @brief 定义按键到动作的映射关系
   * @{
   */

  /**
   * @brief 单键动作结构 (4 字节)
   *
   * @note 根据 type 字段，其他字段含义不同：
   * - KBD_ACTION_KEYBOARD: modifier=修饰键, param1=键码
   * - KBD_ACTION_MOUSE_BTN: param1=按键掩码
   * - KBD_ACTION_MOUSE_WHEEL: param1=滚轮方向
   * - KBD_ACTION_CONSUMER: param1=低字节, param2=高字节
   * - KBD_ACTION_MACRO: param1=宏ID
   * - KBD_ACTION_LAYER: modifier=操作类型, param1=层号
   */
  typedef struct __attribute__((packed))
  {
    uint8_t type;     /**< 动作类型 @ref kbd_action_type_t */
    uint8_t modifier; /**< 修饰键或操作类型 */
    uint8_t param1;   /**< 参数1 (键码/按键/方向/ID) */
    uint8_t param2;   /**< 参数2 (多媒体键高字节) */
  } kbd_action_t;

  /**
   * @brief 单层按键映射 (32 字节)
   */
  typedef struct __attribute__((packed))
  {
    kbd_action_t keys[KBD_MAX_KEYS]; /**< 按键动作数组 */
  } kbd_layer_t;

  /**
   * @brief 完整按键映射配置 (164 字节)
   */
  typedef struct __attribute__((packed))
  {
    uint8_t num_layers;                 /**< 实际使用的层数 (1-KBD_MAX_LAYERS) */
    uint8_t current_layer;              /**< 当前激活层 */
    uint8_t default_layer;              /**< 默认层 */
    uint8_t reserved;                   /**< 保留字段 */
    kbd_layer_t layers[KBD_MAX_LAYERS]; /**< 层映射数组 */
  } kbd_keymap_t;

  /** @} */ /* end of KBD_KeyMapping */

  /*============================================================================*/
  /**
   * @defgroup KBD_FnKey FN 功能键配置
   * @brief FN 键支持短按和长按两种动作
   * @{
   */

  /**
   * @brief FN 键动作类型
   *
   * FN 键只执行系统功能，不发送 HID 报告
   */
  typedef enum
  {
    KBD_FN_NONE = 0x00, /**< 无动作 */

    /* 模式控制 0x01-0x0F */
    KBD_FN_MODE_TOGGLE = 0x01,     /**< USB/BLE 模式切换 */
    KBD_FN_BLE_ADV = 0x02,         /**< 开始蓝牙广播 */
    KBD_FN_BLE_DISCONNECT = 0x03,  /**< 断开蓝牙连接 */
    KBD_FN_BLE_CLEAR_BONDS = 0x04, /**< 清除所有配对信息 */

    /* RGB 控制 0x10-0x1F */
    KBD_FN_RGB_TOGGLE = 0x10,      /**< RGB 开关 */
    KBD_FN_RGB_MODE_NEXT = 0x11,   /**< 下一个灯效模式 */
    KBD_FN_RGB_MODE_PREV = 0x12,   /**< 上一个灯效模式 */
    KBD_FN_RGB_BRIGHT_UP = 0x13,   /**< 增加亮度 */
    KBD_FN_RGB_BRIGHT_DOWN = 0x14, /**< 降低亮度 */

    /* 层控制 0x20-0x2F */
    KBD_FN_LAYER_NEXT = 0x20, /**< 切换到下一层 */
    KBD_FN_LAYER_PREV = 0x21, /**< 切换到上一层 */
    KBD_FN_LAYER_SET = 0x22,  /**< 设置为指定层 (param=层号) */

    /* 宏 0x30-0x3F */
    KBD_FN_MACRO = 0x30, /**< 执行宏 (param=宏ID) */

    /* 系统 0x40-0x4F */
    KBD_FN_SLEEP = 0x40,      /**< 进入睡眠模式 */
    KBD_FN_BOOTLOADER = 0x41, /**< 进入 Bootloader */
  } kbd_fn_action_t;

  /**
   * @brief 单个 FN 键配置 (8 字节)
   */
  typedef struct __attribute__((packed))
  {
    uint8_t click_action;   /**< 短按动作 @ref kbd_fn_action_t */
    uint8_t click_param;    /**< 短按参数 */
    uint8_t long_action;    /**< 长按动作 @ref kbd_fn_action_t */
    uint8_t long_param;     /**< 长按参数 */
    uint16_t long_press_ms; /**< 长按判定阈值 (毫秒) */
    uint8_t reserved[2];    /**< 保留字段 */
  } kbd_fnkey_entry_t;

  /**
   * @brief FN 键配置集合 (32 字节)
   */
  typedef struct __attribute__((packed))
  {
    kbd_fnkey_entry_t fn[KBD_MAX_FN_KEYS]; /**< FN 键配置数组 */
  } kbd_fnkey_config_t;

  /** @} */ /* end of KBD_FnKey */

  /*============================================================================*/
  /**
   * @defgroup KBD_Macro 宏定义结构
   * @brief 宏支持键盘、鼠标、延时等多种动作
   * @{
   */

  /**
   * @brief 宏触发模式
   *
   * 存储在 kbd_action_t.modifier 字段（当 type == KBD_ACTION_MACRO 时）
   */
  typedef enum
  {
    KBD_MACRO_TRIG_ONCE = 0x00,        /**< 按下触发 1 次 */
    KBD_MACRO_TRIG_HOLD_ABORT = 0x01,  /**< 按住循环，松开立即中断 */
    KBD_MACRO_TRIG_HOLD_FINISH = 0x02, /**< 按住循环，松开跑完当轮再停 */
    KBD_MACRO_TRIG_TOGGLE = 0x03,      /**< 按下开始循环，再按停（跑完当轮） */
  } kbd_macro_trigger_t;

  /**
   * @brief 宏动作类型
   *
   * @note 所有按键类动作都需要显式的 DOWN 和 UP 配对
   */
  typedef enum
  {
    /* 键盘动作 0x01-0x0F */
    KBD_MACRO_KEY_DOWN = 0x01, /**< 按下普通键 (param=键码) */
    KBD_MACRO_KEY_UP = 0x02,   /**< 释放普通键 (param=键码) */
    KBD_MACRO_MOD_DOWN = 0x03, /**< 按下修饰键 (param=修饰键掩码) */
    KBD_MACRO_MOD_UP = 0x04,   /**< 释放修饰键 (param=修饰键掩码) */

    /* 延时 0x10-0x1F */
    KBD_MACRO_DELAY = 0x10, /**< 延时 (param × 10ms, 最大 2550ms) */

    /* 多媒体 0x20-0x2F */
    KBD_MACRO_CONSUMER = 0x20, /**< 多媒体键 (按下立即释放) */

    /* 鼠标 0x30-0x3F */
    KBD_MACRO_MOUSE_DOWN = 0x30, /**< 鼠标按下 (param=按键掩码) */
    KBD_MACRO_MOUSE_UP = 0x31,   /**< 鼠标释放 (param=按键掩码) */
    KBD_MACRO_WHEEL = 0x32,      /**< 滚轮 (param=方向) */

    /* 结束标记 */
    KBD_MACRO_END = 0xFF, /**< 宏结束 */
  } kbd_macro_action_type_t;

  /**
   * @brief 宏动作单元 (2 字节)
   */
  typedef struct __attribute__((packed))
  {
    uint8_t type;  /**< 动作类型 @ref kbd_macro_action_type_t */
    uint8_t param; /**< 动作参数 */
  } kbd_macro_action_t;

  /**
   * @brief 宏头部信息 (24 字节)
   *
   * 存储在 Flash 中，后接实际的动作数据
   */
  typedef struct __attribute__((packed))
  {
    uint8_t valid;         /**< 有效标记 (0xAA=有效) */
    uint8_t id;            /**< 宏 ID (0-7) */
    uint16_t action_count; /**< 动作数量 */
    uint16_t data_size;    /**< 数据大小 (字节) */
    uint8_t reserved[2];   /**< 保留字段 */
    char name[16];         /**< 宏名称 (UTF-8, 空字符结尾) */
  } kbd_macro_header_t;

  /** @} */ /* end of KBD_Macro */

  /*============================================================================*/
  /**
   * @defgroup KBD_RGB RGB 配置
   * @brief RGB 灯效模式与状态指示配置
   * @{
   */

  /**
   * @brief RGB 灯效模式
   */
  typedef enum
  {
    KBD_RGB_OFF = 0,       /**< 关闭 */
    KBD_RGB_STATIC = 1,    /**< 静态单色 */
    KBD_RGB_BREATHING = 2, /**< 呼吸效果 */
    KBD_RGB_BLINK = 3,     /**< 闪烁效果 */
    KBD_RGB_RAINBOW = 4,   /**< 彩虹渐变 */
    KBD_RGB_INDICATOR = 5, /**< 状态指示模式 (最高优先级) */
  } kbd_rgb_mode_t;

  /**
   * @brief RGB 状态指示类型
   *
   * 用于自动显示键盘当前状态
   */
  typedef enum
  {
    KBD_STATE_USB_CONNECTED,    /**< USB 已连接 - 白色常亮 */
    KBD_STATE_BLE_DISCONNECTED, /**< 蓝牙未连接 - 红色慢呼吸 */
    KBD_STATE_BLE_ADVERTISING,  /**< 蓝牙广播中 - 蓝色呼吸 */
    KBD_STATE_BLE_CONNECTED,    /**< 蓝牙已连接 - 绿色常亮 */
    KBD_STATE_LOW_BATTERY,      /**< 低电量警告 - 红色快闪 */
    KBD_STATE_CHARGING,         /**< 充电中 - 琥珀色短脉冲 */
    KBD_STATE_SLEEP_READY,      /**< 低功耗待机 - 蓝青色短呼吸 */
    KBD_STATE_SLEEPING,         /**< 低功耗中 - 蓝色短闪后熄灭 */
  } kbd_state_t;

/** @brief 默认亮度 20% (255 * 20 / 100 ≈ 51) */
#define KBD_RGB_DEFAULT_BRIGHTNESS 51

  /**
   * @brief RGB 配置结构 (32 字节)
   */
  typedef struct __attribute__((packed))
  {
    uint8_t enabled;              /**< RGB 总开关 */
    uint8_t mode;                 /**< 灯效模式 @ref kbd_rgb_mode_t */
    uint8_t brightness;           /**< RGB/按键灯亮度 (0-255) */
    uint8_t speed;                /**< 动画速度 (0-255) */
    uint8_t color_r;              /**< 静态颜色 - 红 */
    uint8_t color_g;              /**< 静态颜色 - 绿 */
    uint8_t color_b;              /**< 静态颜色 - 蓝 */
    uint8_t indicator_enabled;    /**< 状态指示开关 */
    uint8_t indicator_brightness; /**< 指示灯亮度 (0-255) */
    uint8_t press_effect;         /**< 按下效果 (0=无, 1=亮起渐灭, 2=熄灭渐亮) */
    uint8_t reserved[22];         /**< 保留字段 */
  } kbd_rgb_config_t;

  /** @} */ /* end of KBD_RGB */

  /*============================================================================*/
  /**
   * @defgroup KBD_System 系统配置
   * @brief 系统级配置参数
   * @{
   */

  /**
   * @brief 系统配置结构 (64 字节)
   */
  typedef struct __attribute__((packed))
  {
    uint8_t default_mode;    /**< 默认工作模式 (0=USB, 1=BLE) */
    uint8_t auto_sleep_min;  /**< LIGHT 休眠时间 (分钟, 0=禁用) */
    uint8_t debounce_ms;     /**< 按键消抖时间 (毫秒) */
    /* HID 日志配置 (v1.2+) */
    uint8_t log_enabled;     /**< HID 日志开关 (0=关, 非0=开, 默认1) */
    uint8_t deep_sleep_min;  /**< DEEP 延时 (在 LIGHT 后, 分钟, 0=禁用) */
    uint8_t os_mode;         /**< 系统模式 (0=Win, 1=Mac) */
    uint8_t seamless_wake;   /**< 唤醒首键透传，使用 KBD_SEAMLESS_WAKE_* 标记 */
    uint8_t reserved[57];    /**< 保留字段 */
  } kbd_system_config_t;

  typedef enum
  {
    KBD_OS_MODE_WIN = 0,
    KBD_OS_MODE_MAC = 1,
  } kbd_os_mode_t;

  /**
   * @brief 配置头部结构 (32 字节)
   *
   * 用于验证配置有效性和版本兼容性
   */
  typedef struct __attribute__((packed))
  {
    uint32_t magic;       /**< 魔数 @ref KBD_CONFIG_MAGIC */
    uint16_t version;     /**< 配置版本 */
    uint16_t flags;       /**< 标志位 */
    uint32_t crc32;       /**< 数据校验码 */
    uint32_t save_count;  /**< 保存计数 */
    uint8_t reserved[16]; /**< 保留字段 */
  } kbd_config_header_t;

  /** @} */ /* end of KBD_System */

  /*============================================================================*/
  /**
   * @defgroup KBD_Protocol HID 通讯协议
   * @brief USB HID 配置命令定义
   * @{
   */

  /**
   * @brief HID 命令码
   */
  typedef enum
  {
    /* 系统命令 0x01-0x0F */
    KBD_CMD_SYS_INFO = 0x01,   /**< 获取设备信息 */
    KBD_CMD_SYS_STATUS = 0x02, /**< 获取运行状态 */

    /* 配置管理 0x10-0x1F */
    KBD_CMD_CFG_SAVE = 0x10,  /**< 保存配置到 Flash */
    KBD_CMD_CFG_LOAD = 0x11,  /**< 从 Flash 加载配置 */
    KBD_CMD_CFG_RESET = 0x12, /**< 恢复出厂设置 */
    KBD_CMD_CFG_OS_GET = 0x13, /**< 获取系统模式 */
    KBD_CMD_CFG_OS_SET = 0x14, /**< 设置系统模式 */

    /* 按键映射 0x20-0x2F */
    KBD_CMD_KEYMAP_GET = 0x20, /**< 获取按键映射 */
    KBD_CMD_KEYMAP_SET = 0x21, /**< 设置按键映射 */
    KBD_CMD_LAYER_GET = 0x22,  /**< 获取当前层信息 */
    KBD_CMD_LAYER_SET = 0x23,  /**< 设置当前层 */

    /* RGB 控制 0x30-0x3F */
    KBD_CMD_RGB_GET = 0x30, /**< 获取 RGB 配置 */
    KBD_CMD_RGB_SET = 0x31, /**< 设置 RGB 配置 */

    /* 宏管理 0x40-0x4F */
    KBD_CMD_MACRO_INFO = 0x40, /**< 获取宏信息 */
    KBD_CMD_MACRO_GET = 0x41,  /**< 读取宏数据 */
    KBD_CMD_MACRO_SET = 0x42,  /**< 写入宏数据 (分包传输) */
    KBD_CMD_MACRO_DEL = 0x43,  /**< 删除宏 */

    /* FN 键配置 0x50-0x5F */
    KBD_CMD_FNKEY_GET = 0x50, /**< 获取 FN 键配置 */
    KBD_CMD_FNKEY_SET = 0x51, /**< 设置 FN 键配置 */

    /* 电源管理 0x60-0x6F */
    KBD_CMD_BATTERY = 0x60, /**< 获取电池信息 */

    /* 设备日志 0x70-0x7F */
    KBD_CMD_LOG = 0x70,     /**< 设备日志推送 (设备 → 主机, 异步) */
    KBD_CMD_LOG_GET = 0x71, /**< 获取日志配置 */
    KBD_CMD_LOG_SET = 0x72, /**< 设置日志配置 */

    /* DataFlash 调试 0x90-0x9F */
    KBD_CMD_DATAFLASH_INFO = 0x90, /**< 获取 DataFlash 布局信息 */
    KBD_CMD_DATAFLASH_READ = 0x91, /**< 读取 DataFlash 原始字节 */
    KBD_CMD_DATAFLASH_WRITE = 0x92, /**< 写入 DataFlash 单字节（危险） */
  } kbd_cmd_t;

  /**
   * @brief HID 日志类别 (KBD_CMD_LOG 的 SUB 字段)
   */
  typedef enum
  {
    KBD_LOG_KEY_EVENT = 0x01,    /**< 按键按下/释放 */
    KBD_LOG_FN_EVENT = 0x02,     /**< FN 键单击/长按 */
    KBD_LOG_LAYER_EVENT = 0x03,  /**< 层切换 */
    KBD_LOG_MODE_EVENT = 0x04,   /**< USB/BLE 模式切换 */
    KBD_LOG_BLE_EVENT = 0x05,    /**< 蓝牙状态变化 */
    KBD_LOG_RGB_EVENT = 0x06,    /**< RGB 模式变化 */
    KBD_LOG_SYSTEM_EVENT = 0x07, /**< 系统事件 */
  } kbd_log_category_t;

  /**
   * @brief 系统事件子类型
   */
  typedef enum
  {
    KBD_LOG_SYS_BOOT = 0x01,   /**< 设备启动 */
    KBD_LOG_SYS_SLEEP = 0x02,  /**< 进入休眠 */
    KBD_LOG_SYS_WAKEUP = 0x03, /**< 唤醒 */
  } kbd_log_sys_event_t;

  /**
   * @brief HID 响应码
   */
  typedef enum
  {
    KBD_RESP_OK = 0x00,            /**< 成功 */
    KBD_RESP_ERR_INVALID = 0x01,   /**< 无效命令 */
    KBD_RESP_ERR_PARAM = 0x02,     /**< 参数错误 */
    KBD_RESP_ERR_BUSY = 0x03,      /**< 设备忙 */
    KBD_RESP_ERR_FLASH = 0x04,     /**< Flash 操作失败 */
    KBD_RESP_ERR_TOO_LARGE = 0x10, /**< 数据过大 */
    KBD_RESP_ERR_NO_SPACE = 0x11,  /**< 存储空间不足 */
    KBD_RESP_ERR_NOT_FOUND = 0x12, /**< 未找到目标 */
  } kbd_resp_t;

  /**
   * @brief HID 命令帧结构 (64 字节)
   */
  typedef struct __attribute__((packed))
  {
    uint8_t cmd;      /**< 命令码 @ref kbd_cmd_t */
    uint8_t sub;      /**< 子命令/序号 */
    uint8_t len;      /**< 数据长度 */
    uint8_t data[61]; /**< 数据区 */
  } kbd_cmd_frame_t;

/** @} */ /* end of KBD_Protocol */

/*============================================================================*/
/**
 * @defgroup KBD_Helpers 辅助宏定义
 * @brief 用于快速创建按键动作的宏
 * @{
 */

/** @brief 创建键盘按键动作 */
#define KBD_KEY(mod, key) {KBD_ACTION_KEYBOARD, (mod), (key), 0}

/** @brief 创建鼠标按键动作 */
#define KBD_MOUSE(btn) {KBD_ACTION_MOUSE_BTN, 0, (btn), 0}

/** @brief 创建滚轮动作 */
#define KBD_WHEEL(dir) {KBD_ACTION_MOUSE_WHEEL, 0, (dir), 0}

/** @brief 创建多媒体键动作 */
#define KBD_CONSUMER(code) \
  {KBD_ACTION_CONSUMER, 0, (code) & 0xFF, ((code) >> 8)}

/** @brief 创建宏动作 */
#define KBD_MACRO(id) {KBD_ACTION_MACRO, 0, (id), 0}

/** @brief 创建层切换动作 */
#define KBD_LAYER(op, id) {KBD_ACTION_LAYER, (op), (id), 0}

/** @brief 创建空动作 */
#define KBD_NONE() {KBD_ACTION_NONE, 0, 0, 0}

/* 修饰键掩码 */
#define KBD_MOD_LCTRL 0x01  /**< 左 Ctrl */
#define KBD_MOD_LSHIFT 0x02 /**< 左 Shift */
#define KBD_MOD_LALT 0x04   /**< 左 Alt */
#define KBD_MOD_LGUI 0x08   /**< 左 GUI (Win/Cmd) */
#define KBD_MOD_RCTRL 0x10  /**< 右 Ctrl */
#define KBD_MOD_RSHIFT 0x20 /**< 右 Shift */
#define KBD_MOD_RALT 0x40   /**< 右 Alt */
#define KBD_MOD_RGUI 0x80   /**< 右 GUI */

  /** @} */ /* end of KBD_Helpers */

#ifdef __cplusplus
}
#endif

#endif /* __KBD_TYPES_H */
