/********************************** (C) COPYRIGHT *******************************
 * File Name          : Main.c
 * Author             : MeowKJ
 * Version            : V2.0
 * Date               : 2024/11/07
 * Description        : CH592F 键盘主程序入口
 *******************************************************************************/

/* BLE 相关头文件（包含 CH59x_common.h） */
#include "ble_config.h"
#include "ble_hal.h"

/* 键盘核心模块 */
#include "kbd_mode.h"
#include "kbd_core.h"
#include "kbd_macro.h"
#include "kbd_storage.h"
#include "kbd_command.h"
#include "kbd_rgb.h"
#include "kbd_log.h"

/* 硬件抽象层 */
#include "key.h"
#include "kbd_battery.h"
#include "hal_utils.h"
#include "ws2812.h"
#include "encoder.h"
#include "debug.h"

/* ============== TMOS 内存池 ============== */
__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

#if (defined(BLE_MAC)) && (BLE_MAC == TRUE)
const uint8_t MacAddr[6] = {0x84, 0xC2, 0xE4, 0x03, 0x02, 0x02};
#endif

/* ============== 主循环 ============== */
__HIGH_CODE
__attribute__((noinline)) void Main_Circulation(void)
{
    while (1) {
        TMOS_SystemProcess();
        KBD_Mode_Process();
        KBD_Core_Process();
        KBD_Log_Flush();
    }
}

/* ============== 主函数 ============== */
int main(void)
{
    /* 设置系统时钟 */
    SetSysClock(CLK_SOURCE_PLL_60MHz);

    /* Shutdown uses a software reset after GPIO wake. Capture the retained
     * marker and latched GPIO source before peripheral initialization clears it. */
    uint8_t deep_wake_marker = R8_GLOB_RESET_KEEP;
    uint32_t deep_wake_gpioa_flags = GPIOA_ReadITFlagPort();
    uint32_t deep_wake_gpiob_flags = GPIOB_ReadITFlagPort();
    SYS_ResetKeepBuf(0u);

#if (defined(DCDC_ENABLE)) && (DCDC_ENABLE == TRUE)
    /* 启用内部 DCDC，节省核心功耗（约 30%） */
    PWR_DCDCCfg(ENABLE);
#endif

#ifdef DEBUG
    /* 调试串口初始化 */
    Debug_Init();
    /* 最早期串口探活：用于确认 UART1(PB13 TX) 链路正常 */
    Log_Output("I", "BOOT", "UART alive");
#endif

    /* BLE 库初始化（提供 TMOS 调度器，USB/BLE 模式都需要） */
    CH59x_BLEInit();

    /* HAL 初始化 */
    HAL_Init();

    /* 按键驱动初始化 */
    Key_Init();

    /* 旋钮驱动初始化 */
    Encoder_Init();

    /* 存储系统初始化（需在模式判定前完成，以读取 last_mode） */
    KBD_Storage_Init();

    /* 将每个 FN 的持久化长按阈值同步到按键驱动。 */
    {
        kbd_fnkey_config_t *fn_cfg = KBD_GetFnKeyConfig();
        for (uint8_t i = 0; i < KBD_FN_NUM_KEYS; i++) {
            FnKey_SetLongPressThreshold(i, fn_cfg->fn[i].long_press_ms);
        }
    }

    /* DEEP wake is a cold BLE reconnect. Consume all wake/reconnect input and
     * synchronize an empty HID state before accepting fresh key presses. */
    if (deep_wake_marker == KBD_DEEP_WAKE_RESET_MARKER)
    {
        Key_BeginDeepWakeRecovery(deep_wake_gpioa_flags,
                                  deep_wake_gpiob_flags);
    }

    /* 读取上次模式，决定本次启动路径 */
    uint8_t last_mode = KBD_GetLastMode();
    kbd_work_mode_t initial_mode = (last_mode == 1) ? KBD_WORK_MODE_BLE : KBD_WORK_MODE_USB;

    /*
     * 按 WCH Application 示例思路：每种模式只初始化对应协议栈
     * - USB 模式：跳过 GAP/HID 初始化，仅 USB
     * - BLE 模式：完整 BLE HID 初始化，跳过 USB
     */
    if (initial_mode == KBD_WORK_MODE_BLE) {
        GAPRole_PeripheralInit();
    }

    /* 命令处理初始化 */
    KBD_Command_Init();

    /* RGB 灯效初始化 */
    KBD_RGB_Init();

    /* 电池检测初始化 */
    KBD_Battery_Init();

    /* HID 日志系统初始化 */
    KBD_Log_Init();

    /* 键盘核心模块初始化 */
    KBD_Core_Init();

    /* 宏引擎初始化 */
    KBD_Macro_Init();

    /* 模式管理器初始化（根据模式执行对应协议栈初始化） */
    KBD_Mode_Init(initial_mode, KBD_Core_GetCallbacks());

    /* 记录启动事件 */
    KBD_Log_SystemEvent(KBD_LOG_SYS_BOOT);

    /* 进入主循环 */
    Main_Circulation();

    return 0;
}
