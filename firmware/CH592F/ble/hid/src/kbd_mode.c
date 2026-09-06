/********************************** (C) COPYRIGHT *******************************
 * File Name          : kbd_mode.c
 * Author             : MeowKJ
 * Version            : V3.0
 * Date               : 2024/11/07
 * Description        : 键盘模式管理器实现
 *                      采用 WCH Application 示例思路：
 *                      模式切换 = 保存模式 + SYS_ResetExecute() 全系统复位
 *                      每种模式只初始化对应的协议栈，避免动态切换冲突
 *******************************************************************************/

#include "kbd_mode.h"
#include "ble_hid.h"
#include "kbd_battery.h"
#include "kbd_rgb.h"
#include "usb_device.h"
#include "usb_hid.h"
#include "kbd_storage.h"
#include "key.h"
#include "debug.h"
#include "ws2812.h"
#include "ble_rtc.h"
#include "ble_sleep.h"
#include <string.h>

#define TAG "MODE"
#define BLE_BOND_CLEAR_SETTLE_MS 100
#define BLE_DEEP_SLEEP_DISCONNECT_SETTLE_MS 30u
#define KBD_LOW_BATTERY_INDICATOR_LEVEL 10u
#define KBD_SLEEP_ENTRY_FLASH_COUNT 3u
#define KBD_SLEEP_ENTRY_FLASH_ON_MS 90u
#define KBD_SLEEP_ENTRY_FLASH_OFF_MS 70u

#if defined(CLK_OSC32K) && (CLK_OSC32K == 1)
#define KBD_RTC_FREQ_HZ 32000u
#else
#define KBD_RTC_FREQ_HZ 32768u
#endif

/*============================================================================*/
/* 私有变量 */
/*============================================================================*/

static kbd_work_mode_t g_current_mode = KBD_WORK_MODE_USB;
static kbd_conn_state_t g_conn_state = KBD_CONN_DISCONNECTED;
static kbd_mode_callbacks_t *g_pCallbacks = NULL;
static uint8_t g_keyboard_leds = 0;
static kbd_pm_state_t g_pm_state = KBD_PM_ACTIVE;
static bool g_mode_switching = false;
static uint32_t g_last_activity_tick = 0;
static bool g_wake_requested = false;
static uint8_t g_sleep_wake_suppress_fn = 0xFF;
#if KBD_SLEEP_TEST_MODE
static uint32_t g_sleep_test_last_bucket = 0;
#endif

/** 报告缓冲区 */
static uint8_t g_kbd_report[KBD_HID_KEYBOARD_REPORT_LEN];
static uint8_t g_mouse_report[KBD_HID_MOUSE_REPORT_LEN];
static uint16_t g_consumer_report;

/*============================================================================*/
/* 私有函数声明 */
/*============================================================================*/

static void KBD_Mode_UpdateConnState(kbd_conn_state_t state);
static void KBD_Mode_BLE_StateCallback(gapRole_States_t newState);
static void KBD_Mode_BLE_LedCallback(uint8_t leds);
static uint32_t KBD_Mode_GetNow(void);
static void KBD_Mode_RecordActivityInternal(void);
static uint32_t KBD_Mode_GetIdleMs(void);
static uint32_t KBD_Mode_GetLightSleepTimeoutMs(void);
static uint32_t KBD_Mode_GetDeepSleepTimeoutMs(void);
static void KBD_Mode_CancelDeepSleepCheck(void);
static void KBD_Mode_ArmDeepSleepCheck(void);
static bool KBD_Mode_IsDeepSleepCheckDue(void);
static bool KBD_Mode_USB_HasProtocolHandshake(void);
static void KBD_Mode_PlaySleepEntryAnimation(void);
#if KBD_SLEEP_TEST_MODE
static void KBD_Mode_RunSleepTestIndicator(void);
#endif
static kbd_state_t KBD_Mode_ResolveIndicatorState(void);
static void KBD_Mode_RefreshIndicatorState(void);
static void KBD_Mode_EnterLightSleep(void);
static void KBD_Mode_ExitLightSleep(void);
static void KBD_Mode_EnterDeepSleep(void);
static bool KBD_Mode_CanEnterDeepSleep(void);

/*============================================================================*/
/* BLE 回调 */
/*============================================================================*/

static ble_hid_callbacks_t g_ble_callbacks = {
    .onStateChange = KBD_Mode_BLE_StateCallback,
    .onLedReport = KBD_Mode_BLE_LedCallback,
};

/*============================================================================*/
/* 初始化实现 */
/*============================================================================*/

int KBD_Mode_Init(kbd_work_mode_t initial_mode, kbd_mode_callbacks_t *pCBs)
{
    int ret = 0;

    g_pCallbacks = pCBs;
    g_current_mode = initial_mode;
    g_conn_state = KBD_CONN_DISCONNECTED;
    g_pm_state = KBD_PM_ACTIVE;
    g_mode_switching = false;
    g_last_activity_tick = KBD_Mode_GetNow();
    g_wake_requested = false;
    KBD_Mode_CancelDeepSleepCheck();
    PFIC_EnableIRQ(RTC_IRQn);

    /* 清空报告缓冲区 */
    memset(g_kbd_report, 0, sizeof(g_kbd_report));
    memset(g_mouse_report, 0, sizeof(g_mouse_report));
    g_consumer_report = 0;

    LOG_I(TAG, "init mode=%d", initial_mode);

    /*
     * 工作模式只决定按键报告发送到哪里：
     * - USB 模式：按键报告走 USB
     * - BLE 模式：按键报告走 BLE
     *
     * BLE 模式也初始化 USB Device。这样无线使用时插上 USB，Studio 仍可通过
     * USB HID 配置接口读写当前键盘配置，不需要先切换到 USB 工作模式。
     */
    if (initial_mode == KBD_WORK_MODE_BLE)
    {
        /* BLE 模式：初始化 BLE HID，广播由 GAPROLE_STARTED 回调自动触发 */
        ret = BLE_HID_Init(&g_ble_callbacks);
        if (ret != 0)
        {
            LOG_E(TAG, "BLE init failed %d", ret);
            return ret;
        }
        USB_Device_Init();
    }
    else
    {
        /* USB 模式：仅初始化 USB */
        USB_Device_Init();
    }

    return 0;
}

void KBD_Mode_Process(void)
{
    /* USB 模式：轮询枚举状态，枚举完成后才置 CONNECTED */
    if (g_current_mode == KBD_WORK_MODE_USB)
    {
        bool usb_configured = (g_USB_DeviceState == USB_STATE_CONFIGURED);
        bool is_connected = (g_conn_state == KBD_CONN_CONNECTED);

        if (usb_configured && !is_connected)
        {
            KBD_Mode_UpdateConnState(KBD_CONN_CONNECTED);
        }
        else if (!usb_configured && is_connected)
        {
            KBD_Mode_UpdateConnState(KBD_CONN_DISCONNECTED);
        }
    }

    if (g_pm_state == KBD_PM_LIGHT)
    {
        uint32_t deep_timeout_ms = KBD_Mode_GetDeepSleepTimeoutMs();

        if (g_sleep_wake_suppress_fn != 0xFF &&
            FnKey_IsDown(g_sleep_wake_suppress_fn) == 0)
        {
            g_sleep_wake_suppress_fn = 0xFF;
        }

        /* BLE LIGHT is slept by the stack scheduler so connection events are
         * not missed. Other transports use CPU idle directly. */
        if (g_wake_requested || !KBD_Mode_CanEnterLowPower())
        {
            KBD_Mode_ExitLightSleep();
            return;
        }

        if ((deep_timeout_ms > 0u) &&
            KBD_Mode_CanEnterDeepSleep() &&
            (KBD_Mode_GetIdleMs() >= deep_timeout_ms))
        {
            KBD_Mode_EnterDeepSleep();
            /* 不返回（Shutdown 后唤醒等价复位） */
        }

        if ((g_current_mode != KBD_WORK_MODE_BLE) &&
            (deep_timeout_ms > 0u) && KBD_Mode_IsDeepSleepCheckDue())
        {
            if (KBD_Mode_GetIdleMs() < deep_timeout_ms)
            {
                KBD_Mode_ArmDeepSleepCheck();
            }
            else if (KBD_Mode_CanEnterDeepSleep())
            {
                KBD_Mode_EnterDeepSleep();
            }
            else
            {
                /* 临时被按住的 FN 或其他条件阻塞时继续等待，不丢失检查。 */
                KBD_Mode_ArmDeepSleepCheck();
            }
        }

        /* In BLE mode TMOS invokes CH59x_LowPower at the next safe radio
         * deadline. Direct WFI here can miss connection events and time out. */
        if (g_current_mode != KBD_WORK_MODE_BLE)
        {
            LowPower_Idle();
        }
        return;
    }

    /* ACTIVE */
#if KBD_SLEEP_TEST_MODE
    KBD_Mode_RunSleepTestIndicator();
#endif
    if ((KBD_Mode_GetLightSleepTimeoutMs() > 0u) &&
        KBD_Mode_CanEnterLowPower() &&
        (KBD_Mode_GetIdleMs() >= KBD_Mode_GetLightSleepTimeoutMs()))
    {
        KBD_Mode_EnterLightSleep();
        return;
    }

    KBD_Mode_RefreshIndicatorState();
}

void KBD_Mode_RecordActivity(void)
{
    KBD_Mode_RecordActivityInternal();
    if (g_pm_state != KBD_PM_ACTIVE)
    {
        KBD_Mode_RequestWake();
    }
}

void KBD_Mode_RequestWake(void)
{
    if (g_sleep_wake_suppress_fn != 0xFF &&
        FnKey_IsDown(g_sleep_wake_suppress_fn) != 0)
    {
        return;
    }
    g_wake_requested = true;
}

void KBD_Mode_SuppressWakeForFn(uint8_t fn_id)
{
    if (fn_id < KBD_FN_NUM_KEYS)
    {
        g_sleep_wake_suppress_fn = fn_id;
    }
}

/*============================================================================*/
/* 模式切换实现（SYS_ResetExecute 方式）                                      */
/*============================================================================*/

int KBD_Mode_Switch(kbd_work_mode_t mode)
{
    if (g_mode_switching)
    {
        return -1;
    }

    if (mode == g_current_mode)
    {
        return 0;
    }

    LOG_I(TAG, "switch %d -> %d (reset)", g_current_mode, mode);
    g_mode_switching = true;

    /* 释放当前所有按键 */
    KBD_Mode_ReleaseAllKeys();

    /* 保存目标模式到 DataFlash */
    KBD_SetLastMode(mode == KBD_WORK_MODE_BLE ? 1 : 0);
    KBD_Storage_FlushRuntime(); /* 立即落盘，确保复位前写入完成 */

    /* 等待 Flash 写入和外设稳定 */
    mDelaymS(10);

    /* 全系统复位，下次启动自动进入新模式 */
    SYS_ResetExecute();

    /* 不会到达这里 */
    return 0;
}

kbd_work_mode_t KBD_Mode_Get(void)
{
    return g_current_mode;
}

int KBD_Mode_Toggle(void)
{
    kbd_work_mode_t new_mode = (g_current_mode == KBD_WORK_MODE_USB) ? KBD_WORK_MODE_BLE : KBD_WORK_MODE_USB;
    return KBD_Mode_Switch(new_mode);
}

/*============================================================================*/
/* 连接状态实现 */
/*============================================================================*/

kbd_conn_state_t KBD_Mode_GetConnState(void)
{
    return g_conn_state;
}

bool KBD_Mode_IsConnected(void)
{
    return (g_conn_state == KBD_CONN_CONNECTED);
}

bool KBD_Mode_IsInputReady(void)
{
    if (g_current_mode == KBD_WORK_MODE_USB)
    {
        return (g_USB_DeviceState == USB_STATE_CONFIGURED);
    }

    return BLE_HID_IsKeyboardReady();
}

static void KBD_Mode_UpdateConnState(kbd_conn_state_t state)
{
    if (state == g_conn_state)
    {
        return;
    }

    g_conn_state = state;
    g_last_activity_tick = KBD_Mode_GetNow();
    LOG_D(TAG, "conn state=%d", state);

    if (g_pm_state == KBD_PM_LIGHT &&
        g_current_mode != KBD_WORK_MODE_BLE)
    {
        KBD_Mode_ArmDeepSleepCheck();
    }

    if (g_pCallbacks && g_pCallbacks->onConnStateChange)
    {
        g_pCallbacks->onConnStateChange(state);
    }
}

/*============================================================================*/
/* 蓝牙控制实现 */
/*============================================================================*/

int KBD_Mode_BLE_StartAdvertising(void)
{
    if (g_current_mode != KBD_WORK_MODE_BLE)
    {
        return -1;
    }
    return BLE_HID_StartAdvertising();
}

int KBD_Mode_BLE_StopAdvertising(void)
{
    return BLE_HID_StopAdvertising();
}

int KBD_Mode_BLE_Disconnect(void)
{
    return BLE_HID_Disconnect();
}

int KBD_Mode_BLE_ClearBonds(void)
{
    int ret;

    if (g_mode_switching)
    {
        return -1;
    }

    /* 非 BLE 模式下不执行任何清配对操作 */
    if (g_current_mode != KBD_WORK_MODE_BLE)
    {
        return -1;
    }

    BLE_HID_StopAdvertising();

    ret = BLE_HID_ClearBonds();
    if (ret != 0)
    {
        KBD_Mode_UpdateConnState(KBD_CONN_DISCONNECTED);
        return ret;
    }

    /*
     * 清除 bond 后直接复位回 BLE 模式。
     * 这样可以避开当前连接/广播状态机残留，并让 BondMgr/白名单从干净状态重新初始化。
     */
    KBD_Mode_UpdateConnState(KBD_CONN_DISCONNECTED);
    KBD_SetLastMode(1);
    KBD_Storage_FlushRuntime();
    mDelaymS(BLE_BOND_CLEAR_SETTLE_MS);
    SYS_ResetExecute();

    return ret;
}

uint8_t KBD_Mode_BLE_GetBondCount(void)
{
    return BLE_HID_GetBondCount();
}

/*============================================================================*/
/* USB 控制实现 */
/*============================================================================*/

bool KBD_Mode_USB_IsPlugged(void)
{
    return (g_USB_DeviceState >= USB_STATE_POWERED);
}

int KBD_Mode_USB_Wakeup(void)
{
    if (g_current_mode != KBD_WORK_MODE_USB)
    {
        return -1;
    }
    USB_Device_Wakeup();
    return 0;
}

/*============================================================================*/
/* HID 报告发送实现 */
/*============================================================================*/

static uint8_t KBD_Mode_ApplyOsModeModifier(uint8_t modifier)
{
    if (KBD_GetOsMode() != KBD_OS_MODE_MAC)
    {
        return modifier;
    }

    uint8_t mapped = modifier;
    mapped &= (uint8_t)~(KBD_MOD_LCTRL | KBD_MOD_LGUI | KBD_MOD_RCTRL | KBD_MOD_RGUI);
    if (modifier & KBD_MOD_LCTRL) mapped |= KBD_MOD_LGUI;
    if (modifier & KBD_MOD_LGUI)  mapped |= KBD_MOD_LCTRL;
    if (modifier & KBD_MOD_RCTRL) mapped |= KBD_MOD_RGUI;
    if (modifier & KBD_MOD_RGUI)  mapped |= KBD_MOD_RCTRL;
    return mapped;
}

int KBD_Mode_SendKeyboardReport(uint8_t modifier, uint8_t *keys, uint8_t key_count)
{
    if (!KBD_Mode_IsConnected())
    {
        return -1;
    }

    KBD_Mode_RecordActivityInternal();
    uint8_t report_modifier = KBD_Mode_ApplyOsModeModifier(modifier);

    /* 构建报告 */
    memset(g_kbd_report, 0, sizeof(g_kbd_report));
    g_kbd_report[0] = report_modifier;
    g_kbd_report[1] = 0; /* Reserved */

    uint8_t count = (key_count > 6) ? 6 : key_count;
    if (keys && count > 0)
    {
        memcpy(&g_kbd_report[2], keys, count);
    }

    if (g_current_mode == KBD_WORK_MODE_USB)
    {
        USB_Keyboard_Press(report_modifier, keys, key_count);
        return 0;
    }
    else
    {
        return BLE_HID_SendKeyboardReport(report_modifier, keys, key_count);
    }
}

int KBD_Mode_SendKeyPress(uint8_t modifier, uint8_t keycode)
{
    uint8_t keys[1] = {keycode};
    int ret;

    ret = KBD_Mode_SendKeyboardReport(modifier, keys, 1);
    if (ret != 0)
        return ret;

    mDelaymS(20);

    return KBD_Mode_ReleaseAllKeys();
}

int KBD_Mode_ReleaseAllKeys(void)
{
    if (g_current_mode == KBD_WORK_MODE_USB)
    {
        USB_Keyboard_Release();
        return 0;
    }
    else
    {
        return BLE_HID_SendKeyboardReport(0, NULL, 0);
    }
}

int KBD_Mode_SendMouseReport(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    if (!KBD_Mode_IsConnected())
    {
        return -1;
    }

    KBD_Mode_RecordActivityInternal();

    if (g_current_mode == KBD_WORK_MODE_USB)
    {
        if (buttons != g_mouse_report[0])
        {
            USB_Mouse_Press(buttons);
        }
        if (x != 0 || y != 0 || wheel != 0)
        {
            USB_Mouse_Move(x, y, wheel);
        }
        g_mouse_report[0] = buttons;
        return 0;
    }
    else
    {
        return BLE_HID_SendMouseReport(buttons, x, y, wheel);
    }
}

int KBD_Mode_SendMouseClick(uint8_t buttons)
{
    int ret;

    ret = KBD_Mode_SendMouseReport(buttons, 0, 0, 0);
    if (ret != 0)
        return ret;

    mDelaymS(50);

    return KBD_Mode_SendMouseReport(0, 0, 0, 0);
}

int KBD_Mode_SendConsumerReport(uint16_t key)
{
    if (!KBD_Mode_IsConnected())
    {
        return -1;
    }

    KBD_Mode_RecordActivityInternal();

    if (g_current_mode == KBD_WORK_MODE_USB)
    {
        if (key != 0)
        {
            USB_Consumer_Press(key);
        }
        else
        {
            USB_Consumer_Release();
        }
        return 0;
    }
    else
    {
        return BLE_HID_SendConsumerReport(key);
    }
}

int KBD_Mode_SendConsumerKey(uint16_t key)
{
    int ret;

    ret = KBD_Mode_SendConsumerReport(key);
    if (ret != 0)
        return ret;

    mDelaymS(50);

    return KBD_Mode_SendConsumerReport(0);
}

/*============================================================================*/
/* 低功耗实现 */
/*============================================================================*/

void KBD_Mode_EnterSleep(void)
{
    KBD_Mode_EnterLightSleep();
}

void KBD_Mode_ExitSleep(void)
{
    KBD_Mode_ExitLightSleep();
}

bool KBD_Mode_IsInSleep(void)
{
    return (g_pm_state != KBD_PM_ACTIVE);
}

bool KBD_Mode_IsSeamlessWakeEnabled(void)
{
    const kbd_system_config_t *sys = KBD_GetSystemConfig();
    return (sys->seamless_wake != KBD_SEAMLESS_WAKE_DISABLED);
}

kbd_pm_state_t KBD_Mode_GetPMState(void)
{
    return g_pm_state;
}

bool KBD_Mode_CanEnterLowPower(void)
{
    if (!KBD_LOW_POWER_ENABLE)
    {
        return false;
    }

    if (KBD_Battery_GetChargeState() == BAT_CHG_CHARGING)
    {
        return false;
    }

    if (KBD_Mode_USB_HasProtocolHandshake())
    {
        return false;
    }

    return true;
}

/** 进入 DEEP 需要额外条件：BLE 模式允许主动断链后再 Shutdown。 */
static bool KBD_Mode_CanEnterDeepSleep(void)
{
    if (!KBD_Mode_CanEnterLowPower())
    {
        return false;
    }

    /* 仅在 USB 已真正完成枚举时阻止 Shutdown；单纯供电不再拦休眠。 */
    if (KBD_Mode_USB_HasProtocolHandshake())
    {
        return false;
    }

    for (uint8_t i = 0; i < KBD_FN_NUM_KEYS; i++)
    {
        if (FnKey_IsDown(i) != 0)
        {
            return false;
        }
    }

    return true;
}

static void KBD_Mode_EnterLightSleep(void)
{
    if (g_pm_state != KBD_PM_ACTIVE)
        return;

    if (!KBD_Mode_CanEnterLowPower())
    {
        LOG_I(TAG, "sleep blocked: usb_handshake=%d charging=%d",
              KBD_Mode_USB_HasProtocolHandshake() ? 1 : 0,
              KBD_Battery_GetChargeState() == BAT_CHG_CHARGING ? 1 : 0);
        KBD_Mode_RefreshIndicatorState();
        return;
    }

    KBD_Mode_PlaySleepEntryAnimation();

    g_pm_state = KBD_PM_LIGHT;
    g_wake_requested = false;
    if (g_current_mode == KBD_WORK_MODE_BLE)
    {
        /* A previous DEEP timer cancellation may have disabled the shared RTC
         * trigger. Restore the stack-owned wake source before enabling sleep. */
        HAL_SleepInit();
        CH59x_LowPowerSetEnabled(1u);
    }
    else
    {
        CH59x_LowPowerSetEnabled(0u);
    }
    LOG_I(TAG, "enter LIGHT");

    KBD_Mode_ReleaseAllKeys();
    KBD_Storage_FlushRuntime(); /* 强制落盘 runtime 热数据，防断电丢失 */
    KBD_RGB_SetSchedulerEnabled(false);
    KBD_RGB_SetLowPower(true); /* 内部 WS2812_Sleep() 切断 LED 电源 + 数据脚高阻 */
    KBD_Battery_Suspend();     /* 关闭 VBAT 分压 + 停止周期性采样 */
    Key_EnterSleep();          /* 停 TMR0，保留 GPIO 中断作为按键唤醒源 */
    if (g_current_mode != KBD_WORK_MODE_BLE)
    {
        KBD_Mode_ArmDeepSleepCheck();
    }
}

static void KBD_Mode_ExitLightSleep(void)
{
    if (g_pm_state != KBD_PM_LIGHT)
        return;

    g_pm_state = KBD_PM_ACTIVE;
    g_wake_requested = false;
    CH59x_LowPowerSetEnabled(0u);
    LOG_I(TAG, "exit LIGHT");

    KBD_Mode_CancelDeepSleepCheck();
    Key_ExitSleep();
    KBD_Battery_Resume();
    KBD_RGB_SetLowPower(false);
    KBD_RGB_SetSchedulerEnabled(true);

    g_last_activity_tick = KBD_Mode_GetNow();
    g_sleep_wake_suppress_fn = 0xFF;
    KBD_Mode_RefreshIndicatorState();
}

static void KBD_Mode_EnterDeepSleep(void)
{
    if (g_pm_state == KBD_PM_ACTIVE)
    {
        KBD_Mode_PlaySleepEntryAnimation();
        KBD_Mode_ReleaseAllKeys();
        KBD_RGB_SetSchedulerEnabled(false);
        KBD_RGB_SetLowPower(true);
        KBD_Battery_Suspend();
        Key_EnterSleep();
    }

    LOG_I(TAG, "enter DEEP (shutdown)");
    g_pm_state = KBD_PM_DEEP;
    g_wake_requested = false;
    CH59x_LowPowerSetEnabled(0u);
    g_sleep_wake_suppress_fn = 0xFF;
    KBD_Mode_CancelDeepSleepCheck();

    /* BLE 模式下先主动断开并抑制自动恢复广播，再执行深睡。 */
    if (g_current_mode == KBD_WORK_MODE_BLE)
    {
        BLE_HID_SetAutoResumeAdvertising(false);
        BLE_HID_Disconnect();
        BLE_HID_StopAdvertising();
        mDelaymS(BLE_DEEP_SLEEP_DISCONNECT_SETTLE_MS);
    }

    /* 持久化（Shutdown 之后 RAM 不保留） */
    KBD_Storage_FlushRuntime();

    /* 配置按键低电平作为 GPIO 唤醒源 */
    Key_ConfigDeepSleepWakeup();

    /* 确保 Flash / 日志等写操作落盘 */
    mDelaymS(5);

    SYS_ResetKeepBuf(KBD_DEEP_WAKE_RESET_MARKER);
    LowPower_Shutdown(0);
    /* Shutdown 唤醒 = 复位，不会返回 */
}

/*============================================================================*/
/* LED 状态实现 */
/*============================================================================*/

uint8_t KBD_Mode_GetKeyboardLEDs(void)
{
    return g_keyboard_leds;
}

/*============================================================================*/
/* 私有函数实现 */
/*============================================================================*/

static void KBD_Mode_BLE_StateCallback(gapRole_States_t newState)
{
    uint8_t state = (newState & GAPROLE_STATE_ADV_MASK);

    if (g_mode_switching || g_current_mode != KBD_WORK_MODE_BLE)
    {
        return;
    }

    if (state == GAPROLE_CONNECTED || state == GAPROLE_CONNECTED_ADV)
    {
        LOG_I(TAG, "BLE connected");
        KBD_Storage_DeferRuntimeSave(5000); /* 推迟 Flash 写入，避免打断 BLE 配对握手 */
        KBD_Mode_UpdateConnState(KBD_CONN_CONNECTED);
        return;
    }

    switch (state)
    {
    case GAPROLE_STARTED:
        LOG_D(TAG, "BLE started");
        break;

    case GAPROLE_ADVERTISING:
        LOG_I(TAG, "BLE advertising");
        KBD_Mode_UpdateConnState(KBD_CONN_ADVERTISING);
        break;

    case GAPROLE_WAITING:
        LOG_I(TAG, "BLE waiting");
        KBD_Mode_UpdateConnState(KBD_CONN_DISCONNECTED);
        break;

    default:
        break;
    }
}

static void KBD_Mode_BLE_LedCallback(uint8_t leds)
{
    g_keyboard_leds = leds;
    KBD_Mode_RecordActivityInternal();

    if (g_pCallbacks && g_pCallbacks->onLedReport)
    {
        g_pCallbacks->onLedReport(leds);
    }
}

static uint32_t KBD_Mode_GetNow(void)
{
    return RTC_GetCycle32k();
}

static void KBD_Mode_RecordActivityInternal(void)
{
    g_last_activity_tick = KBD_Mode_GetNow();
#if KBD_SLEEP_TEST_MODE
    g_sleep_test_last_bucket = 0;
#endif
}

static uint32_t KBD_Mode_GetIdleMs(void)
{
    uint32_t now = KBD_Mode_GetNow();
    uint32_t elapsed;

    if (now >= g_last_activity_tick)
    {
        elapsed = now - g_last_activity_tick;
    }
    else
    {
        /* RTC 32K 计数器回绕，按同一时基补足差值。 */
        elapsed = (RTC_MAX_COUNT - g_last_activity_tick) + now;
    }

    return (uint32_t)(((uint64_t)elapsed * 1000u + (KBD_RTC_FREQ_HZ / 2u)) / KBD_RTC_FREQ_HZ);
}

static uint32_t KBD_Mode_GetLightSleepTimeoutMs(void)
{
    const kbd_system_config_t *sys = KBD_GetSystemConfig();

    if (sys->auto_sleep_min == 0u)
    {
        return 0u;
    }

    return (uint32_t)sys->auto_sleep_min * 60000u;
}

static uint32_t KBD_Mode_GetDeepSleepTimeoutMs(void)
{
    const kbd_system_config_t *sys = KBD_GetSystemConfig();

    if ((sys->auto_sleep_min == 0u) || (sys->deep_sleep_min == 0u))
    {
        return 0u;
    }

    return ((uint32_t)sys->auto_sleep_min + (uint32_t)sys->deep_sleep_min) * 60000u;
}

static void KBD_Mode_CancelDeepSleepCheck(void)
{
    RTC_ModeFunDisable(RTC_TRIG_MODE);
    RTC_ClearITFlag(RTC_TRIG_EVENT);
    RTCTigFlag = 0;
    PWR_PeriphWakeUpCfg(DISABLE, RB_SLP_RTC_WAKE, Long_Delay);
}

static void KBD_Mode_ArmDeepSleepCheck(void)
{
    uint32_t deep_timeout_ms = KBD_Mode_GetDeepSleepTimeoutMs();
    uint32_t idle_ms = KBD_Mode_GetIdleMs();
    uint32_t remain_ms;
    uint32_t rtc_cycles;

    if (deep_timeout_ms == 0u)
    {
        KBD_Mode_CancelDeepSleepCheck();
        return;
    }

    remain_ms = (idle_ms >= deep_timeout_ms)
                    ? 1u
                    : (deep_timeout_ms - idle_ms);
    rtc_cycles = (uint32_t)(((uint64_t)remain_ms * KBD_RTC_FREQ_HZ + 999u) / 1000u);

    if (rtc_cycles == 0u)
    {
        rtc_cycles = 1u;
    }

    KBD_Mode_CancelDeepSleepCheck();
    PWR_PeriphWakeUpCfg(ENABLE, RB_SLP_RTC_WAKE, Long_Delay);
    PFIC_EnableIRQ(RTC_IRQn);
    RTCTigFlag = 0;
    RTC_TRIGFunCfg(rtc_cycles);
}

static bool KBD_Mode_IsDeepSleepCheckDue(void)
{
    if (RTCTigFlag == 0)
    {
        return false;
    }

    RTCTigFlag = 0;
    RTC_ClearITFlag(RTC_TRIG_EVENT);
    return true;
}

static bool KBD_Mode_USB_HasProtocolHandshake(void)
{
    /* USB_STATE_SUSPENDED is numerically greater than CONFIGURED, but it is
     * not an active protocol session and may remain after the cable is
     * removed.  Ordinal comparison therefore creates a false sleep blocker. */
    return (g_USB_DeviceState == USB_STATE_CONFIGURED);
}

static void KBD_Mode_PlaySleepEntryAnimation(void)
{
    for (uint8_t i = 0; i < KBD_SLEEP_ENTRY_FLASH_COUNT; i++)
    {
        /* 固定使用睡眠提示色，避免 BLE 断连等状态色与“将要休眠”混淆。 */
        WS2812_FillKeys(0, 0, 0);
        WS2812_Set_Indicator(KBD_IND_SLEEP_READY_R,
                             KBD_IND_SLEEP_READY_G,
                             KBD_IND_SLEEP_READY_B);
        WS2812_Update();
        mDelaymS(KBD_SLEEP_ENTRY_FLASH_ON_MS);

        WS2812_Clear_Indicator();
        WS2812_Update();

        if ((i + 1u) < KBD_SLEEP_ENTRY_FLASH_COUNT)
        {
            mDelaymS(KBD_SLEEP_ENTRY_FLASH_OFF_MS);
        }
    }
}

#if KBD_SLEEP_TEST_MODE
/**
 * 测试固件的无串口休眠进度提示。
 * 只读空闲计时，不调用 RecordActivity，因此不会改变真实休眠计时。
 */
static void KBD_Mode_RunSleepTestIndicator(void)
{
    uint32_t idle_ms = KBD_Mode_GetIdleMs();
    uint32_t light_timeout_ms = KBD_Mode_GetLightSleepTimeoutMs();
    uint32_t bucket = idle_ms / KBD_SLEEP_TEST_FLASH_INTERVAL_MS;

    if (bucket == 0u)
    {
        g_sleep_test_last_bucket = 0u;
        return;
    }

    if (bucket != g_sleep_test_last_bucket)
    {
        g_sleep_test_last_bucket = bucket;

        if (light_timeout_ms == 0u)
        {
            /* LIGHT=0 explicitly disables automatic sleep. */
            KBD_RGB_Flash(180, 0, 255,
                          KBD_SLEEP_TEST_FLASH_DURATION_MS);
            return;
        }

        /* Once LIGHT is due, report why entry is blocked instead of
         * continuing to show the normal idle-progress colour. */
        if (idle_ms >= light_timeout_ms)
        {
            if (KBD_Mode_USB_HasProtocolHandshake())
            {
                KBD_RGB_Flash(255, 0, 0, KBD_SLEEP_TEST_FLASH_DURATION_MS);
                return;
            }

            if (KBD_Battery_GetChargeState() == BAT_CHG_CHARGING)
            {
                KBD_RGB_Flash(255, 160, 0,
                              KBD_SLEEP_TEST_FLASH_DURATION_MS);
                return;
            }

            /* No blocker: the caller will immediately enter LIGHT and play
             * the three-flash entry animation. */
            return;
        }

        KBD_RGB_Flash(KBD_IND_SLEEP_READY_R,
                      KBD_IND_SLEEP_READY_G,
                      KBD_IND_SLEEP_READY_B,
                      KBD_SLEEP_TEST_FLASH_DURATION_MS);
    }
}
#endif

static kbd_state_t KBD_Mode_ResolveIndicatorState(void)
{
    if (g_current_mode == KBD_WORK_MODE_USB && g_USB_DeviceState >= USB_STATE_POWERED)
    {
        return KBD_STATE_USB_CONNECTED;
    }

    if (KBD_Battery_GetChargeState() == BAT_CHG_CHARGING)
    {
        return KBD_STATE_CHARGING;
    }

    if (KBD_Battery_GetLevel() <= KBD_LOW_BATTERY_INDICATOR_LEVEL)
    {
        return KBD_STATE_LOW_BATTERY;
    }

    if (g_current_mode == KBD_WORK_MODE_USB)
    {
        return KBD_STATE_USB_CONNECTED;
    }

    switch (g_conn_state)
    {
    case KBD_CONN_CONNECTED:
        return KBD_STATE_BLE_CONNECTED;
    case KBD_CONN_ADVERTISING:
        return KBD_STATE_BLE_ADVERTISING;
    case KBD_CONN_SUSPENDED:
    case KBD_CONN_DISCONNECTED:
    default:
        return KBD_STATE_BLE_DISCONNECTED;
    }
}

static void KBD_Mode_RefreshIndicatorState(void)
{
    KBD_RGB_SetState(KBD_Mode_ResolveIndicatorState());
}
