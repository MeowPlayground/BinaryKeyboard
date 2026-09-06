/**
 * @file    kbd_storage.c
 * @brief   MeowKeyboard 配置存储实现
 * @author  MeowKJ
 * @version V1.0.0
 * @date    2024-11-07
 *
 * @details
 * 基于 CH592F DataFlash 实现配置持久化存储。
 *
 * DataFlash 特性：
 * - 总容量：32KB (0x0000 ~ 0x7FFF)
 * - 擦除粒度：256B / 4KB（CH592F 配置区使用 256B；CH592A 存在 4KB 限制）
 * - 写入推荐：256 字节对齐
 *
 * 存储布局：
 * - 0x0000 ~ 0x0BFF: 配置槽轮转区 (3 槽 × 1KB，冷/温数据)
 * - 0x0C00 ~ 0x0FFF: runtime 热数据区 (4 页 × 256B，仅高频字段)
 * - 0x1000 ~ 0x4FFF: 宏数据区 (16KB, 8 槽 × 2KB)
 * - 0x7E00 ~ 0x7EFF: BLE SNV (蓝牙配对，WCH 库管理)
 *
 * @copyright Copyright (c) 2024 MeowKJ. All rights reserved.
 */

#include "kbd_storage.h"
#include "kbd_command.h"
#include "CH59x_common.h"
#include "ble_config.h"
#include "debug.h"
#include "kbd_config.h"
#include <string.h>

/** @brief 模块日志标签 */
#define TAG "STOR"

/* 配置存储（CH592F）采用“冷热分离”：
 * - 冷/温配置：3 个 1KB 槽位轮转（header/system/keymap/fn/rgb）
 * - 热数据（current_layer / last_mode）：1KB 区域内 4 个 256B 页轮转
 */
#define KBD_CFG_SLOT_SIZE KBD_FLASH_RESERVED          /* 0x400 (1KB) */
#define KBD_CFG_PAGE_SIZE EEPROM_PAGE_SIZE            /* 256B */
#define KBD_CFG_PAGE_COUNT (KBD_CFG_SLOT_SIZE / KBD_CFG_PAGE_SIZE) /* 4 */
#define KBD_CFG_INVALID_SLOT 0xFF

#define KBD_RUNTIME_REGION_ADDR (KBD_FLASH_MACRO_BASE - KBD_CFG_SLOT_SIZE) /* 0x0C00 */
#define KBD_CFG_REGION_SIZE (KBD_RUNTIME_REGION_ADDR - KBD_FLASH_BASE)      /* 0x0C00 */
#define KBD_CFG_SLOT_COUNT (KBD_CFG_REGION_SIZE / KBD_CFG_SLOT_SIZE) /* 3 */
#define KBD_CFG_SLOT_ADDR(slot) ((uint32_t)KBD_FLASH_BASE + ((uint32_t)(slot) * KBD_CFG_SLOT_SIZE))

#define KBD_RUNTIME_PAGE_COUNT KBD_CFG_PAGE_COUNT
#define KBD_RUNTIME_INVALID_PAGE 0xFF
#define KBD_RUNTIME_PAGE_ADDR(page) (KBD_RUNTIME_REGION_ADDR + ((uint32_t)(page) * KBD_CFG_PAGE_SIZE))
#define KBD_RUNTIME_MAGIC 0x52554E54u /* 'RUNT' */
#define KBD_RUNTIME_VERSION 0x0001u

/* TMOS 延迟保存（高频 runtime 状态） */
#define KBD_STORAGE_RUNTIME_SAVE_EVT 0x0001u
#ifndef KBD_STORAGE_RUNTIME_SAVE_DELAY_MS
#define KBD_STORAGE_RUNTIME_SAVE_DELAY_MS 200u
#endif
#ifndef KBD_STORAGE_RUNTIME_SAVE_RETRY_MS
#define KBD_STORAGE_RUNTIME_SAVE_RETRY_MS 100u
#endif

/* TMOS 延迟宏写入（从 USB ISR 延迟到主循环，避免 ISR 中操作 Flash） */
#define KBD_STORAGE_MACRO_WRITE_EVT 0x0002u

/* 宏操作类型 */
#define MACRO_OP_IDLE       0
#define MACRO_OP_WRITE      1
#define MACRO_OP_ERASE_PAGE 2
#define MACRO_OP_ERASE_ALL  3

/* 最大单包写入长度（与 Studio 的 CH592_MEOWFS_WRITE_CHUNK 一致） */
#define MACRO_WRITE_BUF_SIZE 58

/* 前向声明（在 TMOS 事件处理中使用） */
static int MeowFs_WriteRawInternal(uint16_t offset, const uint8_t *buf,
                                   uint16_t len);

/*============================================================================*/
/*                              私有变量 */
/*============================================================================*/

/** @brief 配置头部 (RAM 缓存) */
static kbd_config_header_t s_config_header;

/** @brief 系统配置 (RAM 缓存) */
static kbd_system_config_t s_system_config;

/** @brief 按键映射配置 (RAM 缓存) */
static kbd_keymap_t s_keymap_config;

/** @brief FN 键配置 (RAM 缓存) */
static kbd_fnkey_config_t s_fnkey_config;

/** @brief RGB 配置 (RAM 缓存) */
static kbd_rgb_config_t s_rgb_config;

/** @brief 当前已加载配置所在槽位（0~3，0xFF=未加载） */
static uint8_t s_config_active_slot = KBD_CFG_INVALID_SLOT;

/** @brief runtime 热数据页环当前页（0~3，0xFF=未加载） */
static uint8_t s_runtime_active_page = KBD_RUNTIME_INVALID_PAGE;

/** @brief runtime 热数据序号（单调递增） */
static uint32_t s_runtime_seq = 0;

/** @brief TMOS 存储任务 ID（用于 runtime 延迟保存） */
static tmosTaskID s_storage_task_id = TASK_NO_TASK;

/** @brief runtime 待保存标志 */
static uint8_t s_runtime_dirty = 0;

/** @brief runtime 待保存层号（防抖合并） */
static uint8_t s_runtime_pending_layer = 0;

/** @brief runtime 最近一次成功持久化的层号（0xFF=未知） */
static uint8_t s_runtime_last_saved_layer = 0xFF;

/** @brief runtime 待保存工作模式（0=USB, 1=BLE, 0xFF=未知） */
static uint8_t s_runtime_pending_mode = 0xFF;

/** @brief runtime 最近一次成功持久化的工作模式（0xFF=未知） */
static uint8_t s_runtime_last_saved_mode = 0xFF;

/** @brief 待延迟执行的宏操作（ISR → TMOS 主循环） */
static struct {
  uint8_t  type;       /**< MACRO_OP_* 操作类型 */
  uint8_t  sub;        /**< 子命令编号（用于回复） */
  uint16_t offset;     /**< 写入偏移 */
  uint16_t len;        /**< 写入长度 */
  uint8_t  erase_page; /**< 擦除页索引 */
  uint8_t  data[MACRO_WRITE_BUF_SIZE]; /**< 写入数据缓冲 */
} s_macro_pending;

/*============================================================================*/
/*                              默认配置 */
/*============================================================================*/

/**
 * @brief 默认按键映射 (根据键盘类型自动选择)
 */
#if defined(KBD_LAYOUT_5KEY)
/*---------------------------------------------------------------------------*/
/* 五键款: 5 键, 5 层 → 数字键 1-5 */
/*---------------------------------------------------------------------------*/
static const kbd_keymap_t s_default_keymap = {
    .num_layers = KBD_DEFAULT_LAYERS,
    .current_layer = 0,
    .default_layer = 0,
    .reserved = 0,
    .layers = {
        {/* 层 1 */
         .keys =
             {
                 {KBD_ACTION_KEYBOARD, 0, 0x1E, 0}, /* '1' */
                 {KBD_ACTION_KEYBOARD, 0, 0x1F, 0}, /* '2' */
                 {KBD_ACTION_KEYBOARD, 0, 0x20, 0}, /* '3' */
                 {KBD_ACTION_KEYBOARD, 0, 0x21, 0}, /* '4' */
                 {KBD_ACTION_KEYBOARD, 0, 0x22, 0}, /* '5' */
                 {KBD_ACTION_NONE, 0, 0, 0},
                 {KBD_ACTION_NONE, 0, 0, 0},
                 {KBD_ACTION_NONE, 0, 0, 0},
             }},
        {.keys = {{KBD_ACTION_NONE, 0, 0, 0}}}, /* 层 2 */
        {.keys = {{KBD_ACTION_NONE, 0, 0, 0}}}, /* 层 3 */
        {.keys = {{KBD_ACTION_NONE, 0, 0, 0}}}, /* 层 4 */
        {.keys = {{KBD_ACTION_NONE, 0, 0, 0}}}, /* 层 5 */
    }};

#elif defined(KBD_LAYOUT_KNOB)
/*---------------------------------------------------------------------------*/
/* 旋钮款: 4 普通键 + 3 旋钮动作, 4 层 */
/* [0-3] 普通键 → 数字键 1-4 */
/* [4]   旋钮顺时针 → 音量增加 */
/* [5]   旋钮逆时针 → 音量减少 */
/* [6]   旋钮按下 → 静音 */
/*---------------------------------------------------------------------------*/
static const kbd_keymap_t s_default_keymap = {
    .num_layers = KBD_DEFAULT_LAYERS,
    .current_layer = 0,
    .default_layer = 0,
    .reserved = 0,
    .layers = {
        {/* 层 1 */
         .keys =
             {
                 {KBD_ACTION_KEYBOARD, 0, 0x1E, 0},    /* K1: '1' */
                 {KBD_ACTION_KEYBOARD, 0, 0x1F, 0},    /* K2: '2' */
                 {KBD_ACTION_KEYBOARD, 0, 0x20, 0},    /* K3: '3' */
                 {KBD_ACTION_KEYBOARD, 0, 0x21, 0},    /* K4: '4' */
                 {KBD_ACTION_CONSUMER, 0, 0xE9, 0x00}, /* CW: Vol+ */
                 {KBD_ACTION_CONSUMER, 0, 0xEA, 0x00}, /* CCW: Vol- */
                 {KBD_ACTION_CONSUMER, 0, 0xE2, 0x00}, /* Click: Mute */
                 {KBD_ACTION_NONE, 0, 0, 0},
             }},
        {.keys = {{KBD_ACTION_NONE, 0, 0, 0}}}, /* 层 2 */
        {.keys = {{KBD_ACTION_NONE, 0, 0, 0}}}, /* 层 3 */
        {.keys = {{KBD_ACTION_NONE, 0, 0, 0}}}, /* 层 4 */
        {.keys = {{KBD_ACTION_NONE, 0, 0, 0}}}, /* 预留 */
    }};

#else
#error "请通过 CMake 或 MRS 预处理宏选择一个键盘型号"
#endif

/**
 * @brief 默认 FN 键配置
 *
 * - FN1: 短按=切换模式, 长按=切换模式
 * - FN2: 短按=下一层, 长按=清除配对
 */
static const kbd_fnkey_config_t s_default_fnkey = {
    .fn = {
        /* FN1 */
        {
            .click_action = KBD_FN_MODE_TOGGLE,
            .click_param = 0,
            .long_action = KBD_FN_MODE_TOGGLE,
            .long_param = 0,
            .long_press_ms = 800,
        },
        /* FN2 */
        {
            .click_action = KBD_FN_LAYER_NEXT,
            .click_param = 0,
            .long_action = KBD_FN_BLE_CLEAR_BONDS,
            .long_param = 0,
            .long_press_ms = 2000,
        },
        /* FN3-4: 未使用 */
        {.click_action = KBD_FN_NONE},
        {.click_action = KBD_FN_NONE},
    }};

/**
 * @brief 默认 RGB 配置 (默认 20% 亮度)
 */
static const kbd_rgb_config_t s_default_rgb = {
    .enabled = 1,
    .mode = KBD_RGB_INDICATOR,
    .brightness = KBD_RGB_DEFAULT_BRIGHTNESS,
    .speed = 128,
    .color_r = 255,
    .color_g = 255,
    .color_b = 255,
    .indicator_enabled = 1,
    .indicator_brightness = KBD_RGB_DEFAULT_BRIGHTNESS,
};

/**
 * @brief 默认系统配置
 */
static const kbd_system_config_t s_default_system = {
    .default_mode = 0,          /* USB */
    .auto_sleep_min = 1,        /* LIGHT 默认 1 分钟 */
    .debounce_ms = 10,          /* 10ms */
    .log_enabled = KBD_LOG_DEFAULT_ENABLED, /* HID 日志默认开关由构建类型决定 */
    .deep_sleep_min = 1,        /* DEEP 默认在 LIGHT 后 1 分钟 */
    .os_mode = KBD_OS_MODE_WIN, /* 默认 Win 模式 */
    .seamless_wake = KBD_SEAMLESS_WAKE_ENABLED,
};

/*============================================================================*/
/*                              CRC32 查找表 */
/*============================================================================*/

/** @brief CRC32 查找表 (IEEE 802.3 多项式) */
static const uint32_t s_crc32_table[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F,
    0xE963A535, 0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
    0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
    0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9,
    0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
    0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
    0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
    0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
    0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D, 0x76DC4190, 0x01DB7106,
    0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
    0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
    0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
    0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7,
    0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
    0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7A8B, 0x5005713C, 0x270241AA,
    0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
    0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
    0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683, 0xE3630B12, 0x94643B84,
    0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB,
    0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
    0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
    0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55,
    0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
    0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28,
    0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
    0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
    0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242,
    0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69,
    0x616BFFD3, 0x166CCF45, 0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
    0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
    0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD706B3,
    0x54DE5729, 0x23D967BF, 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
    0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D};

/*============================================================================*/
/*                              私有函数 */
/*============================================================================*/

/**
 * @brief 加载默认配置到 RAM
 */
static void LoadDefaults(void) {
  memset(&s_config_header, 0, sizeof(s_config_header));
  s_config_header.magic = KBD_CONFIG_MAGIC;
  s_config_header.version = KBD_CONFIG_VERSION;
  s_config_header.save_count = 0;
  s_config_active_slot = KBD_CFG_INVALID_SLOT;
  s_runtime_active_page = KBD_RUNTIME_INVALID_PAGE;
  s_runtime_seq = 0;
  s_runtime_dirty = 0;
  s_runtime_pending_layer = 0;
  s_runtime_last_saved_layer = 0xFF;
  s_runtime_pending_mode = 0xFF;
  s_runtime_last_saved_mode = 0xFF;

  memcpy(&s_system_config, &s_default_system, sizeof(kbd_system_config_t));
  memcpy(&s_keymap_config, &s_default_keymap, sizeof(kbd_keymap_t));
  memcpy(&s_fnkey_config, &s_default_fnkey, sizeof(kbd_fnkey_config_t));
  memcpy(&s_rgb_config, &s_default_rgb, sizeof(kbd_rgb_config_t));
}

typedef struct {
  uint8_t slot;
  kbd_config_header_t header;
  kbd_system_config_t system;
  kbd_keymap_t keymap;
  kbd_fnkey_config_t fnkey;
  kbd_rgb_config_t rgb;
} kbd_config_slot_cache_t;

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t version;
  uint16_t flags;
  uint32_t seq;
  uint8_t current_layer;
  uint8_t last_mode;     /**< 工作模式 (0=USB, 1=BLE, 0xFF=未知) */
  uint8_t reserved[238];
  uint32_t crc32;
} kbd_runtime_page_t;

static uint32_t CalcConfigCRC(const kbd_system_config_t *system,
                              const kbd_keymap_t *keymap,
                              const kbd_fnkey_config_t *fnkey,
                              const kbd_rgb_config_t *rgb) {
  uint32_t crc = 0;
  crc = KBD_CalcCRC32((const uint8_t *)system, sizeof(*system));
  crc ^= KBD_CalcCRC32((const uint8_t *)keymap, sizeof(*keymap));
  crc ^= KBD_CalcCRC32((const uint8_t *)fnkey, sizeof(*fnkey));
  crc ^= KBD_CalcCRC32((const uint8_t *)rgb, sizeof(*rgb));
  return crc;
}

static uint32_t CalcRuntimeCRC(const kbd_runtime_page_t *page) {
  return KBD_CalcCRC32((const uint8_t *)page, sizeof(*page) - sizeof(page->crc32));
}

static uint16_t KBD_Storage_ProcessEvent(uint8_t task_id, uint16_t events);

static void KBD_Storage_InitTMOSTask(void) {
  if (s_storage_task_id != TASK_NO_TASK) {
    return;
  }
  s_storage_task_id = TMOS_ProcessEventRegister(KBD_Storage_ProcessEvent);
  if (s_storage_task_id == TASK_NO_TASK) {
    LOG_W(TAG, "TMOS storage task register failed");
  }
}

static bool TryLoadRuntimePage(uint8_t page_idx, kbd_runtime_page_t *out) {
  EEPROM_READ(KBD_RUNTIME_PAGE_ADDR(page_idx), out, sizeof(*out));
  if (out->magic != KBD_RUNTIME_MAGIC) return false;
  if (out->version != KBD_RUNTIME_VERSION) return false;
  if (CalcRuntimeCRC(out) != out->crc32) return false;
  return true;
}

static void ApplyRuntimeLayerIfValid(void) {
  bool found = false;
  kbd_runtime_page_t best = {0};
  kbd_runtime_page_t cur;
  uint8_t best_page = KBD_RUNTIME_INVALID_PAGE;

  for (uint8_t page = 0; page < KBD_RUNTIME_PAGE_COUNT; page++) {
    if (!TryLoadRuntimePage(page, &cur)) continue;
    if (!found || cur.seq > best.seq) {
      memcpy(&best, &cur, sizeof(best));
      best_page = page;
      found = true;
    }
  }

  if (!found) {
    s_runtime_active_page = KBD_RUNTIME_INVALID_PAGE;
    s_runtime_seq = 0;
    s_runtime_last_saved_layer = 0xFF;
    s_runtime_pending_mode = 0xFF;
    s_runtime_last_saved_mode = 0xFF;
    return;
  }

  s_runtime_active_page = best_page;
  s_runtime_seq = best.seq;
  s_runtime_pending_layer = best.current_layer;
  s_runtime_last_saved_layer = best.current_layer;
  s_runtime_pending_mode = best.last_mode;
  s_runtime_last_saved_mode = best.last_mode;
  s_runtime_dirty = 0;

  if (best.current_layer < s_keymap_config.num_layers) {
    s_keymap_config.current_layer = best.current_layer;
  }
}

static int SaveRuntimeState(void) {
  __attribute__((aligned(4))) kbd_runtime_page_t page;
  uint8_t target_page;

  memset(&page, 0xFF, sizeof(page));
  page.magic = KBD_RUNTIME_MAGIC;
  page.version = KBD_RUNTIME_VERSION;
  page.flags = 0;
  page.seq = s_runtime_seq + 1;
  page.current_layer = s_runtime_pending_layer;
  page.last_mode = s_runtime_pending_mode;
  page.crc32 = CalcRuntimeCRC(&page);

  if (s_runtime_active_page == KBD_RUNTIME_INVALID_PAGE) {
    target_page = 0;
  } else {
    target_page = (uint8_t)((s_runtime_active_page + 1) % KBD_RUNTIME_PAGE_COUNT);
  }

  if (EEPROM_ERASE(KBD_RUNTIME_PAGE_ADDR(target_page), KBD_CFG_PAGE_SIZE) != 0) {
    LOG_E(TAG, "Erase runtime page failed: %d", target_page);
    return -1;
  }
  if (EEPROM_WRITE(KBD_RUNTIME_PAGE_ADDR(target_page), &page, KBD_CFG_PAGE_SIZE) !=
      0) {
    LOG_E(TAG, "Write runtime page failed: %d", target_page);
    return -2;
  }

  s_runtime_active_page = target_page;
  s_runtime_seq = page.seq;
  s_runtime_last_saved_layer = s_runtime_pending_layer;
  s_runtime_last_saved_mode = s_runtime_pending_mode;
  s_runtime_dirty = 0;
  return 0;
}

static void KBD_Storage_RequestRuntimeSave(void) {
  bool layer_changed = (s_runtime_pending_layer != s_runtime_last_saved_layer);
  bool mode_changed  = (s_runtime_pending_mode != 0xFF) &&
                       (s_runtime_pending_mode != s_runtime_last_saved_mode);

  if (!layer_changed && !mode_changed) {
    s_runtime_dirty = 0;
    if (s_storage_task_id != TASK_NO_TASK) {
      (void)tmos_stop_task(s_storage_task_id, KBD_STORAGE_RUNTIME_SAVE_EVT);
    }
    return;
  }

  s_runtime_dirty = 1;

  if (s_storage_task_id == TASK_NO_TASK) {
    /* 兜底：TMOS 未就绪时同步保存 */
    (void)SaveRuntimeState();
    return;
  }

  /* 防抖合并：layer/mode 快速变化只保存最后一次 */
  (void)tmos_stop_task(s_storage_task_id, KBD_STORAGE_RUNTIME_SAVE_EVT);
  (void)tmos_start_task(s_storage_task_id, KBD_STORAGE_RUNTIME_SAVE_EVT,
                        MS1_TO_SYSTEM_TIME(KBD_STORAGE_RUNTIME_SAVE_DELAY_MS));
}

static uint16_t KBD_Storage_ProcessEvent(uint8_t task_id, uint16_t events) {
  (void)task_id;

  if (events & KBD_STORAGE_RUNTIME_SAVE_EVT) {
    if (s_runtime_dirty) {
      if (SaveRuntimeState() != 0) {
        LOG_W(TAG, "Runtime save failed, retry");
        (void)tmos_start_task(s_storage_task_id, KBD_STORAGE_RUNTIME_SAVE_EVT,
                              MS1_TO_SYSTEM_TIME(
                                  KBD_STORAGE_RUNTIME_SAVE_RETRY_MS));
      }
    } else {
      s_runtime_dirty = 0;
    }
    return (events ^ KBD_STORAGE_RUNTIME_SAVE_EVT);
  }

  if (events & KBD_STORAGE_MACRO_WRITE_EVT) {
    if (s_macro_pending.type != MACRO_OP_IDLE) {
      int ret = -1;
      switch (s_macro_pending.type) {
      case MACRO_OP_WRITE:
        ret = MeowFs_WriteRawInternal(s_macro_pending.offset,
                                      s_macro_pending.data,
                                      s_macro_pending.len);
        break;
      case MACRO_OP_ERASE_PAGE:
        ret = Kbd_Macro_ErasePage(s_macro_pending.erase_page);
        break;
      case MACRO_OP_ERASE_ALL:
        ret = Kbd_Macro_EraseAll();
        break;
      }

      uint8_t sub = s_macro_pending.sub;
      s_macro_pending.type = MACRO_OP_IDLE; /* 先清标志，再发响应 */

      uint8_t resp = (ret == 0) ? KBD_RESP_OK : KBD_RESP_ERR_FLASH;
      KBD_Command_SendResponse(KBD_CMD_MACRO_SET, sub, &resp, 1);

      if (ret != 0) {
        LOG_W(TAG, "Deferred macro op failed: %d", ret);
      }
    }
    return (events ^ KBD_STORAGE_MACRO_WRITE_EVT);
  }

  return 0;
}

static void BuildConfigImage(uint8_t *image, const kbd_config_header_t *header,
                             const kbd_system_config_t *system,
                             const kbd_keymap_t *keymap,
                             const kbd_fnkey_config_t *fnkey,
                             const kbd_rgb_config_t *rgb) {
  memset(image, 0xFF, KBD_CFG_SLOT_SIZE);
  memcpy(image + KBD_FLASH_HEADER, header, sizeof(*header));
  memcpy(image + KBD_FLASH_SYSTEM, system, sizeof(*system));
  memcpy(image + KBD_FLASH_KEYMAP, keymap, sizeof(*keymap));
  memcpy(image + KBD_FLASH_FNKEY, fnkey, sizeof(*fnkey));
  memcpy(image + KBD_FLASH_RGB, rgb, sizeof(*rgb));
}

static void ReadConfigPayloadFromSlot(uint32_t base_addr,
                                      kbd_system_config_t *system,
                                      kbd_keymap_t *keymap,
                                      kbd_fnkey_config_t *fnkey,
                                      kbd_rgb_config_t *rgb) {
  EEPROM_READ(base_addr + KBD_FLASH_SYSTEM, system, sizeof(*system));
  EEPROM_READ(base_addr + KBD_FLASH_KEYMAP, keymap, sizeof(*keymap));
  EEPROM_READ(base_addr + KBD_FLASH_FNKEY, fnkey, sizeof(*fnkey));
  EEPROM_READ(base_addr + KBD_FLASH_RGB, rgb, sizeof(*rgb));
}

static bool TryLoadConfigSlot(uint8_t slot, kbd_config_slot_cache_t *out) {
  const uint32_t base_addr = KBD_CFG_SLOT_ADDR(slot);

  memset(out, 0, sizeof(*out));
  out->slot = slot;
  EEPROM_READ(base_addr + KBD_FLASH_HEADER, &out->header, sizeof(out->header));

  if (out->header.magic != KBD_CONFIG_MAGIC) {
    return false;
  }
  if (out->header.version != KBD_CONFIG_VERSION) {
    return false;
  }

  ReadConfigPayloadFromSlot(base_addr, &out->system, &out->keymap, &out->fnkey,
                            &out->rgb);

  if (CalcConfigCRC(&out->system, &out->keymap, &out->fnkey, &out->rgb) !=
      out->header.crc32) {
    return false;
  }

  return true;
}

static void ApplyLoadedConfig(const kbd_config_slot_cache_t *cfg) {
  memcpy(&s_config_header, &cfg->header, sizeof(s_config_header));
  memcpy(&s_system_config, &cfg->system, sizeof(s_system_config));
  if (s_system_config.os_mode > KBD_OS_MODE_MAC) {
    s_system_config.os_mode = KBD_OS_MODE_WIN;
  }
  if (s_system_config.seamless_wake != KBD_SEAMLESS_WAKE_ENABLED &&
      s_system_config.seamless_wake != KBD_SEAMLESS_WAKE_DISABLED) {
    /* Legacy slots used this byte as reserved; preserve user config and apply
     * the new default without forcing a layout-version reset. */
    s_system_config.seamless_wake = KBD_SEAMLESS_WAKE_ENABLED;
  }
  memcpy(&s_keymap_config, &cfg->keymap, sizeof(s_keymap_config));
  memcpy(&s_fnkey_config, &cfg->fnkey, sizeof(s_fnkey_config));
  memcpy(&s_rgb_config, &cfg->rgb, sizeof(s_rgb_config));
  s_config_active_slot = cfg->slot;
}

static int SaveConfigImageToSlot(uint8_t slot, const uint8_t *image) {
  const uint32_t base_addr = KBD_CFG_SLOT_ADDR(slot);
  __attribute__((aligned(4))) uint8_t old_page[KBD_CFG_PAGE_SIZE];
  bool page_dirty[KBD_CFG_PAGE_COUNT] = {false};
  uint8_t dirty_count = 0;

  for (uint8_t page = 0; page < KBD_CFG_PAGE_COUNT; page++) {
    const uint32_t off = (uint32_t)page * KBD_CFG_PAGE_SIZE;
    EEPROM_READ(base_addr + off, old_page, KBD_CFG_PAGE_SIZE);
    if (memcmp(old_page, image + off, KBD_CFG_PAGE_SIZE) != 0) {
      page_dirty[page] = true;
      dirty_count++;
    }
  }

  if (dirty_count == 0) {
    return 0;
  }

  /* 先写 payload 页，最后写 header 页（page0），确保掉电时老槽位仍可回退 */
  for (uint8_t pass = 0; pass < 2; pass++) {
    uint8_t begin = (pass == 0) ? 1 : 0;
    uint8_t end = (pass == 0) ? KBD_CFG_PAGE_COUNT : 1;
    for (uint8_t page = begin; page < end; page++) {
      if (!page_dirty[page]) continue;
      const uint32_t off = (uint32_t)page * KBD_CFG_PAGE_SIZE;
      if (EEPROM_ERASE(base_addr + off, KBD_CFG_PAGE_SIZE) != 0) {
        LOG_E(TAG, "Erase cfg page failed: slot=%d page=%d", slot, page);
        return -1;
      }
      if (EEPROM_WRITE(base_addr + off, (void *)(image + off), KBD_CFG_PAGE_SIZE) !=
          0) {
        LOG_E(TAG, "Write cfg page failed: slot=%d page=%d", slot, page);
        return -2;
      }
    }
  }

  LOG_D(TAG, "Config slot %d updated (%d/%d pages)", slot, dirty_count,
        KBD_CFG_PAGE_COUNT);
  return 0;
}

/*============================================================================*/
/*                              公共函数实现 */
/*============================================================================*/

uint32_t KBD_CalcCRC32(const uint8_t *data, uint32_t len) {
  uint32_t crc = 0xFFFFFFFF;
  while (len--) {
    crc = s_crc32_table[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFF;
}

void KBD_Storage_DeferRuntimeSave(uint32_t delay_ms) {
  if (s_storage_task_id == TASK_NO_TASK || !s_runtime_dirty) {
    return;
  }
  (void)tmos_stop_task(s_storage_task_id, KBD_STORAGE_RUNTIME_SAVE_EVT);
  (void)tmos_start_task(s_storage_task_id, KBD_STORAGE_RUNTIME_SAVE_EVT,
                        MS1_TO_SYSTEM_TIME(delay_ms));
  LOG_I(TAG, "Runtime save deferred %ums", (unsigned)delay_ms);
}

int KBD_Storage_FlushRuntime(void) {
  if (s_storage_task_id != TASK_NO_TASK) {
    (void)tmos_stop_task(s_storage_task_id, KBD_STORAGE_RUNTIME_SAVE_EVT);
  }

  if (!s_runtime_dirty) {
    return 0;
  }

  return SaveRuntimeState();
}

void KBD_Storage_PollStatus(kbd_storage_status_t *status) {
  if (!status) {
    return;
  }

  status->config_active_slot = s_config_active_slot;
  status->runtime_active_page = s_runtime_active_page;
  status->runtime_dirty = s_runtime_dirty;
  status->reserved = 0;
  status->config_save_count = s_config_header.save_count;
  status->runtime_seq = s_runtime_seq;
}

int KBD_Storage_Init(void) {
  LOG_I(TAG, "Storage init");
  KBD_Storage_InitTMOSTask();

  int ret = KBD_Config_Load();
  if (ret != 0) {
    LOG_W(TAG, "Config load failed (%d), using defaults", ret);
    LoadDefaults();

    /*
     * 首次启动时默认值原先只存在于 RAM，导致 DataFlash 仍是空的，
     * 后续启动和网页读取可能看到不同的配置来源。立即保存一份默认
     * 配置，让 FN、休眠和 RGB 默认值成为设备的实际配置。
     */
    ret = KBD_Config_Save();
    if (ret != 0) {
      LOG_W(TAG, "Default config save failed (%d)", ret);
    }
  }

  LOG_I(TAG, "Effective config: fn1_click=0x%02X fn1_long=0x%02X sleep=%u/%u seamless=%u",
        s_fnkey_config.fn[0].click_action,
        s_fnkey_config.fn[0].long_action,
        s_system_config.auto_sleep_min,
        s_system_config.deep_sleep_min,
        s_system_config.seamless_wake == KBD_SEAMLESS_WAKE_ENABLED ? 1u : 0u);

  return 0;
}

int KBD_Config_Load(void) {
  bool found = false;
  kbd_config_slot_cache_t best = {0};
  kbd_config_slot_cache_t cur;

  for (uint8_t slot = 0; slot < KBD_CFG_SLOT_COUNT; slot++) {
    if (!TryLoadConfigSlot(slot, &cur)) continue;

    if (!found || cur.header.save_count > best.header.save_count) {
      memcpy(&best, &cur, sizeof(best));
      found = true;
    }
  }

  if (!found) {
    LOG_W(TAG, "No valid config slot found");
    s_config_active_slot = KBD_CFG_INVALID_SLOT;
    return -1;
  }

  ApplyLoadedConfig(&best);

#if !KBD_USB_LOG_ENABLE
  s_system_config.log_enabled = 0;
#endif

  /* 热数据（层号）独立覆盖，减少高频切层带来的整份配置写入 */
  ApplyRuntimeLayerIfValid();

  LOG_I(TAG, "Config loaded: slot=%d ver=0x%04X saves=%d", s_config_active_slot,
        s_config_header.version, s_config_header.save_count);
  LOG_D(TAG, "layers=%d, current=%d", s_keymap_config.num_layers,
        s_keymap_config.current_layer);

  return 0;
}

int KBD_Config_Save(void) {
  __attribute__((aligned(4))) uint8_t image[KBD_CFG_SLOT_SIZE];
  kbd_config_header_t new_header = s_config_header;
  uint8_t target_slot;
  int ret;

  LOG_I(TAG, "Saving config...");

  /* 更新头部 */
  new_header.magic = KBD_CONFIG_MAGIC;
  new_header.version = KBD_CONFIG_VERSION;
  new_header.save_count = s_config_header.save_count + 1;
  new_header.crc32 =
      CalcConfigCRC(&s_system_config, &s_keymap_config, &s_fnkey_config, &s_rgb_config);

  if (s_config_active_slot != KBD_CFG_INVALID_SLOT &&
      s_config_header.version == KBD_CONFIG_VERSION &&
      s_config_header.crc32 == new_header.crc32) {
    LOG_I(TAG, "Config unchanged, skip slot write");
    (void)KBD_Storage_FlushRuntime();
    return 0;
  }

  BuildConfigImage(image, &new_header, &s_system_config, &s_keymap_config,
                   &s_fnkey_config, &s_rgb_config);

  if (s_config_active_slot == KBD_CFG_INVALID_SLOT) {
    target_slot = 0;
  } else {
    target_slot = (uint8_t)((s_config_active_slot + 1) % KBD_CFG_SLOT_COUNT);
  }

  ret = SaveConfigImageToSlot(target_slot, image);
  if (ret != 0) {
    return ret;
  }

  memcpy(&s_config_header, &new_header, sizeof(s_config_header));
  s_config_active_slot = target_slot;
  /* 同步 runtime 热数据（当前层） */
  (void)KBD_Storage_FlushRuntime();

  LOG_I(TAG, "Config saved: slot=%d count=%d", s_config_active_slot,
        s_config_header.save_count);
  return 0;
}

int KBD_Config_Reset(void) {
  LOG_I(TAG, "Factory reset");

  LoadDefaults();

  /* 清除整个 MeowFS 宏区 */
  Kbd_Macro_EraseAll();

  return KBD_Config_Save();
}

/*============================================================================*/
/*                              配置访问函数 */
/*============================================================================*/

kbd_system_config_t *KBD_GetSystemConfig(void) { return &s_system_config; }

kbd_keymap_t *KBD_GetKeymap(void) { return &s_keymap_config; }

kbd_fnkey_config_t *KBD_GetFnKeyConfig(void) { return &s_fnkey_config; }

kbd_rgb_config_t *KBD_GetRgbConfig(void) { return &s_rgb_config; }

uint8_t KBD_GetOsMode(void) {
  return (s_system_config.os_mode == KBD_OS_MODE_MAC) ? KBD_OS_MODE_MAC
                                                       : KBD_OS_MODE_WIN;
}

int KBD_SetOsMode(uint8_t mode) {
  if (mode > KBD_OS_MODE_MAC) {
    return -1;
  }
  s_system_config.os_mode = mode;
  return 0;
}

/*============================================================================*/
/*                              层操作函数 */
/*============================================================================*/

uint8_t KBD_GetLastMode(void) { return s_runtime_pending_mode; }

int KBD_SetLastMode(uint8_t mode) {
  if (mode == s_runtime_last_saved_mode && mode == s_runtime_pending_mode) {
    return 0;
  }
  s_runtime_pending_mode = mode;
  KBD_Storage_RequestRuntimeSave();
  return 0;
}

uint8_t KBD_GetCurrentLayer(void) { return s_keymap_config.current_layer; }

int KBD_SetCurrentLayer(uint8_t layer) {
  if (layer >= s_keymap_config.num_layers) {
    return -1;
  }
  if (s_keymap_config.current_layer == layer) {
    return 0; // 未变化, 跳过写 Flash
  }
  s_keymap_config.current_layer = layer;
  s_runtime_pending_layer = layer;
  LOG_D(TAG, "Switch to layer %d, queue runtime save", layer);
  KBD_Storage_RequestRuntimeSave();
  return 0;
}

uint8_t KBD_NextLayer(void) {
  uint8_t next =
      (s_keymap_config.current_layer + 1) % s_keymap_config.num_layers;
  KBD_SetCurrentLayer(next);
  return next;
}

uint8_t KBD_PrevLayer(void) {
  uint8_t prev = (s_keymap_config.current_layer == 0)
                     ? (s_keymap_config.num_layers - 1)
                     : (s_keymap_config.current_layer - 1);
  KBD_SetCurrentLayer(prev);
  return prev;
}

const kbd_action_t *KBD_GetKeyAction(uint8_t key_index) {
  if (key_index >= KBD_MAX_KEYS) {
    return NULL;
  }
  uint8_t layer = s_keymap_config.current_layer;
  if (layer >= s_keymap_config.num_layers) {
    layer = s_keymap_config.default_layer;
  }
  return &s_keymap_config.layers[layer].keys[key_index];
}

/*============================================================================*/
/*                              宏操作函数 */
/*============================================================================*/

static bool IsMeowFsMarker(uint8_t marker) {
  return marker == 0xFF || marker == 0x00 || marker == KBD_MACRO_VALID_MAGIC;
}

static int MeowFs_ReadHeader(uint16_t offset, uint8_t *marker,
                             uint8_t *action_count) {
  uint8_t header[KBD_FLASH_MACRO_HEADER];
  if (offset + KBD_FLASH_MACRO_HEADER > KBD_FLASH_MACRO_SIZE) {
    return -1;
  }

  EEPROM_READ(KBD_FLASH_MACRO_BASE + offset, header, sizeof(header));
  *marker = header[0];
  *action_count = header[1];
  return 0;
}

static int MeowFs_FindMacro(uint8_t index, uint16_t *entry_offset,
                            uint8_t *action_count) {
  uint16_t offset = 0;
  uint8_t current = 0;

  while (offset + KBD_FLASH_MACRO_HEADER <= KBD_FLASH_MACRO_SIZE) {
    uint8_t marker = 0xFF;
    uint8_t count = 0;
    if (MeowFs_ReadHeader(offset, &marker, &count) != 0) {
      return -1;
    }
    if (marker == 0xFF) {
      return -2;
    }
    if (!IsMeowFsMarker(marker)) {
      return -1;
    }

    uint16_t entry_size =
        KBD_FLASH_MACRO_HEADER + ((uint16_t)count * sizeof(kbd_macro_action_t));
    if (offset + entry_size > KBD_FLASH_MACRO_SIZE) {
      return -1;
    }

    if (marker == KBD_MACRO_VALID_MAGIC) {
      if (current == index) {
        if (entry_offset) {
          *entry_offset = offset;
        }
        if (action_count) {
          *action_count = count;
        }
        return 0;
      }
      current++;
    }

    offset += entry_size;
  }

  return -2;
}

static int MeowFs_ReadRawInternal(uint16_t offset, uint8_t *buf, uint16_t len) {
  if (offset >= KBD_FLASH_MACRO_SIZE) {
    return -1;
  }

  if (offset + len > KBD_FLASH_MACRO_SIZE) {
    len = (uint16_t)(KBD_FLASH_MACRO_SIZE - offset);
  }

  EEPROM_READ(KBD_FLASH_MACRO_BASE + offset, buf, len);
  return len;
}

static int MeowFs_WriteRawInternal(uint16_t offset, const uint8_t *buf,
                                   uint16_t len) {
  while (len > 0) {
    uint16_t page_offset = offset & (KBD_FLASH_MACRO_PAGE - 1u);
    uint16_t chunk = (uint16_t)(KBD_FLASH_MACRO_PAGE - page_offset);
    uint32_t page_addr =
        KBD_FLASH_MACRO_BASE + (offset & ~(KBD_FLASH_MACRO_PAGE - 1u));
    __attribute__((aligned(4))) uint8_t page[KBD_FLASH_MACRO_PAGE];

    if (chunk > len) {
      chunk = len;
    }

    if (EEPROM_READ(page_addr, page, sizeof(page)) != 0) {
      return -1;
    }
    memcpy(page + page_offset, buf, chunk);
    if (EEPROM_ERASE(page_addr, KBD_FLASH_MACRO_PAGE) != 0) {
      return -2;
    }
    if (EEPROM_WRITE(page_addr, page, sizeof(page)) != 0) {
      return -3;
    }

    offset = (uint16_t)(offset + chunk);
    buf += chunk;
    len = (uint16_t)(len - chunk);
  }

  return 0;
}

int Kbd_Macro_GetInfo(uint8_t slot, kbd_macro_header_t *header) {
  uint16_t entry_offset = 0;
  uint8_t action_count = 0;

  if (MeowFs_FindMacro(slot, &entry_offset, &action_count) != 0) {
    return -2;
  }

  if (header) {
    memset(header, 0, sizeof(*header));
    header->valid = KBD_MACRO_VALID_MAGIC;
    header->id = slot;
    header->action_count = action_count;
    header->data_size = (uint16_t)(action_count * sizeof(kbd_macro_action_t));
  }

  return 0;
}

int Kbd_Macro_Read(uint8_t slot, uint16_t offset, uint8_t *buf, uint16_t len) {
  uint16_t entry_offset = 0;
  uint8_t action_count = 0;
  uint16_t data_size = 0;

  if (MeowFs_FindMacro(slot, &entry_offset, &action_count) != 0) {
    return -2;
  }

  data_size = (uint16_t)(action_count * sizeof(kbd_macro_action_t));
  if (offset >= data_size) {
    return 0;
  }
  if (offset + len > data_size) {
    len = (uint16_t)(data_size - offset);
  }

  return MeowFs_ReadRawInternal(
      (uint16_t)(entry_offset + KBD_FLASH_MACRO_HEADER + offset), buf, len);
}

int Kbd_Macro_ReadRaw(uint16_t offset, uint8_t *buf, uint16_t len) {
  return MeowFs_ReadRawInternal(offset, buf, len);
}

int Kbd_Macro_WriteRaw(uint16_t offset, const uint8_t *buf, uint16_t len) {
  if (offset + len > KBD_FLASH_MACRO_SIZE) {
    return -1;
  }
  return MeowFs_WriteRawInternal(offset, buf, len);
}

int Kbd_Macro_WriteRawDeferred(uint16_t offset, const uint8_t *buf,
                               uint16_t len, uint8_t sub) {
  if (s_macro_pending.type != MACRO_OP_IDLE) {
    return -1; /* 上一次操作尚未完成 */
  }
  if (offset + len > KBD_FLASH_MACRO_SIZE ||
      len > MACRO_WRITE_BUF_SIZE) {
    return -2;
  }

  s_macro_pending.sub = sub;
  s_macro_pending.offset = offset;
  s_macro_pending.len = len;
  memcpy(s_macro_pending.data, buf, len);
  s_macro_pending.type = MACRO_OP_WRITE;

  if (s_storage_task_id != TASK_NO_TASK) {
    tmos_set_event(s_storage_task_id, KBD_STORAGE_MACRO_WRITE_EVT);
  } else {
    /* 兜底：TMOS 未就绪时同步执行 */
    int ret = MeowFs_WriteRawInternal(offset, s_macro_pending.data, len);
    s_macro_pending.type = MACRO_OP_IDLE;
    uint8_t resp = (ret == 0) ? KBD_RESP_OK : KBD_RESP_ERR_FLASH;
    KBD_Command_SendResponse(KBD_CMD_MACRO_SET, sub, &resp, 1);
  }
  return 0;
}

int Kbd_Macro_EraseDeferred(uint8_t page, uint8_t sub) {
  if (s_macro_pending.type != MACRO_OP_IDLE) {
    return -1;
  }

  s_macro_pending.sub = sub;
  s_macro_pending.erase_page = page;
  s_macro_pending.type = (page == 0xFF) ? MACRO_OP_ERASE_ALL
                                        : MACRO_OP_ERASE_PAGE;

  if (s_storage_task_id != TASK_NO_TASK) {
    tmos_set_event(s_storage_task_id, KBD_STORAGE_MACRO_WRITE_EVT);
  } else {
    int ret = (page == 0xFF) ? Kbd_Macro_EraseAll()
                             : Kbd_Macro_ErasePage(page);
    s_macro_pending.type = MACRO_OP_IDLE;
    uint8_t resp = (ret == 0) ? KBD_RESP_OK : KBD_RESP_ERR_FLASH;
    KBD_Command_SendResponse(KBD_CMD_MACRO_SET, sub, &resp, 1);
  }
  return 0;
}

int Kbd_Macro_ErasePage(uint8_t page_index) {
  if (page_index >= (KBD_FLASH_MACRO_SIZE / KBD_FLASH_MACRO_PAGE)) {
    return -1;
  }

  if (EEPROM_ERASE(KBD_FLASH_MACRO_BASE +
                       ((uint32_t)page_index * KBD_FLASH_MACRO_PAGE),
                   KBD_FLASH_MACRO_PAGE) != 0) {
    return -2;
  }
  return 0;
}

int Kbd_Macro_EraseAll(void) {
  for (uint8_t page = 0; page < (KBD_FLASH_MACRO_SIZE / KBD_FLASH_MACRO_PAGE);
       page++) {
    if (Kbd_Macro_ErasePage(page) != 0) {
      return -1;
    }
  }
  return 0;
}

int Kbd_Macro_Delete(uint8_t slot) {
  uint16_t entry_offset = 0;
  uint8_t action_count = 0;
  uint8_t deleted_marker = 0x00;

  if (MeowFs_FindMacro(slot, &entry_offset, &action_count) != 0) {
    return -1;
  }

  (void)action_count;
  return MeowFs_WriteRawInternal(entry_offset, &deleted_marker, 1);
}

bool Kbd_Macro_IsValid(uint8_t slot) {
  return MeowFs_FindMacro(slot, NULL, NULL) == 0;
}

uint8_t Kbd_Macro_GetUsedCount(void) {
  uint16_t offset = 0;
  uint8_t count = 0;

  while (offset + KBD_FLASH_MACRO_HEADER <= KBD_FLASH_MACRO_SIZE) {
    uint8_t marker = 0xFF;
    uint8_t action_count = 0;
    if (MeowFs_ReadHeader(offset, &marker, &action_count) != 0) {
      break;
    }
    if (marker == 0xFF) {
      break;
    }
    if (!IsMeowFsMarker(marker)) {
      break;
    }

    if (marker == KBD_MACRO_VALID_MAGIC) {
      count++;
    }

    offset +=
        (uint16_t)(KBD_FLASH_MACRO_HEADER +
                   ((uint16_t)action_count * sizeof(kbd_macro_action_t)));
  }

  return count;
}

uint16_t Kbd_Macro_GetUsedBytes(void) {
  uint16_t offset = 0;

  while (offset + KBD_FLASH_MACRO_HEADER <= KBD_FLASH_MACRO_SIZE) {
    uint8_t marker = 0xFF;
    uint8_t action_count = 0;
    if (MeowFs_ReadHeader(offset, &marker, &action_count) != 0) {
      break;
    }
    if (marker == 0xFF) {
      break;
    }
    if (!IsMeowFsMarker(marker)) {
      break;
    }

    offset +=
        (uint16_t)(KBD_FLASH_MACRO_HEADER +
                   ((uint16_t)action_count * sizeof(kbd_macro_action_t)));
  }

  return offset;
}

uint16_t Kbd_Macro_GetFreeBytes(void) {
  return (uint16_t)(KBD_FLASH_MACRO_SIZE - Kbd_Macro_GetUsedBytes());
}

uint16_t Kbd_Macro_GetTotalSize(void) { return KBD_FLASH_MACRO_SIZE; }

uint16_t Kbd_Macro_GetPageSize(void) { return KBD_FLASH_MACRO_PAGE; }

uint8_t Kbd_Macro_IsBusy(void) {
  return (s_macro_pending.type != MACRO_OP_IDLE) ? 1 : 0;
}
