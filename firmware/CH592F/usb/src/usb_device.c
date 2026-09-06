/********************************** USB Device Layer Implementation ***********
 * File Name          : usb_device.c
 * Author             : Custom USB Library
 * Version            : V1.0
 * Date               : 2024/11/07
 * Description        : USB 设备层实现
 *******************************************************************************/

#include "usb_device.h"
#include "debug.h"
#include "kbd_mode.h"
#include <string.h>

#define TAG "USB"

/* ==================== Global Variables ==================== */
USB_DeviceState_t g_USB_DeviceState = USB_STATE_DETACHED;
uint8_t g_USB_DeviceConfig = 0;
uint8_t g_USB_DeviceAddress = 0;
uint8_t g_USB_SleepStatus = 0;

uint8_t g_SetupReqCode = 0;
uint16_t g_SetupReqLen = 0;
const uint8_t *g_pDescriptor = NULL;
static uint8_t g_SetupReqInterface = 0; // 保存当前请求的接口号

uint8_t g_IdleValue[4] = {0};
uint8_t g_ProtocolValue[4] = {0};

/* 端点缓冲区 */
__attribute__((aligned(4))) uint8_t EP0_Databuf[64 + 64 + 64]; // EP0 + EP4_OUT + EP4_IN
__attribute__((aligned(4))) uint8_t EP1_Databuf[64 + 64];      // EP1_OUT + EP1_IN
__attribute__((aligned(4))) uint8_t EP2_Databuf[64 + 64];      // EP2_OUT + EP2_IN
__attribute__((aligned(4))) uint8_t EP3_Databuf[64 + 64];      // EP3_OUT + EP3_IN

/* ==================== Private Functions ==================== */

/**
 * @brief 处理 Setup 包
 */
static void USB_HandleSetupPacket(void)
{
    uint8_t len = 0;
    uint8_t errflag = 0;
    uint8_t chtype = pSetupReqPak->bRequestType;

    g_SetupReqLen = pSetupReqPak->wLength;
    g_SetupReqCode = pSetupReqPak->bRequest;

    if ((chtype & USB_REQ_TYP_MASK) != USB_REQ_TYP_STANDARD)
    {
        /* 非标准请求 */
        if (chtype & 0x20)
        { // 类请求
            uint8_t interface = pSetupReqPak->wIndex & 0xFF;

            switch (g_SetupReqCode)
            {
            case DEF_USB_SET_IDLE: /* 0x0A */
                if (interface < 4)
                {
                    g_IdleValue[interface] = pSetupReqPak->wValue >> 8;
                }
                break;

            case DEF_USB_GET_IDLE: /* 0x02 */
                if (interface < 4)
                {
                    pEP0_DataBuf[0] = g_IdleValue[interface];
                    len = 1;
                }
                break;

            case DEF_USB_SET_PROTOCOL: /* 0x0B */
                if (interface < 4)
                {
                    g_ProtocolValue[interface] = pSetupReqPak->wValue & 0xFF;
                }
                break;

            case DEF_USB_GET_PROTOCOL: /* 0x03 */
                if (interface < 4)
                {
                    pEP0_DataBuf[0] = g_ProtocolValue[interface];
                    len = 1;
                }
                break;

            case DEF_USB_SET_REPORT: /* 0x09 */
                // 保存接口号，在 OUT 阶段处理数据
                g_SetupReqInterface = pSetupReqPak->wIndex & 0xFF;
                break;

            case DEF_USB_GET_REPORT: /* 0x01 */
                // 可根据接口返回当前报告
                errflag = 0xFF;
                break;

            default:
                errflag = 0xFF;
                break;
            }
        }
        else if (chtype & 0x40)
        { // 厂商请求
            errflag = 0xFF;
        }
    }
    else
    {
        /* 标准请求 */
        switch (g_SetupReqCode)
        {
        case USB_GET_DESCRIPTOR:
            switch ((pSetupReqPak->wValue) >> 8)
            {
            case USB_DESCR_TYP_DEVICE:
                g_pDescriptor = USB_DeviceDescriptor;
                len = USB_DeviceDescriptor[0];
                break;

            case USB_DESCR_TYP_CONFIG:
                g_pDescriptor = USB_ConfigDescriptor;
                len = USB_ConfigDescriptor[2] | (USB_ConfigDescriptor[3] << 8);
                break;

            case USB_DESCR_TYP_STRING:
                switch (pSetupReqPak->wValue & 0xFF)
                {
                case 0:
                    g_pDescriptor = USB_StringLangID;
                    len = USB_StringLangID[0];
                    break;
                case 1:
                    g_pDescriptor = USB_StringVendor;
                    len = USB_StringVendor[0];
                    break;
                case 2:
                    g_pDescriptor = USB_StringProduct;
                    len = USB_StringProduct[0];
                    break;
                default:
                    errflag = 0xFF;
                    break;
                }
                break;

            case USB_DESCR_TYP_REPORT:
            {
                uint8_t interface = pSetupReqPak->wIndex & 0xFF;
                switch (interface)
                {
                case INTF_KEYBOARD:
                    g_pDescriptor = HID_KeyboardReportDescriptor;
                    len = HID_KeyboardReportDescSize;
                    break;
                case INTF_MOUSE:
                    g_pDescriptor = HID_MouseReportDescriptor;
                    len = HID_MouseReportDescSize;
                    break;
                case INTF_CONSUMER:
                    g_pDescriptor = HID_ConsumerReportDescriptor;
                    len = HID_ConsumerReportDescSize;
                    break;
                case INTF_CONFIG:
                    g_pDescriptor = HID_ConfigReportDescriptor;
                    len = HID_ConfigReportDescSize;
                    break;
                default:
                    errflag = 0xFF;
                    break;
                }
            }
            break;

            case 0x06: // Qualifier
                g_pDescriptor = USB_QualifierDescriptor;
                len = USB_QUALIFIER_DESC_SIZE;
                break;

            default:
                errflag = 0xFF;
                break;
            }

            if (g_SetupReqLen > len)
                g_SetupReqLen = len;
            len = (g_SetupReqLen >= DevEP0SIZE) ? DevEP0SIZE : g_SetupReqLen;
            memcpy(pEP0_DataBuf, g_pDescriptor, len);
            g_pDescriptor += len;
            break;

        case USB_SET_ADDRESS:
            g_SetupReqLen = pSetupReqPak->wValue & 0xFF;
            break;

        case USB_GET_CONFIGURATION:
            pEP0_DataBuf[0] = g_USB_DeviceConfig;
            if (g_SetupReqLen > 1)
                g_SetupReqLen = 1;
            break;

        case USB_SET_CONFIGURATION:
            g_USB_DeviceConfig = pSetupReqPak->wValue & 0xFF;
            g_USB_DeviceState = USB_STATE_CONFIGURED;
            break;

        case USB_GET_INTERFACE:
            pEP0_DataBuf[0] = 0x00;
            if (g_SetupReqLen > 1)
                g_SetupReqLen = 1;
            break;

        case USB_SET_INTERFACE:
            break;

        case USB_CLEAR_FEATURE:
            if ((chtype & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP)
            {
                switch (pSetupReqPak->wIndex & 0xFF)
                {
                case 0x81:
                    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK;
                    break;
                case 0x82:
                    R8_UEP2_CTRL = (R8_UEP2_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK;
                    break;
                case 0x83:
                    R8_UEP3_CTRL = (R8_UEP3_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK;
                    break;
                case 0x84:
                    R8_UEP4_CTRL = (R8_UEP4_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK;
                    break;
                case 0x04:
                    R8_UEP4_CTRL = (R8_UEP4_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) | UEP_R_RES_ACK;
                    break;
                default:
                    errflag = 0xFF;
                    break;
                }
            }
            else if ((chtype & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE)
            {
                if (pSetupReqPak->wValue == 1)
                {
                    g_USB_SleepStatus &= ~0x01;
                }
            }
            break;

        case USB_SET_FEATURE:
            if ((chtype & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP)
            {
                switch (pSetupReqPak->wIndex & 0xFF)
                {
                case 0x81:
                    R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_STALL;
                    break;
                case 0x82:
                    R8_UEP2_CTRL = (R8_UEP2_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_STALL;
                    break;
                case 0x83:
                    R8_UEP3_CTRL = (R8_UEP3_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_STALL;
                    break;
                case 0x84:
                    R8_UEP4_CTRL = (R8_UEP4_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_STALL;
                    break;
                case 0x04:
                    R8_UEP4_CTRL = (R8_UEP4_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) | UEP_R_RES_STALL;
                    break;
                default:
                    errflag = 0xFF;
                    break;
                }
            }
            else if ((chtype & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE)
            {
                if (pSetupReqPak->wValue == 1)
                {
                    g_USB_SleepStatus |= 0x01;
                }
            }
            break;

        case USB_GET_STATUS:
            pEP0_DataBuf[0] = 0x00;
            pEP0_DataBuf[1] = 0x00;
            if (g_SetupReqLen >= 2)
                g_SetupReqLen = 2;
            break;

        default:
            errflag = 0xFF;
            break;
        }
    }

    if (errflag == 0xFF)
    {
        R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
    }
    else
    {
        if (chtype & 0x80)
        {
            len = (g_SetupReqLen > DevEP0SIZE) ? DevEP0SIZE : g_SetupReqLen;
            g_SetupReqLen -= len;
        }
        else
        {
            len = 0;
        }
        R8_UEP0_T_LEN = len;
        R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
    }
}

/* ==================== Public Functions ==================== */

/**
 * @brief USB 设备初始化
 */
void USB_Device_Init(void)
{
    USB_Descriptors_Init();

    // 设置端点缓冲区地址
    pEP0_RAM_Addr = EP0_Databuf;
    pEP1_RAM_Addr = EP1_Databuf;
    pEP2_RAM_Addr = EP2_Databuf;
    pEP3_RAM_Addr = EP3_Databuf;

    // 调用底层 USB 初始化
    USB_DeviceInit();

    // 初始化各 HID 功能
    USB_Keyboard_Init();
    USB_Mouse_Init();
    USB_Consumer_Init();
    USB_Config_Init();

    g_USB_DeviceState = USB_STATE_ATTACHED;
    PFIC_EnableIRQ(USB_IRQn);
}

void USB_Device_Deinit(void)
{
    PFIC_DisableIRQ(USB_IRQn);
    R16_PIN_ANALOG_IE &= ~(RB_PIN_USB_IE | RB_PIN_USB_DP_PU); /* 移除 D+ 上拉与 USB 模拟使能 */
    R8_UDEV_CTRL = 0;                                         /* 关闭 USB 端口 */
    R8_USB_CTRL = 0;                                          /* 关闭 USB 模块 */
    R8_USB_INT_EN = 0;                                        /* 清除中断使能 */
    GPIOB_ModeCfg(GPIO_Pin_10 | GPIO_Pin_11, GPIO_ModeIN_PD); /* USB D-/D+ 下拉，避免浮空干扰 */
    g_USB_DeviceState = USB_STATE_DETACHED;
}

static bool USB_Device_ShouldWakeKeyboard(void)
{
    if (!KBD_Mode_IsInSleep())
    {
        return false;
    }

    /* BLE mode keeps USB initialized for Studio configuration. Without a
     * cable, phantom RESET/TRANSFER flags must not wake LIGHT. Only a host
     * that completed enumeration is allowed to wake the keyboard. */
    if (KBD_Mode_Get() == KBD_WORK_MODE_BLE)
    {
        return (g_USB_DeviceState == USB_STATE_CONFIGURED);
    }

    return true;
}

/**
 * @brief USB 传输处理
 */
void USB_Device_TransferProcess(void)
{
    uint8_t len;
    uint8_t intflag = R8_USB_INT_FG;

    if (intflag & RB_UIF_TRANSFER)
    {
        if (USB_Device_ShouldWakeKeyboard())
        {
            KBD_Mode_RequestWake();
        }

        if ((R8_USB_INT_ST & MASK_UIS_TOKEN) != MASK_UIS_TOKEN)
        {
            switch (R8_USB_INT_ST & (MASK_UIS_TOKEN | MASK_UIS_ENDP))
            {
            case UIS_TOKEN_IN: // EP0 IN
                switch (g_SetupReqCode)
                {
                case USB_GET_DESCRIPTOR:
                    len = g_SetupReqLen >= DevEP0SIZE ? DevEP0SIZE : g_SetupReqLen;
                    memcpy(pEP0_DataBuf, g_pDescriptor, len);
                    g_SetupReqLen -= len;
                    g_pDescriptor += len;
                    R8_UEP0_T_LEN = len;
                    R8_UEP0_CTRL ^= RB_UEP_T_TOG;
                    break;
                case USB_SET_ADDRESS:
                    R8_USB_DEV_AD = (R8_USB_DEV_AD & RB_UDA_GP_BIT) | g_SetupReqLen;
                    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                    g_USB_DeviceState = USB_STATE_ADDRESS;
                    break;
                default:
                    R8_UEP0_T_LEN = 0;
                    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                    break;
                }
                break;

            case UIS_TOKEN_OUT: // EP0 OUT
                len = R8_USB_RX_LEN;
                if (g_SetupReqCode == DEF_USB_SET_REPORT)
                {
                    if (g_SetupReqInterface == INTF_CONFIG && len > 0)
                    {
                        // 配置接口 SET_REPORT - 调用命令处理
                        USB_ConfigReport_t *report = (USB_ConfigReport_t *)pEP0_DataBuf;
                        USB_Config_ProcessCommand(report);
                    }
                }
                R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                break;

            case UIS_TOKEN_IN | 1: // EP1 IN (Keyboard)
                R8_UEP1_CTRL ^= RB_UEP_T_TOG;
                R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                USB_DevEP1_IN_Callback();
                break;

            case UIS_TOKEN_IN | 2: // EP2 IN (Mouse)
                R8_UEP2_CTRL ^= RB_UEP_T_TOG;
                R8_UEP2_CTRL = (R8_UEP2_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                USB_DevEP2_IN_Callback();
                break;

            case UIS_TOKEN_IN | 3: // EP3 IN (Consumer)
                R8_UEP3_CTRL ^= RB_UEP_T_TOG;
                R8_UEP3_CTRL = (R8_UEP3_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                USB_DevEP3_IN_Callback();
                break;

            case UIS_TOKEN_IN | 4: // EP4 IN (Config)
                R8_UEP4_CTRL ^= RB_UEP_T_TOG;
                R8_UEP4_CTRL = (R8_UEP4_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                USB_DevEP4_IN_Callback();
                break;

            case UIS_TOKEN_OUT | 4: // EP4 OUT (Config)
                if (R8_USB_INT_ST & RB_UIS_TOG_OK)
                {
                    R8_UEP4_CTRL ^= RB_UEP_R_TOG;
                    len = R8_USB_RX_LEN;
                    DevEP4_OUT_Deal(len);
                }
                break;

            default:
                break;
            }
            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }

        if (R8_USB_INT_ST & RB_UIS_SETUP_ACT)
        {
            if (USB_Device_ShouldWakeKeyboard())
            {
                KBD_Mode_RequestWake();
            }
            USB_HandleSetupPacket();
            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }
    }
    else if (intflag & RB_UIF_BUS_RST)
    {
        R8_USB_DEV_AD = 0;
        R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP1_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP2_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP3_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP4_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_USB_INT_FG = RB_UIF_BUS_RST;
        g_USB_DeviceState = USB_STATE_DEFAULT;
        if (USB_Device_ShouldWakeKeyboard())
        {
            KBD_Mode_RequestWake();
        }
    }
    else if (intflag & RB_UIF_SUSPEND)
    {
        R8_USB_INT_FG = RB_UIF_SUSPEND;
        g_USB_DeviceState = USB_STATE_SUSPENDED;
    }
    else
    {
        R8_USB_INT_FG = intflag;
    }
}

/**
 * @brief USB 设备唤醒主机
 */
void USB_Device_Wakeup(void)
{
    R16_PIN_ANALOG_IE &= ~(RB_PIN_USB_DP_PU);
    R8_UDEV_CTRL |= RB_UD_LOW_SPEED;
    mDelaymS(2);
    R8_UDEV_CTRL &= ~RB_UD_LOW_SPEED;
    R16_PIN_ANALOG_IE |= RB_PIN_USB_DP_PU;
}

/**
 * @brief USB 中断处理函数
 */
__INTERRUPT
__HIGH_CODE
void USB_IRQHandler(void)
{
    USB_Device_TransferProcess();
}
