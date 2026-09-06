/**
 * @file    kbd_command.c
 * @brief   MeowKeyboard USB HID 命令处理实现
 * @author  MeowKJ
 * @version V1.0.0
 * @date    2024-11-07
 *
 * @details
 * 实现 USB HID 配置协议，支持上位机对键盘进行配置：
 * - 命令帧格式：[CMD:1][SUB:1][LEN:1][DATA:61]
 * - 响应帧格式：相同
 *
 * @copyright Copyright (c) 2024 MeowKJ. All rights reserved.
 */

#include "kbd_command.h"
#include "kbd_iap.h"
#include "debug.h"
#include "kbd_config.h"
#include "kbd_battery.h"
#include "kbd_mode.h"
#include "kbd_rgb.h"
#include "kbd_log.h"
#include "kbd_storage.h"
#include "key.h"
#include "iap_config.h"
#include "CH59x_common.h"
#include <string.h>

/** @brief 模块日志标签 */
#define TAG "CMD"

static kbd_command_response_sender_t s_response_sender = NULL;

/*============================================================================*/
/*                              外部函数声明 */
/*============================================================================*/

/**
 * @brief USB HID 配置响应发送函数
 * @note 在 usb_hid.c 中实现
 */
extern void USB_Config_SendResponse(uint8_t cmd, uint8_t *data, uint8_t len);

/*============================================================================*/
/*                              私有函数声明 */
/*============================================================================*/

static void HandleSysInfo(const kbd_cmd_frame_t *frame);
static void HandleSysStatus(const kbd_cmd_frame_t *frame);
static void HandleCfgSave(const kbd_cmd_frame_t *frame);
static void HandleCfgLoad(const kbd_cmd_frame_t *frame);
static void HandleCfgReset(const kbd_cmd_frame_t *frame);
static void HandleCfgOsGet(const kbd_cmd_frame_t *frame);
static void HandleCfgOsSet(const kbd_cmd_frame_t *frame);
static void HandleKeymapGet(const kbd_cmd_frame_t *frame);
static void HandleKeymapSet(const kbd_cmd_frame_t *frame);
static void HandleLayerGet(const kbd_cmd_frame_t *frame);
static void HandleLayerSet(const kbd_cmd_frame_t *frame);
static void HandleRgbGet(const kbd_cmd_frame_t *frame);
static void HandleRgbSet(const kbd_cmd_frame_t *frame);
static void HandleMacroInfo(const kbd_cmd_frame_t *frame);
static void HandleMacroGet(const kbd_cmd_frame_t *frame);
static void HandleMacroSet(const kbd_cmd_frame_t *frame);
static void HandleMacroDel(const kbd_cmd_frame_t *frame);
static void HandleFnkeyGet(const kbd_cmd_frame_t *frame);
static void HandleFnkeySet(const kbd_cmd_frame_t *frame);
static void HandleBattery(const kbd_cmd_frame_t *frame);
static void HandleLogGet(const kbd_cmd_frame_t *frame);
static void HandleLogSet(const kbd_cmd_frame_t *frame);
static void HandleDataFlashInfo(const kbd_cmd_frame_t *frame);
static void HandleDataFlashRead(const kbd_cmd_frame_t *frame);
static void HandleDataFlashWrite(const kbd_cmd_frame_t *frame);

/*============================================================================*/
/*                              公共函数实现 */
/*============================================================================*/

void KBD_Command_Init(void) { LOG_I(TAG, "Command handler init"); }

void KBD_Command_SetResponseSender(kbd_command_response_sender_t sender)
{
  s_response_sender = sender;
}

int KBD_Command_Process(const kbd_cmd_frame_t *frame)
{
  switch (frame->cmd)
  {
  /* 系统命令 */
  case KBD_CMD_SYS_INFO:
    HandleSysInfo(frame);
    break;
  case KBD_CMD_SYS_STATUS:
    HandleSysStatus(frame);
    break;

  /* 配置管理 */
  case KBD_CMD_CFG_SAVE:
    HandleCfgSave(frame);
    break;
  case KBD_CMD_CFG_LOAD:
    HandleCfgLoad(frame);
    break;
  case KBD_CMD_CFG_RESET:
    HandleCfgReset(frame);
    break;
  case KBD_CMD_CFG_OS_GET:
    HandleCfgOsGet(frame);
    break;
  case KBD_CMD_CFG_OS_SET:
    HandleCfgOsSet(frame);
    break;

  /* 按键映射 */
  case KBD_CMD_KEYMAP_GET:
    HandleKeymapGet(frame);
    break;
  case KBD_CMD_KEYMAP_SET:
    HandleKeymapSet(frame);
    break;
  case KBD_CMD_LAYER_GET:
    HandleLayerGet(frame);
    break;
  case KBD_CMD_LAYER_SET:
    HandleLayerSet(frame);
    break;

  /* RGB 控制 */
  case KBD_CMD_RGB_GET:
    HandleRgbGet(frame);
    break;
  case KBD_CMD_RGB_SET:
    HandleRgbSet(frame);
    break;

  /* 宏管理 */
  case KBD_CMD_MACRO_INFO:
    HandleMacroInfo(frame);
    break;
  case KBD_CMD_MACRO_GET:
    HandleMacroGet(frame);
    break;
  case KBD_CMD_MACRO_SET:
    HandleMacroSet(frame);
    break;
  case KBD_CMD_MACRO_DEL:
    HandleMacroDel(frame);
    break;

  /* FN 键配置 */
  case KBD_CMD_FNKEY_GET:
    HandleFnkeyGet(frame);
    break;
  case KBD_CMD_FNKEY_SET:
    HandleFnkeySet(frame);
    break;

  /* 电池信息 */
  case KBD_CMD_BATTERY:
    HandleBattery(frame);
    break;

  /* 日志配置 */
  case KBD_CMD_LOG_GET:
    HandleLogGet(frame);
    break;
  case KBD_CMD_LOG_SET:
    HandleLogSet(frame);
    break;

  /* DataFlash 调试 */
  case KBD_CMD_DATAFLASH_INFO:
    HandleDataFlashInfo(frame);
    break;
  case KBD_CMD_DATAFLASH_READ:
    HandleDataFlashRead(frame);
    break;
  case KBD_CMD_DATAFLASH_WRITE:
    HandleDataFlashWrite(frame);
    break;

  /* IAP 固件更新 */
  case KBD_CMD_IAP_INFO:
  case KBD_CMD_IAP_PREPARE:
  case KBD_CMD_IAP_WRITE:
  case KBD_CMD_IAP_VERIFY:
  case KBD_CMD_IAP_ACTIVATE:
    KBD_IAP_Process(frame);
    break;

  default:
    LOG_W(TAG, "Unknown cmd 0x%02X", frame->cmd);
    {
      uint8_t resp[1] = {KBD_RESP_ERR_INVALID};
      KBD_Command_SendResponse(frame->cmd, 0, resp, 1);
    }
    return -1;
  }

  return 0;
}

void KBD_Command_SendResponse(uint8_t cmd, uint8_t sub, const uint8_t *data,
                              uint8_t len)
{
  uint8_t frame[64];
  uint8_t copy_len = 0;
  memset(frame, 0, sizeof(frame));
  frame[0] = cmd;
  frame[1] = sub;
  frame[2] = len;
  if (data && len > 0)
  {
    copy_len = (len > 61) ? 61 : len;
    memcpy(&frame[3], data, copy_len);
  }

  if (s_response_sender)
  {
    s_response_sender(frame, sizeof(frame));
    return;
  }

  /* USB_ConfigReport_t 格式: [CMD:1][DATA:63] */
  USB_Config_SendResponse(cmd, &frame[1], copy_len + 2);
}

/*============================================================================*/
/*                              命令处理实现 */
/*============================================================================*/

/**
 * @brief 处理系统信息查询
 *
 * 响应格式 (18 字节):
 * [0]  KBD_RESP_OK
 * [1]  VID 高字节
 * [2]  VID 低字节
 * [3]  PID 高字节
 * [4]  PID 低字节
 * [5]  固件主版本
 * [6]  固件次版本
 * [7]  固件补丁版本
 * [8]  最大层数
 * [9]  最大按键数 (单层)
 * [10] 宏槽位数
 * [11] 键盘类型 (kbd_type_t)
 * [12] 实际按键数 (当前类型)
 * [13] FN 键数量
 * [14] 保留
 * [15] 保留
 * [16] 保留
 * [17] 保留
 */
static void HandleSysInfo(const kbd_cmd_frame_t *frame)
{
  uint8_t resp[18];
  resp[0] = KBD_RESP_OK;
  resp[1] = (KBD_VENDOR_ID >> 8) & 0xFF;
  resp[2] = KBD_VENDOR_ID & 0xFF;
  resp[3] = (KBD_PRODUCT_ID >> 8) & 0xFF;
  resp[4] = KBD_PRODUCT_ID & 0xFF;
  resp[5] = KBD_VERSION_MAJOR;
  resp[6] = KBD_VERSION_MINOR;
  resp[7] = KBD_VERSION_PATCH;
  resp[8] = KBD_GetDefaultLayers();
  resp[9] = KBD_MAX_KEYS;
  resp[10] = Kbd_Macro_GetUsedCount();
  resp[11] = (uint8_t)KBD_GetType(); /* 键盘类型 */
  resp[12] = KBD_GetTotalKeyCount(); /* 实际键位数 */
  resp[13] = KBD_FN_NUM_KEYS;        /* FN 键数量 */
  resp[14] = 0;
  resp[15] = 0;
  resp[16] = 0;
  resp[17] = 0;

  KBD_Command_SendResponse(KBD_CMD_SYS_INFO, 0, resp, 18);
  LOG_D(TAG, "SysInfo sent: type=%d keys=%d", resp[11], resp[12]);
}

/**
 * @brief 处理系统状态查询
 */
static void HandleSysStatus(const kbd_cmd_frame_t *frame)
{
  uint8_t resp[9];
  uint16_t adc_raw = KBD_Battery_GetAdcRaw();
  uint8_t charge_pin_raw = KBD_Battery_GetChargePinRaw();
  kbd_charge_state_t charge_state = KBD_Battery_GetChargeState();
  resp[0] = KBD_RESP_OK;
  resp[1] = (uint8_t)DualMode_GetMode();
  resp[2] = (uint8_t)DualMode_GetConnState();
  resp[3] = KBD_GetCurrentLayer();
  resp[4] = KBD_Battery_GetLevel();
  resp[5] = (uint8_t)charge_state;
  resp[6] = (uint8_t)(adc_raw & 0xFF);
  resp[7] = (uint8_t)((adc_raw >> 8) & 0xFF);
  resp[8] = charge_pin_raw;

  KBD_Command_SendResponse(KBD_CMD_SYS_STATUS, 0, resp, 9);
}

/**
 * @brief 处理配置保存
 */
static void HandleCfgSave(const kbd_cmd_frame_t *frame)
{
  int ret = KBD_Config_Save();
  uint8_t resp[1] = {(ret == 0) ? KBD_RESP_OK : KBD_RESP_ERR_FLASH};
  KBD_Command_SendResponse(KBD_CMD_CFG_SAVE, 0, resp, 1);
  LOG_I(TAG, "Config save: %d", ret);
}

/**
 * @brief 处理配置加载
 */
static void HandleCfgLoad(const kbd_cmd_frame_t *frame)
{
  int ret = KBD_Config_Load();
  uint8_t resp[1] = {(ret == 0) ? KBD_RESP_OK : KBD_RESP_ERR_FLASH};
  KBD_Command_SendResponse(KBD_CMD_CFG_LOAD, 0, resp, 1);
  LOG_I(TAG, "Config load: %d", ret);
}

/**
 * @brief 处理配置重置
 */
static void HandleCfgReset(const kbd_cmd_frame_t *frame)
{
  int ret = KBD_Config_Reset();
  uint8_t resp[1] = {(ret == 0) ? KBD_RESP_OK : KBD_RESP_ERR_FLASH};
  KBD_Command_SendResponse(KBD_CMD_CFG_RESET, 0, resp, 1);
  LOG_I(TAG, "Config reset: %d", ret);
}

static void HandleCfgOsGet(const kbd_cmd_frame_t *frame)
{
  uint8_t resp[2] = {KBD_RESP_OK, KBD_GetOsMode()};
  KBD_Command_SendResponse(KBD_CMD_CFG_OS_GET, 0, resp, 2);
}

static void HandleCfgOsSet(const kbd_cmd_frame_t *frame)
{
  uint8_t resp[1] = {KBD_RESP_OK};
  uint8_t mode = (frame->len >= 1) ? frame->data[0] : 0xFF;

  if (frame->len < 1 || KBD_SetOsMode(mode) != 0) {
    resp[0] = KBD_RESP_ERR_PARAM;
  }

  KBD_Command_SendResponse(KBD_CMD_CFG_OS_SET, 0, resp, 1);
  LOG_I(TAG, "OS mode set: %d status=%d", mode, resp[0]);
}

/**
 * @brief 处理按键映射获取
 */
static void HandleKeymapGet(const kbd_cmd_frame_t *frame)
{
  uint8_t layer = frame->sub;
  kbd_keymap_t *keymap = KBD_GetKeymap();

  if (layer >= keymap->num_layers)
  {
    uint8_t resp[1] = {KBD_RESP_ERR_PARAM};
    KBD_Command_SendResponse(KBD_CMD_KEYMAP_GET, layer, resp, 1);
    return;
  }

  uint8_t resp[40];
  resp[0] = KBD_RESP_OK;
  resp[1] = keymap->num_layers;
  resp[2] = keymap->current_layer;
  resp[3] = keymap->default_layer;
  memcpy(&resp[4], &keymap->layers[layer], sizeof(kbd_layer_t));

  KBD_Command_SendResponse(KBD_CMD_KEYMAP_GET, layer, resp,
                           4 + sizeof(kbd_layer_t));
}

/**
 * @brief 处理按键映射设置
 */
static void HandleKeymapSet(const kbd_cmd_frame_t *frame)
{
  uint8_t layer = frame->sub;
  kbd_keymap_t *keymap = KBD_GetKeymap();
  const uint8_t expected_len = 3 + sizeof(kbd_layer_t);

  if (layer >= KBD_GetDefaultLayers())
  {
    uint8_t resp[1] = {KBD_RESP_ERR_PARAM};
    KBD_Command_SendResponse(KBD_CMD_KEYMAP_SET, layer, resp, 1);
    return;
  }

  /* 数据格式: [numLayers:1][reserved:1][defaultLayer:1][layer_data:32] = 35
   * bytes */
  if (frame->len >= expected_len)
  {
    uint8_t num_layers = frame->data[0];
    uint8_t default_layer = frame->data[2];

    LOG_D(TAG, "Keymap set: layer=%d numLayers=%d defLayer=%d len=%d", layer,
          num_layers, default_layer, frame->len);

    if (num_layers > 0 && num_layers <= KBD_GetDefaultLayers())
    {
      keymap->num_layers = num_layers;
    }
    if (default_layer < keymap->num_layers)
    {
      keymap->default_layer = default_layer;
    }

    memcpy(&keymap->layers[layer], &frame->data[3], sizeof(kbd_layer_t));
  }
  else
  {
    uint8_t resp[1] = {KBD_RESP_ERR_PARAM};
    LOG_W(TAG, "Keymap data too short: len=%d need=%d", frame->len,
          expected_len);
    KBD_Command_SendResponse(KBD_CMD_KEYMAP_SET, layer, resp, 1);
    return;
  }

  uint8_t resp[1] = {KBD_RESP_OK};
  KBD_Command_SendResponse(KBD_CMD_KEYMAP_SET, layer, resp, 1);
  LOG_D(TAG, "Keymap set: layer=%d", layer);
}

/**
 * @brief 处理当前层获取
 */
static void HandleLayerGet(const kbd_cmd_frame_t *frame)
{
  kbd_keymap_t *keymap = KBD_GetKeymap();

  uint8_t resp[4];
  resp[0] = KBD_RESP_OK;
  resp[1] = keymap->current_layer;
  resp[2] = keymap->num_layers;
  resp[3] = keymap->default_layer;

  KBD_Command_SendResponse(KBD_CMD_LAYER_GET, 0, resp, 4);
}

/**
 * @brief 处理当前层设置
 */
static void HandleLayerSet(const kbd_cmd_frame_t *frame)
{
  if (frame->len < 1)
  {
    uint8_t resp[2] = {KBD_RESP_ERR_PARAM, KBD_GetCurrentLayer()};
    LOG_W(TAG, "Layer set rejected: len=%d", frame->len);
    KBD_Command_SendResponse(KBD_CMD_LAYER_SET, 0, resp, 2);
    return;
  }

  uint8_t layer = frame->data[0];
  uint8_t old_layer = KBD_GetCurrentLayer();
  int ret = KBD_SetCurrentLayer(layer);
  uint8_t new_layer = KBD_GetCurrentLayer();

  if (ret == 0 && new_layer != old_layer)
  {
    KBD_Log_LayerEvent(old_layer, new_layer);
    KBD_RGB_FlashLayer(new_layer);
  }

  uint8_t resp[2];
  resp[0] = (ret == 0) ? KBD_RESP_OK : KBD_RESP_ERR_PARAM;
  resp[1] = new_layer;

  KBD_Command_SendResponse(KBD_CMD_LAYER_SET, 0, resp, 2);
  LOG_D(TAG, "Layer switch: %d", layer);
}

/**
 * @brief 处理 RGB 配置获取
 */
static void HandleRgbGet(const kbd_cmd_frame_t *frame)
{
  kbd_rgb_config_t *rgb = KBD_GetRgbConfig();
  kbd_system_config_t *sys = KBD_GetSystemConfig();

  uint8_t resp[14];
  resp[0] = KBD_RESP_OK;
  resp[1] = rgb->enabled;
  resp[2] = rgb->mode;
  resp[3] = rgb->brightness;
  resp[4] = rgb->speed;
  resp[5] = rgb->color_r;
  resp[6] = rgb->color_g;
  resp[7] = rgb->color_b;
  resp[8] = rgb->indicator_enabled;
  resp[9] = rgb->indicator_brightness;
  resp[10] = rgb->press_effect;
  resp[11] = sys->auto_sleep_min;
  resp[12] = sys->deep_sleep_min;
  resp[13] = (sys->seamless_wake != KBD_SEAMLESS_WAKE_DISABLED) ? 1u : 0u;

  KBD_Command_SendResponse(KBD_CMD_RGB_GET, 0, resp, 14);
}

/**
 * @brief 处理 RGB 配置设置
 */
static void HandleRgbSet(const kbd_cmd_frame_t *frame)
{
  kbd_rgb_config_t *rgb = KBD_GetRgbConfig();
  kbd_system_config_t *sys = KBD_GetSystemConfig();
  const uint8_t min_len = 12;

  if (frame->len < min_len)
  {
    uint8_t resp[1] = {KBD_RESP_ERR_PARAM};
    LOG_W(TAG, "RGB set data invalid: len=%d need>=%d", frame->len, min_len);
    KBD_Command_SendResponse(KBD_CMD_RGB_SET, 0, resp, 1);
    return;
  }

  rgb->enabled = frame->data[0];
  rgb->mode = frame->data[1];
  rgb->brightness = frame->data[2];
  rgb->speed = frame->data[3];
  rgb->color_r = frame->data[4];
  rgb->color_g = frame->data[5];
  rgb->color_b = frame->data[6];
  rgb->indicator_enabled = 1; /* 指示灯始终启用，不允许用户关闭 */

  {
    uint8_t v = frame->data[8];
    rgb->indicator_brightness =
        (v < KBD_INDICATOR_MIN_BRIGHTNESS) ? KBD_INDICATOR_MIN_BRIGHTNESS : v;
  }

  {
    uint8_t pe = frame->data[9];
    rgb->press_effect = (pe <= 2) ? pe : 0;
  }

  sys->auto_sleep_min = frame->data[10];
  sys->deep_sleep_min = frame->data[11];
  if (frame->len >= 13u)
  {
    sys->seamless_wake = frame->data[12]
                             ? KBD_SEAMLESS_WAKE_ENABLED
                             : KBD_SEAMLESS_WAKE_DISABLED;
  }

  KBD_RGB_SetMode((kbd_rgb_mode_t)rgb->mode);
  KBD_RGB_SetBrightness(rgb->brightness);
  KBD_RGB_SetIndicatorBrightness(rgb->indicator_brightness);

  KBD_Config_Save();

  uint8_t resp[1] = {KBD_RESP_OK};
  KBD_Command_SendResponse(KBD_CMD_RGB_SET, 0, resp, 1);
  LOG_D(TAG, "RGB config set");
}

/**
 * @brief 处理宏信息查询
 */
static void HandleMacroInfo(const kbd_cmd_frame_t *frame)
{
  uint16_t total = Kbd_Macro_GetTotalSize();
  uint16_t page = Kbd_Macro_GetPageSize();
  uint16_t free = Kbd_Macro_GetFreeBytes();
  uint8_t resp[8];

  resp[0] = KBD_RESP_OK;
  resp[1] = (uint8_t)(total >> 8);
  resp[2] = (uint8_t)(total & 0xFF);
  resp[3] = (uint8_t)(page >> 8);
  resp[4] = (uint8_t)(page & 0xFF);
  resp[5] = Kbd_Macro_GetUsedCount();
  resp[6] = (uint8_t)(free >> 8);
  resp[7] = (uint8_t)(free & 0xFF);
  KBD_Command_SendResponse(KBD_CMD_MACRO_INFO, frame->sub, resp, sizeof(resp));
}

/**
 * @brief 处理宏数据读取
 */
static void HandleMacroGet(const kbd_cmd_frame_t *frame)
{
  uint16_t offset = (frame->data[0] << 8) | frame->data[1];
  uint8_t req_len = frame->data[2];

  if (req_len > 59)
    req_len = 59;

  uint8_t resp[64];
  int read_len = Kbd_Macro_ReadRaw(offset, &resp[2], req_len);

  if (read_len >= 0)
  {
    resp[0] = KBD_RESP_OK;
    resp[1] = (uint8_t)read_len;
    KBD_Command_SendResponse(KBD_CMD_MACRO_GET, frame->sub, resp, 2 + read_len);
  }
  else
  {
    resp[0] = KBD_RESP_ERR_NOT_FOUND;
    KBD_Command_SendResponse(KBD_CMD_MACRO_GET, frame->sub, resp, 1);
  }
}

/**
 * @brief 处理宏数据写入
 */
static void HandleMacroSet(const kbd_cmd_frame_t *frame)
{
  uint8_t resp[1];

  if (frame->sub == 0)
  {
    uint8_t page = frame->data[0];
    /* 延迟到 TMOS 主循环执行 Flash 擦除，完成后自动发送响应 */
    if (Kbd_Macro_EraseDeferred(page, frame->sub) != 0)
    {
      resp[0] = KBD_RESP_ERR_FLASH;
      KBD_Command_SendResponse(KBD_CMD_MACRO_SET, frame->sub, resp, 1);
    }
    return;
  }

  if (frame->sub == 1)
  {
    uint16_t offset = (frame->data[0] << 8) | frame->data[1];
    uint8_t len = frame->data[2];
    if (len > (uint8_t)(frame->len - 3))
    {
      len = (uint8_t)(frame->len - 3);
    }

    /* 延迟到 TMOS 主循环执行 Flash 写入，完成后自动发送响应 */
    if (Kbd_Macro_WriteRawDeferred(offset, &frame->data[3], len,
                                   frame->sub) != 0)
    {
      resp[0] = KBD_RESP_ERR_FLASH;
      KBD_Command_SendResponse(KBD_CMD_MACRO_SET, frame->sub, resp, 1);
    }
    return;
  }

  resp[0] = KBD_RESP_ERR_PARAM;
  KBD_Command_SendResponse(KBD_CMD_MACRO_SET, frame->sub, resp, 1);
}

/**
 * @brief 处理宏删除
 */
static void HandleMacroDel(const kbd_cmd_frame_t *frame)
{
  uint8_t slot = frame->sub;
  int ret = Kbd_Macro_Delete(slot);

  uint8_t resp[1] = {(ret == 0) ? KBD_RESP_OK : KBD_RESP_ERR_PARAM};
  KBD_Command_SendResponse(KBD_CMD_MACRO_DEL, slot, resp, 1);
  LOG_D(TAG, "Macro delete: slot=%d ret=%d", slot, ret);
}

/**
 * @brief 处理 FN 键配置获取
 */
static void HandleFnkeyGet(const kbd_cmd_frame_t *frame)
{
  kbd_fnkey_config_t *fnkey = KBD_GetFnKeyConfig();

  uint8_t resp[36];
  resp[0] = KBD_RESP_OK;
  memcpy(&resp[1], fnkey, sizeof(kbd_fnkey_config_t));

  KBD_Command_SendResponse(KBD_CMD_FNKEY_GET, 0, resp,
                           1 + sizeof(kbd_fnkey_config_t));
}

/**
 * @brief 处理 FN 键配置设置
 */
static void HandleFnkeySet(const kbd_cmd_frame_t *frame)
{
  kbd_fnkey_config_t *fnkey = KBD_GetFnKeyConfig();
  const kbd_fnkey_config_t *req = (const kbd_fnkey_config_t *)frame->data;

  if (frame->len < sizeof(kbd_fnkey_config_t))
  {
    uint8_t resp[1] = {KBD_RESP_ERR_PARAM};
    KBD_Command_SendResponse(KBD_CMD_FNKEY_SET, 0, resp, 1);
    LOG_W(TAG, "FN key config set rejected: len=%d", frame->len);
    return;
  }

  memcpy(fnkey, req, sizeof(kbd_fnkey_config_t));

  /* 网页写入后立即同步阈值，不必重启才能生效。 */
  for (uint8_t i = 0; i < KBD_FN_NUM_KEYS; i++) {
    FnKey_SetLongPressThreshold(i, fnkey->fn[i].long_press_ms);
  }

  uint8_t resp[1] = {KBD_RESP_OK};
  KBD_Command_SendResponse(KBD_CMD_FNKEY_SET, 0, resp, 1);
  LOG_D(TAG, "FN key config set");
}

/**
 * @brief 处理电池信息查询
 *
 * 响应格式 (5 字节):
 * [0] KBD_RESP_OK
 * [1] 电量百分比 (0-100)
 * [2] 充电状态 (0=未充电, 1=充电中)
 * [3..4] 电压 mV, little-endian (如 4150 = 4.15V)
 */
static void HandleBattery(const kbd_cmd_frame_t *frame)
{
  uint8_t resp[8];
  uint16_t mv = KBD_Battery_GetVoltage_mV();
  uint16_t adc_raw = KBD_Battery_GetAdcRaw();
  uint8_t charge_pin_raw = KBD_Battery_GetChargePinRaw();
  kbd_charge_state_t charge_state = KBD_Battery_GetChargeState();
  resp[0] = KBD_RESP_OK;
  resp[1] = KBD_Battery_GetLevel();
  resp[2] = (uint8_t)charge_state;
  resp[3] = (uint8_t)(mv & 0xFF);
  resp[4] = (uint8_t)((mv >> 8) & 0xFF);
  resp[5] = (uint8_t)(adc_raw & 0xFF);
  resp[6] = (uint8_t)((adc_raw >> 8) & 0xFF);
  resp[7] = charge_pin_raw;

  KBD_Command_SendResponse(KBD_CMD_BATTERY, 0, resp, 8);
}

/**
 * @brief 处理日志配置获取
 *
 * 响应格式 (2 字节):
 * [0] KBD_RESP_OK
 * [1] enabled
 */
static void HandleLogGet(const kbd_cmd_frame_t *frame)
{
  uint8_t resp[2];
  resp[0] = KBD_RESP_OK;
  resp[1] = KBD_Log_IsEnabled();

  KBD_Command_SendResponse(KBD_CMD_LOG_GET, 0, resp, 2);
}

/**
 * @brief 处理日志配置设置
 *
 * 请求格式 (1 字节):
 * [0] enabled (0=关, 非0=开)
 */
static void HandleLogSet(const kbd_cmd_frame_t *frame)
{
  if (frame->len >= 1)
  {
    KBD_Log_SetEnabled(frame->data[0]);
    LOG_D(TAG, "Log enabled=%d", frame->data[0]);
  }

  uint8_t resp[1] = {KBD_RESP_OK};
  KBD_Command_SendResponse(KBD_CMD_LOG_SET, 0, resp, 1);
}

/**
 * @brief 获取 DataFlash 布局信息
 *
 * 响应格式:
 * [0]  OK
 * [1..2]  DataFlash 总容量
 * [3..4]  页大小
 * [5..6]  配置槽区结束
 * [7..8]  runtime 热数据区起始
 * [9..10] 宏区起始
 * [11..12] 宏区大小
 * [13..14] BLE SNV 起始
 * [15..16] BLE SNV 大小
 */
static void HandleDataFlashInfo(const kbd_cmd_frame_t *frame)
{
  const uint16_t total = 0x8000u;
  const uint16_t page = EEPROM_PAGE_SIZE;
  const uint16_t config_end = 0x0C00u;
  const uint16_t runtime_base = 0x0C00u;
  const uint16_t ble_snv_base = 0x7E00u;
  const uint16_t ble_snv_size = 0x0100u;
  uint8_t resp[17];

  resp[0] = KBD_RESP_OK;
  resp[1] = (uint8_t)(total >> 8);
  resp[2] = (uint8_t)(total & 0xFF);
  resp[3] = (uint8_t)(page >> 8);
  resp[4] = (uint8_t)(page & 0xFF);
  resp[5] = (uint8_t)(config_end >> 8);
  resp[6] = (uint8_t)(config_end & 0xFF);
  resp[7] = (uint8_t)(runtime_base >> 8);
  resp[8] = (uint8_t)(runtime_base & 0xFF);
  resp[9] = (uint8_t)(KBD_FLASH_MACRO_BASE >> 8);
  resp[10] = (uint8_t)(KBD_FLASH_MACRO_BASE & 0xFF);
  resp[11] = (uint8_t)(KBD_FLASH_MACRO_SIZE >> 8);
  resp[12] = (uint8_t)(KBD_FLASH_MACRO_SIZE & 0xFF);
  resp[13] = (uint8_t)(ble_snv_base >> 8);
  resp[14] = (uint8_t)(ble_snv_base & 0xFF);
  resp[15] = (uint8_t)(ble_snv_size >> 8);
  resp[16] = (uint8_t)(ble_snv_size & 0xFF);

  KBD_Command_SendResponse(KBD_CMD_DATAFLASH_INFO, 0, resp, sizeof(resp));
}

/**
 * @brief 读取 DataFlash 原始字节
 *
 * 请求格式: [offset_hi][offset_lo][len]，单帧最多 58 字节。
 * 响应格式: [OK][read_len][data...]
 */
static void HandleDataFlashRead(const kbd_cmd_frame_t *frame)
{
  const uint16_t total = 0x8000u;
  uint8_t resp[61];
  uint16_t offset;
  uint8_t req_len;

  if (frame->len < 3)
  {
    resp[0] = KBD_RESP_ERR_PARAM;
    KBD_Command_SendResponse(KBD_CMD_DATAFLASH_READ, frame->sub, resp, 1);
    return;
  }

  offset = ((uint16_t)frame->data[0] << 8) | frame->data[1];
  req_len = frame->data[2];
  if (req_len > 58)
  {
    req_len = 58;
  }

  if (req_len == 0 || offset >= total || ((uint32_t)offset + req_len) > total)
  {
    resp[0] = KBD_RESP_ERR_PARAM;
    KBD_Command_SendResponse(KBD_CMD_DATAFLASH_READ, frame->sub, resp, 1);
    return;
  }

  if (EEPROM_READ(offset, &resp[2], req_len) != 0)
  {
    resp[0] = KBD_RESP_ERR_FLASH;
    KBD_Command_SendResponse(KBD_CMD_DATAFLASH_READ, frame->sub, resp, 1);
    return;
  }

  resp[0] = KBD_RESP_OK;
  resp[1] = req_len;
  KBD_Command_SendResponse(KBD_CMD_DATAFLASH_READ, frame->sub, resp,
                           (uint8_t)(2 + req_len));
}

/**
 * @brief 写入 DataFlash 单字节（危险）
 *
 * 请求格式: [offset_hi][offset_lo][value]
 * 响应格式: [OK][offset_hi][offset_lo][value]
 */
static void HandleDataFlashWrite(const kbd_cmd_frame_t *frame)
{
  const uint16_t total = 0x8000u;
  uint8_t resp[4];
  uint16_t offset;
  uint16_t page_addr;
  uint16_t page_off;
  __attribute__((aligned(4))) uint8_t page[EEPROM_PAGE_SIZE];

  if (frame->len < 3)
  {
    resp[0] = KBD_RESP_ERR_PARAM;
    KBD_Command_SendResponse(KBD_CMD_DATAFLASH_WRITE, frame->sub, resp, 1);
    return;
  }

  offset = ((uint16_t)frame->data[0] << 8) | frame->data[1];
  if (offset >= total)
  {
    resp[0] = KBD_RESP_ERR_PARAM;
    KBD_Command_SendResponse(KBD_CMD_DATAFLASH_WRITE, frame->sub, resp, 1);
    return;
  }

  page_addr = (uint16_t)(offset & ~(EEPROM_PAGE_SIZE - 1u));
  page_off = (uint16_t)(offset - page_addr);

  if (EEPROM_READ(page_addr, page, sizeof(page)) != 0)
  {
    resp[0] = KBD_RESP_ERR_FLASH;
    KBD_Command_SendResponse(KBD_CMD_DATAFLASH_WRITE, frame->sub, resp, 1);
    return;
  }

  page[page_off] = frame->data[2];

  if (EEPROM_ERASE(page_addr, EEPROM_PAGE_SIZE) != 0 ||
      EEPROM_WRITE(page_addr, page, sizeof(page)) != 0)
  {
    resp[0] = KBD_RESP_ERR_FLASH;
    KBD_Command_SendResponse(KBD_CMD_DATAFLASH_WRITE, frame->sub, resp, 1);
    return;
  }

  resp[0] = KBD_RESP_OK;
  resp[1] = (uint8_t)(offset >> 8);
  resp[2] = (uint8_t)(offset & 0xFF);
  resp[3] = frame->data[2];
  KBD_Command_SendResponse(KBD_CMD_DATAFLASH_WRITE, frame->sub, resp,
                           sizeof(resp));
}
