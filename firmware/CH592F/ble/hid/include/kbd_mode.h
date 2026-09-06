/********************************** (C) COPYRIGHT *******************************
 * File Name          : kbd_mode.h
 * Author             : MeowKJ
 * Version            : V2.0
 * Date               : 2024/11/07
 * Description        : 键盘模式管理器接口（支持 USB/BLE/2.4G 多模）
 *******************************************************************************/

#ifndef __KBD_MODE_H
#define __KBD_MODE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "kbd_mode_config.h"
#include <stdint.h>
#include <stdbool.h>

    /*============================================================================*/
    /* 类型定义 */
    /*============================================================================*/

    /**
     * @brief 电源管理状态（低功耗层级）
     */
    typedef enum
    {
        KBD_PM_ACTIVE = 0, /**< 活跃：RGB/BLE/采样正常；BLE 在事件之间由 idleCB 自动睡眠 */
        KBD_PM_LIGHT = 1,  /**< 浅休眠：RGB 断电 + 停 TMR0 + 停 ADC；BLE 保持 */
        KBD_PM_DEEP = 2,   /**< 深休眠：LowPower_Shutdown；GPIO 按键唤醒 = 复位 */
    } kbd_pm_state_t;

    /**
     * @brief 模式切换回调函数类型
     * @param new_mode 新的工作模式
     */
    typedef void (*kbd_mode_change_cb_t)(kbd_work_mode_t new_mode);

    /**
     * @brief 连接状态变化回调函数类型
     * @param state 新的连接状态
     */
    typedef void (*kbd_conn_state_cb_t)(kbd_conn_state_t state);

    /**
     * @brief LED 输出报告回调函数类型
     * @param leds LED 状态位图
     */
    typedef void (*kbd_led_report_cb_t)(uint8_t leds);

    /**
     * @brief 模式管理器回调结构
     */
    typedef struct
    {
        kbd_mode_change_cb_t onModeChange;     /**< 模式切换回调 */
        kbd_conn_state_cb_t onConnStateChange; /**< 连接状态变化回调 */
        kbd_led_report_cb_t onLedReport;       /**< LED 报告回调 */
    } kbd_mode_callbacks_t;

    /*============================================================================*/
    /* 初始化 API */
    /*============================================================================*/

    /**
     * @brief 初始化模式管理器
     * @param initial_mode 初始工作模式
     * @param pCBs 回调函数指针（可为 NULL）
     * @return 0 成功，其他失败
     */
    int KBD_Mode_Init(kbd_work_mode_t initial_mode, kbd_mode_callbacks_t *pCBs);

    /**
     * @brief 模式管理器主循环处理
     * @note  需要在主循环中周期性调用
     */
    void KBD_Mode_Process(void);

    /**
     * @brief 记录用户活动，用于刷新低功耗空闲计时。
     */
    void KBD_Mode_RecordActivity(void);

    /**
     * @brief 请求退出低功耗，由主循环统一执行恢复。
     */
void KBD_Mode_RequestWake(void);

/**
 * @brief 标记某个 FN 长按正在执行休眠，松开该次长按不应立即唤醒。
 */
void KBD_Mode_SuppressWakeForFn(uint8_t fn_id);

    /*============================================================================*/
    /* 模式切换 API */
    /*============================================================================*/

    /**
     * @brief 切换工作模式
     * @param mode 目标模式
     * @return 0 成功，其他失败
     */
    int KBD_Mode_Switch(kbd_work_mode_t mode);

    /**
     * @brief 获取当前工作模式
     * @return 当前模式
     */
    kbd_work_mode_t KBD_Mode_Get(void);

    /**
     * @brief 切换到下一个模式
     * @return 0 成功，其他失败
     */
    int KBD_Mode_Toggle(void);

    /*============================================================================*/
    /* 连接状态 API */
    /*============================================================================*/

    /**
     * @brief 获取当前连接状态
     * @return 连接状态
     */
    kbd_conn_state_t KBD_Mode_GetConnState(void);

    /**
     * @brief 检查是否已连接（可发送报告）
     * @return true 已连接，false 未连接
     */
    bool KBD_Mode_IsConnected(void);
    bool KBD_Mode_IsInputReady(void);

    /*============================================================================*/
    /* 蓝牙控制 API */
    /*============================================================================*/

    /**
     * @brief 开始蓝牙广播
     * @return 0 成功，其他失败
     */
    int KBD_Mode_BLE_StartAdvertising(void);

    /**
     * @brief 停止蓝牙广播
     * @return 0 成功，其他失败
     */
    int KBD_Mode_BLE_StopAdvertising(void);

    /**
     * @brief 断开蓝牙连接
     * @return 0 成功，其他失败
     */
    int KBD_Mode_BLE_Disconnect(void);

    /**
     * @brief 清除所有蓝牙配对信息，并自动进入可重新配对广播状态
     * @note  仅在 BLE 模式下执行；非 BLE 模式下不执行任何操作
     * @return 0 成功，其他失败
     */
    int KBD_Mode_BLE_ClearBonds(void);

    /**
     * @brief 获取蓝牙绑定设备数量
     * @return 绑定设备数量
     */
    uint8_t KBD_Mode_BLE_GetBondCount(void);

    /*============================================================================*/
    /* USB 控制 API */
    /*============================================================================*/

    /**
     * @brief 检测 USB 是否已插入
     * @return true USB 已插入，false 未插入
     */
    bool KBD_Mode_USB_IsPlugged(void);

    /**
     * @brief USB 远程唤醒主机
     * @return 0 成功，其他失败
     */
    int KBD_Mode_USB_Wakeup(void);

    /*============================================================================*/
    /* HID 报告发送 API */
    /*============================================================================*/

    /**
     * @brief 发送键盘报告
     * @param modifier 修饰键位图
     * @param keys 按键数组（最多 6 个）
     * @param key_count 按键数量
     * @return 0 成功，其他失败
     */
    int KBD_Mode_SendKeyboardReport(uint8_t modifier, uint8_t *keys, uint8_t key_count);

    /**
     * @brief 发送单个按键（按下+释放）
     * @param modifier 修饰键
     * @param keycode 按键码
     * @return 0 成功，其他失败
     */
    int KBD_Mode_SendKeyPress(uint8_t modifier, uint8_t keycode);

    /**
     * @brief 释放所有键盘按键
     * @return 0 成功，其他失败
     */
    int KBD_Mode_ReleaseAllKeys(void);

    /**
     * @brief 发送鼠标报告
     * @param buttons 按钮位图
     * @param x X 轴移动量
     * @param y Y 轴移动量
     * @param wheel 滚轮移动量
     * @return 0 成功，其他失败
     */
    int KBD_Mode_SendMouseReport(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);

    /**
     * @brief 发送鼠标点击
     * @param buttons 按钮位图
     * @return 0 成功，其他失败
     */
    int KBD_Mode_SendMouseClick(uint8_t buttons);

    /**
     * @brief 发送多媒体控制报告
     * @param key 多媒体控制码
     * @return 0 成功，其他失败
     */
    int KBD_Mode_SendConsumerReport(uint16_t key);

    /**
     * @brief 发送多媒体按键（按下+释放）
     * @param key 多媒体控制码
     * @return 0 成功，其他失败
     */
    int KBD_Mode_SendConsumerKey(uint16_t key);

    /*============================================================================*/
    /* 低功耗 API */
    /*============================================================================*/

    /**
     * @brief 进入低功耗模式
     */
    void KBD_Mode_EnterSleep(void);

    /**
     * @brief 退出低功耗模式
     */
    void KBD_Mode_ExitSleep(void);

    /**
     * @brief 检查是否处于低功耗模式
     * @return true 低功耗模式，false 正常模式
     */
bool KBD_Mode_IsInSleep(void);

/** 是否将唤醒设备的首个真实按键动作透传给上层。 */
bool KBD_Mode_IsSeamlessWakeEnabled(void);

    /**
     * @brief 当前策略下是否允许进入低功耗。
     * @return true 允许，false 不允许
     */
    bool KBD_Mode_CanEnterLowPower(void);

    /**
     * @brief 获取当前电源管理状态
     */
    kbd_pm_state_t KBD_Mode_GetPMState(void);

    /*============================================================================*/
    /* LED 状态 API */
    /*============================================================================*/

    /**
     * @brief 获取键盘 LED 状态
     * @return LED 状态位图（NumLock, CapsLock, ScrollLock）
     */
    uint8_t KBD_Mode_GetKeyboardLEDs(void);

    /*============================================================================*/
    /* 兼容旧命名（过渡期使用） */
    /*============================================================================*/

#define dual_mode_callbacks_t kbd_mode_callbacks_t

#define DualMode_Init KBD_Mode_Init
#define DualMode_Process KBD_Mode_Process
#define DualMode_SwitchMode KBD_Mode_Switch
#define DualMode_GetMode KBD_Mode_Get
#define DualMode_ToggleMode KBD_Mode_Toggle
#define DualMode_GetConnState KBD_Mode_GetConnState
#define DualMode_IsConnected KBD_Mode_IsConnected
#define DualMode_BLE_StartAdvertising KBD_Mode_BLE_StartAdvertising
#define DualMode_BLE_StopAdvertising KBD_Mode_BLE_StopAdvertising
#define DualMode_BLE_Disconnect KBD_Mode_BLE_Disconnect
#define DualMode_BLE_ClearBonds KBD_Mode_BLE_ClearBonds
#define DualMode_BLE_GetBondCount KBD_Mode_BLE_GetBondCount
#define DualMode_USB_IsPlugged KBD_Mode_USB_IsPlugged
#define DualMode_USB_Wakeup KBD_Mode_USB_Wakeup
#define DualMode_SendKeyboardReport KBD_Mode_SendKeyboardReport
#define DualMode_SendKeyPress KBD_Mode_SendKeyPress
#define DualMode_ReleaseAllKeys KBD_Mode_ReleaseAllKeys
#define DualMode_SendMouseReport KBD_Mode_SendMouseReport
#define DualMode_SendMouseClick KBD_Mode_SendMouseClick
#define DualMode_SendConsumerReport KBD_Mode_SendConsumerReport
#define DualMode_SendConsumerKey KBD_Mode_SendConsumerKey
#define DualMode_EnterSleep KBD_Mode_EnterSleep
#define DualMode_ExitSleep KBD_Mode_ExitSleep
#define DualMode_IsInSleep KBD_Mode_IsInSleep
#define DualMode_GetKeyboardLEDs KBD_Mode_GetKeyboardLEDs

#ifdef __cplusplus
}
#endif

#endif /* __KBD_MODE_H */
