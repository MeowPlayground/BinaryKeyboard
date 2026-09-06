/********************************** (C) COPYRIGHT *******************************
 * File Name          : ble_hid.h
 * Author             : Custom Keyboard Library
 * Version            : V1.0
 * Date               : 2024/11/07
 * Description        : 蓝牙 HID 层封装接口
 *******************************************************************************/

#ifndef BLE_HID_H
#define BLE_HID_H

#ifdef __cplusplus
extern "C" {
#endif

#include "kbd_mode_config.h"
#include "ble_config.h"
#include "hiddev.h"
#include <stdint.h>
#include <stdbool.h>

/* ==================== 类型定义 ==================== */

/**
 * @brief BLE 状态变化回调
 */
typedef void (*ble_state_cb_t)(gapRole_States_t newState);

/**
 * @brief BLE LED 报告回调
 */
typedef void (*ble_led_cb_t)(uint8_t leds);

/**
 * @brief BLE HID 回调结构
 */
typedef struct {
    ble_state_cb_t  onStateChange;      // 状态变化回调
    ble_led_cb_t    onLedReport;        // LED 报告回调
} ble_hid_callbacks_t;

/* ==================== 初始化 API ==================== */

/**
 * @brief 初始化 BLE HID 服务
 * @param pCBs 回调函数
 * @return 0 成功，其他失败
 */
int BLE_HID_Init(ble_hid_callbacks_t *pCBs);

/**
 * @brief BLE HID TMOS 事件处理
 * @param task_id 任务 ID
 * @param events 事件
 * @return 未处理的事件
 */
uint16_t BLE_HID_ProcessEvent(uint8_t task_id, uint16_t events);

/* ==================== 连接管理 ==================== */

/**
 * @brief 开始广播
 * @return 0 成功，其他失败
 */
int BLE_HID_StartAdvertising(void);

/**
 * @brief 停止广播
 * @return 0 成功，其他失败
 */
int BLE_HID_StopAdvertising(void);

/**
 * @brief 设置断链后是否自动恢复广播
 * @param enable true=自动恢复广播，false=保持静默
 */
void BLE_HID_SetAutoResumeAdvertising(bool enable);

/**
 * @brief 断开连接
 * @return 0 成功，其他失败
 */
int BLE_HID_Disconnect(void);

/**
 * @brief 获取连接状态
 * @return true 已连接，false 未连接
 */
bool BLE_HID_IsConnected(void);
bool BLE_HID_IsKeyboardReady(void);

/**
 * @brief 获取 GAP 角色状态
 * @return GAP 角色状态
 */
gapRole_States_t BLE_HID_GetState(void);

/**
 * @brief 清除所有绑定
 * @return 0 成功，其他失败
 */
int BLE_HID_ClearBonds(void);

/**
 * @brief 获取绑定设备数量
 * @return 绑定数量
 */
uint8_t BLE_HID_GetBondCount(void);

/* ==================== HID 报告发送 ==================== */

/**
 * @brief 发送键盘报告
 * @param modifier 修饰键
 * @param keys 按键数组
 * @param key_count 按键数量
 * @return 0 成功，其他失败
 */
int BLE_HID_SendKeyboardReport(uint8_t modifier, uint8_t *keys, uint8_t key_count);

/**
 * @brief 发送鼠标报告
 * @param buttons 按钮
 * @param x X 移动
 * @param y Y 移动
 * @param wheel 滚轮
 * @return 0 成功，其他失败
 */
int BLE_HID_SendMouseReport(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);

/**
 * @brief 发送多媒体报告
 * @param key 控制码
 * @return 0 成功，其他失败
 */
int BLE_HID_SendConsumerReport(uint16_t key);

/**
 * @brief 获取键盘 LED 状态
 * @return LED 状态位图
 */
uint8_t BLE_HID_GetKeyboardLEDs(void);

/* ==================== 内部使用 ==================== */

// TMOS 任务 ID
extern uint8_t bleHidTaskId;

// 事件定义
#define BLE_HID_PARAM_UPDATE_EVT        0x0001
#define BLE_HID_PHY_UPDATE_EVT          0x0002
#define BLE_HID_SECURITY_REQ_EVT        0x0004

#ifdef __cplusplus
}
#endif

#endif /* BLE_HID_H */
