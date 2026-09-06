/**
 * @file    kbd_battery.c
 * @brief   电池电压 ADC 采样 + TP4054 充电状态检测
 *
 * 采样电路 (VDD = 2.5V):
 * - PA15 (VBAT_AD_EN): 高电平启动分压电路, 低电平关闭
 * - PA14 (AIN4): VBAT 经两个 100K 电阻分压 (1/2) 后进入 ADC
 * - ADC 配置: 外部通道 CH_EXTIN_4, PGA = 0dB (1x)
 * - Vref = VINTA, 典型值 1.05V
 * - 公式: VBAT_mV = ADC_val * 1050 / 2048 * 2
 * - ADC 输入最大 2.1V < VIO33 2.5V, 安全
 *
 * 充电检测:
 * - PA13 (TP4054 CHRG): 开漏输出, 内部上拉
 *   读高 = 未充电, 读低 = 充电中
 */

#include "kbd_battery.h"
#include "kbd_config.h"
#include "ble_config.h"
#include "debug.h"

#define TAG "BAT"

/* TMOS 事件 */
#define BAT_SAMPLE_EVT 0x0001
#define BAT_PERIODIC_EVT 0x0002
#define BAT_CHARGE_POLL_EVT 0x0004

/* 采样时序 */
#define BAT_SETTLE_MS 30u
#define BAT_PERIODIC_MS (30u * 1000u)
#define BAT_ADC_SAMPLE_COUNT 10u
#define BAT_ADC_CALIB_LIMIT 256
#define BAT_VALID_MIN_MV 2500u
#define BAT_VALID_MAX_MV 4400u
#define BAT_VOLTAGE_JUMP_MV 250u
#define BAT_VOLTAGE_CONFIRM_MV 80u
#define BAT_FILTER_OLD_WEIGHT 3u
#define BAT_FILTER_TOTAL_WEIGHT 4u
#define BAT_CHARGE_POLL_MS 100u
#define BAT_CHARGE_ASSERT_SAMPLES 3u
#define BAT_CHARGE_RELEASE_SAMPLES 100u

/*
 * TP4054 充电时端电压会高于静置电压。没有电流采样和库仑计时，只能做
 * 电压估算；减去一个保守偏移可避免充电中明显高估，CHRG 释放后再允许 100%。
 */
#define BAT_CHARGING_VOLTAGE_BIAS_MV 40u
#define BAT_CHARGING_LEVEL_MAX 99u
#define BAT_LEVEL_HYSTERESIS_PCT 1u
#define BAT_FULL_ASSERT_MV 4160u
#define BAT_FULL_RELEASE_MV 4100u
#define BAT_FULL_ASSERT_SAMPLES 3u
#define BAT_FULL_RELEASE_SAMPLES 2u
#define BAT_FULL_CHARGE_RELEASE_SAMPLES 10u

/*============================================================================*/
/*                              私有变量                                      */
/*============================================================================*/

/** ADC 粗调校准偏移量 */
static int16_t s_adc_calib = 0;
static tmosTaskID s_task_id = TASK_NO_TASK;
static uint16_t s_cached_voltage_mv = 3700;
static uint16_t s_cached_adc_raw = 0;
static uint8_t s_cached_level = 50;
static uint8_t s_cache_ready = FALSE;
static uint8_t s_level_ready = FALSE;
static uint8_t s_sample_pending = FALSE;
static uint16_t s_charge_candidate_samples = 0;
static kbd_charge_state_t s_charge_state = BAT_CHG_NONE;
static kbd_charge_state_t s_last_charge_state = BAT_CHG_NONE;
static uint8_t s_full_assert_samples = 0;
static uint8_t s_full_release_samples = 0;
static uint8_t s_full_latched = FALSE;

/*============================================================================*/
/*                              LiPo 电压 → 百分比                            */
/*============================================================================*/

/**
 * @brief 单节 LiPo 静置电压的分段线性估算曲线
 * @note 电芯型号、温度和负载都会影响结果；该曲线不是库仑计。
 */
static const struct
{
  uint16_t mv;
  uint8_t pct;
} lipo_curve[] = {
    {3000, 0},
    {3300, 3},
    {3500, 8},
    {3600, 12},
    {3700, 20},
    {3750, 30},
    {3800, 45},
    {3850, 60},
    {3900, 70},
    {3950, 80},
    {4000, 87},
    {4050, 92},
    {4100, 96},
    {4150, 98},
    {4180, 100},
    {4200, 100},
};
#define CURVE_LEN (sizeof(lipo_curve) / sizeof(lipo_curve[0]))

static uint8_t voltage_to_percent(uint16_t mv)
{
  if (mv <= lipo_curve[0].mv)
    return 0;
  if (mv >= lipo_curve[CURVE_LEN - 1].mv)
    return 100;

  for (uint8_t i = 1; i < CURVE_LEN; i++)
  {
    if (mv <= lipo_curve[i].mv)
    {
      uint16_t dv = lipo_curve[i].mv - lipo_curve[i - 1].mv;
      uint8_t dp = lipo_curve[i].pct - lipo_curve[i - 1].pct;
      return lipo_curve[i - 1].pct +
             (uint8_t)((uint32_t)(mv - lipo_curve[i - 1].mv) * dp / dv);
    }
  }
  return 100;
}

static uint8_t battery_level_from_voltage(uint16_t mv,
                                          kbd_charge_state_t charge_state)
{
  uint8_t level;

  if (s_full_latched)
  {
    return 100u;
  }

  if (charge_state == BAT_CHG_CHARGING &&
      mv > BAT_CHARGING_VOLTAGE_BIAS_MV)
  {
    mv -= BAT_CHARGING_VOLTAGE_BIAS_MV;
  }

  level = voltage_to_percent(mv);
  if (charge_state == BAT_CHG_CHARGING && level > BAT_CHARGING_LEVEL_MAX)
  {
    level = BAT_CHARGING_LEVEL_MAX;
  }
  return level;
}

static void battery_update_full_latch(uint16_t mv)
{
  if (s_full_latched)
  {
    s_full_assert_samples = 0;
    if (mv <= BAT_FULL_RELEASE_MV)
    {
      if (++s_full_release_samples >= BAT_FULL_RELEASE_SAMPLES)
      {
        s_full_latched = FALSE;
        s_full_release_samples = 0;
        LOG_I(TAG, "Full latch released at %umV", mv);
      }
    }
    else
    {
      s_full_release_samples = 0;
    }
    return;
  }

  s_full_release_samples = 0;
  if (mv >= BAT_FULL_ASSERT_MV)
  {
    if (++s_full_assert_samples >= BAT_FULL_ASSERT_SAMPLES)
    {
      s_full_latched = TRUE;
      s_full_assert_samples = 0;
      LOG_I(TAG, "Full latch asserted at %umV", mv);
    }
  }
  else
  {
    s_full_assert_samples = 0;
  }
}

static void battery_update_level(uint16_t mv, kbd_charge_state_t charge_state)
{
  uint8_t level = battery_level_from_voltage(mv, charge_state);

  if (!s_level_ready || charge_state != s_last_charge_state)
  {
    s_cached_level = level;
  }
  else if ((level > s_cached_level &&
            (uint8_t)(level - s_cached_level) > BAT_LEVEL_HYSTERESIS_PCT) ||
           (level < s_cached_level &&
            (uint8_t)(s_cached_level - level) > BAT_LEVEL_HYSTERESIS_PCT))
  {
    s_cached_level = level;
  }

  s_level_ready = TRUE;
  s_last_charge_state = charge_state;
}

static uint16_t KBD_Battery_ProcessEvent(uint8_t task_id, uint16_t events);

/*============================================================================*/
/*                              ADC 采样                                       */
/*============================================================================*/

/** 初始化外部 ADC 通道用于 VBAT 采样 */
static void adc_vbat_init(void)
{
  /* WCH 外部分压参考实现使用 3.2MHz、0dB 单端采样。 */
  ADC_ExtSingleChSampInit(SampleFreq_3_2, ADC_PGA_0);
  /* 选择 AIN4 通道 (PA14) */
  ADC_ChannelCfg(KBD_VBAT_ADC_CH);
}

/**
 * @brief 多次 ADC 采样，去掉一个最大值和一个最小值后求平均
 * @return 未应用校准偏移的 ADC 原始平均值
 */
static uint16_t adc_sample_raw_trimmed_mean(void)
{
  uint32_t sum = 0;
  uint16_t min = 0xFFFFu;
  uint16_t max = 0u;

  for (uint8_t i = 0; i < BAT_ADC_SAMPLE_COUNT; i++)
  {
    uint16_t sample = ADC_ExcutSingleConver();
    sum += sample;
    if (sample < min)
      min = sample;
    if (sample > max)
      max = sample;
  }

  sum -= min;
  sum -= max;
  return (uint16_t)((sum + ((BAT_ADC_SAMPLE_COUNT - 2u) / 2u)) /
                    (BAT_ADC_SAMPLE_COUNT - 2u));
}

static uint16_t adc_raw_to_voltage_mv(uint16_t adc_raw)
{
  int32_t adc = (int32_t)adc_raw + s_adc_calib;

  if (adc < 0)
    adc = 0;
  if (adc > 4095)
    adc = 4095;

  return (uint16_t)((uint32_t)adc * KBD_ADC_VREF_MV *
                    KBD_VBAT_DIVIDER_NUM /
                    (2048u * KBD_VBAT_DIVIDER_DEN));
}

static uint16_t abs_diff_u16(uint16_t a, uint16_t b)
{
  return (a > b) ? (a - b) : (b - a);
}

static void battery_schedule_periodic_refresh(void)
{
  if (s_task_id != TASK_NO_TASK)
  {
    tmos_start_task(s_task_id, BAT_PERIODIC_EVT, MS1_TO_SYSTEM_TIME(BAT_PERIODIC_MS));
  }
}

static void battery_schedule_charge_poll(void)
{
#if KBD_HAS_CHARGE_DET
  if (s_task_id != TASK_NO_TASK)
  {
    tmos_start_task(s_task_id, BAT_CHARGE_POLL_EVT,
                    MS1_TO_SYSTEM_TIME(BAT_CHARGE_POLL_MS));
  }
#endif
}

static void battery_poll_charge_state(void)
{
#if KBD_HAS_CHARGE_DET
  uint8_t raw = KBD_Battery_GetChargePinRaw();
  kbd_charge_state_t next_state = s_charge_state;

  if (s_full_latched)
  {
    /*
     * 满电后 TP4054 会因板载负载产生短暂补充充电脉冲。高电平稳定约 1 秒
     * 即认为主充电已经结束；锁存期间忽略短低脉冲，避免状态灯再次跳变。
     */
    if (s_charge_state == BAT_CHG_CHARGING && raw != 0u)
    {
      if (++s_charge_candidate_samples >= BAT_FULL_CHARGE_RELEASE_SAMPLES)
      {
        next_state = BAT_CHG_NONE;
      }
    }
    else
    {
      s_charge_candidate_samples = 0;
    }
  }
  else if (s_charge_state == BAT_CHG_CHARGING)
  {
    if (raw != 0u)
    {
      if (++s_charge_candidate_samples >= BAT_CHARGE_RELEASE_SAMPLES)
      {
        next_state = BAT_CHG_NONE;
      }
    }
    else
    {
      s_charge_candidate_samples = 0;
    }
  }
  else
  {
    if (raw == 0u)
    {
      if (++s_charge_candidate_samples >= BAT_CHARGE_ASSERT_SAMPLES)
      {
        next_state = BAT_CHG_CHARGING;
      }
    }
    else
    {
      s_charge_candidate_samples = 0;
    }
  }

  if (next_state != s_charge_state)
  {
    s_charge_state = next_state;
    s_charge_candidate_samples = 0;
    if (s_cache_ready)
    {
      battery_update_level(s_cached_voltage_mv, s_charge_state);
    }
    LOG_I(TAG, "Charge state=%d raw=%u", s_charge_state, raw);
  }
#endif
}

static void battery_start_sample(void)
{
  if (s_task_id == TASK_NO_TASK || s_sample_pending)
  {
    return;
  }

  /* 常开诊断版中该引脚已保持高电平；保留此操作便于恢复脉冲采样。 */
  GPIOA_SetBits(KBD_VBAT_EN_PIN);
  s_sample_pending = TRUE;
  tmos_start_task(s_task_id, BAT_SAMPLE_EVT, MS1_TO_SYSTEM_TIME(BAT_SETTLE_MS));
}

static void battery_finish_sample(void)
{
  uint16_t adc_raw;
  uint16_t measured_mv;
  uint16_t confirm_raw;
  uint16_t confirm_mv;
  uint32_t irq_status;
  uint8_t saved_adc_channel;
  uint8_t saved_adc_cfg;
  uint8_t saved_adc_gain2;
  uint8_t saved_tkey_cfg;
  uint8_t reset_filter = FALSE;
  uint8_t sample_valid = TRUE;

  if (!s_sample_pending)
  {
    return;
  }

  /*
   * BLE 温度校准与电池检测共用 ADC。等待分压稳定期间不能保留一份
   * 可能被覆盖的 ADC 配置；实际转换前重新配置，并在短采样窗口内
   * 防止温度回调切换通道。完成后恢复共享外设原状态。
   */
  SYS_DisableAllIrq(&irq_status);
  saved_adc_channel = R8_ADC_CHANNEL;
  saved_adc_cfg = R8_ADC_CFG;
  saved_adc_gain2 = R8_ADC_CONVERT & RB_ADC_PGA_GAIN2;
  saved_tkey_cfg = R8_TKEY_CFG;

  adc_vbat_init();
  /* 丢弃首次采样，等待模拟前端稳定。 */
  ADC_ExcutSingleConver();
  adc_raw = adc_sample_raw_trimmed_mean();
  measured_mv = adc_raw_to_voltage_mv(adc_raw);

  /*
   * 电池在 30 秒内不应无故跳变数百毫伏。遇到大跳变立即再采一组：
   * 两组一致则接受真实变化，不一致则丢弃瞬态干扰。
   */
  if (s_cache_ready &&
      abs_diff_u16(measured_mv, s_cached_voltage_mv) > BAT_VOLTAGE_JUMP_MV)
  {
    ADC_ExcutSingleConver();
    confirm_raw = adc_sample_raw_trimmed_mean();
    confirm_mv = adc_raw_to_voltage_mv(confirm_raw);
    if (abs_diff_u16(measured_mv, confirm_mv) <= BAT_VOLTAGE_CONFIRM_MV)
    {
      adc_raw = (uint16_t)(((uint32_t)adc_raw + confirm_raw + 1u) / 2u);
      measured_mv = adc_raw_to_voltage_mv(adc_raw);
      reset_filter = TRUE;
    }
    else
    {
      sample_valid = FALSE;
    }
  }

  R8_ADC_CHANNEL = saved_adc_channel;
  R8_ADC_CFG = saved_adc_cfg;
  if (saved_adc_gain2)
  {
    R8_ADC_CONVERT |= RB_ADC_PGA_GAIN2;
  }
  else
  {
    R8_ADC_CONVERT &= ~RB_ADC_PGA_GAIN2;
  }
  R8_TKEY_CFG = saved_tkey_cfg;
  SYS_RecoverIrq(irq_status);

#if !KBD_VBAT_DIVIDER_ALWAYS_ON
  GPIOA_ResetBits(KBD_VBAT_EN_PIN);
#endif

  if (measured_mv < BAT_VALID_MIN_MV || measured_mv > BAT_VALID_MAX_MV)
  {
    sample_valid = FALSE;
  }

  if (!sample_valid)
  {
    LOG_W(TAG, "Reject battery sample: raw=%u mv=%u cached=%u", adc_raw,
          measured_mv, s_cached_voltage_mv);
    s_sample_pending = FALSE;
    return;
  }

  s_cached_adc_raw = adc_raw;
  if (!s_cache_ready || reset_filter)
  {
    s_cached_voltage_mv = measured_mv;
  }
  else
  {
    s_cached_voltage_mv =
        (uint16_t)(((uint32_t)s_cached_voltage_mv * BAT_FILTER_OLD_WEIGHT +
                    measured_mv + (BAT_FILTER_TOTAL_WEIGHT / 2u)) /
                   BAT_FILTER_TOTAL_WEIGHT);
  }
  battery_update_full_latch(s_cached_voltage_mv);
  battery_update_level(s_cached_voltage_mv, KBD_Battery_GetChargeState());
  s_cache_ready = TRUE;
  s_sample_pending = FALSE;
}

/*============================================================================*/
/*                              公共接口                                       */
/*============================================================================*/

void KBD_Battery_Init(void)
{
  /* PA15: 分压使能；诊断版保持高电平，便于直接测量 VBAT_AD。 */
#if KBD_VBAT_DIVIDER_ALWAYS_ON
  GPIOA_SetBits(KBD_VBAT_EN_PIN);
#else
  GPIOA_ResetBits(KBD_VBAT_EN_PIN);
#endif
  GPIOA_ModeCfg(KBD_VBAT_EN_PIN, GPIO_ModeOut_PP_5mA);

  /* PA14: ADC 输入, 浮空 */
  GPIOA_ModeCfg(KBD_VBAT_ADC_PIN, GPIO_ModeIN_Floating);

  /* 初始化 ADC 并获取校准值 */
  adc_vbat_init();
  s_adc_calib = ADC_DataCalib_Rough();
  if (s_adc_calib > BAT_ADC_CALIB_LIMIT)
  {
    LOG_W(TAG, "Battery ADC calib too high: %d -> %d", s_adc_calib,
          BAT_ADC_CALIB_LIMIT);
    s_adc_calib = BAT_ADC_CALIB_LIMIT;
  }
  else if (s_adc_calib < -BAT_ADC_CALIB_LIMIT)
  {
    LOG_W(TAG, "Battery ADC calib too low: %d -> %d", s_adc_calib,
          -BAT_ADC_CALIB_LIMIT);
    s_adc_calib = -BAT_ADC_CALIB_LIMIT;
  }
  s_task_id = TMOS_ProcessEventRegister(KBD_Battery_ProcessEvent);
  if (s_task_id == TASK_NO_TASK)
  {
    LOG_W(TAG, "Battery TMOS task register failed");
  }

#if KBD_HAS_CHARGE_DET
  /* PA13: 充电引脚, 上拉输入 (检测 TP4054 开漏输出) */
  GPIOA_ModeCfg(KBD_CHG_PIN, GPIO_ModeIN_PU);
  s_charge_state = KBD_Battery_GetChargePinRaw() ? BAT_CHG_NONE
                                                 : BAT_CHG_CHARGING;
#endif

  KBD_Battery_RequestRefresh();
  battery_schedule_periodic_refresh();
  battery_schedule_charge_poll();
  LOG_I(TAG, "Battery init: cached=%dmV calib=%d", s_cached_voltage_mv,
        s_adc_calib);
}

void KBD_Battery_RequestRefresh(void)
{
  battery_start_sample();
}

uint16_t KBD_Battery_GetVoltage_mV(void)
{
  if (!s_cache_ready && !s_sample_pending)
  {
    KBD_Battery_RequestRefresh();
  }
  return s_cached_voltage_mv;
}

uint16_t KBD_Battery_GetAdcRaw(void)
{
  return s_cached_adc_raw;
}

uint8_t KBD_Battery_GetLevel(void)
{
  if (!s_level_ready && !s_sample_pending)
  {
    KBD_Battery_RequestRefresh();
  }

  if (!s_level_ready)
  {
    return battery_level_from_voltage(s_cached_voltage_mv,
                                      KBD_Battery_GetChargeState());
  }

  return s_cached_level;
}

uint8_t KBD_Battery_GetChargePinRaw(void)
{
#if KBD_HAS_CHARGE_DET
  return (GPIOA_ReadPortPin(KBD_CHG_PIN) != 0) ? 1u : 0u;
#else
  return 1u;
#endif
}

kbd_charge_state_t KBD_Battery_GetChargeState(void)
{
  return s_charge_state;
}

uint8_t KBD_Battery_GetVoltage_dV(void)
{
  return (uint8_t)(KBD_Battery_GetVoltage_mV() / 100);
}

void KBD_Battery_Suspend(void)
{
  if (s_task_id != TASK_NO_TASK)
  {
    tmos_stop_task(s_task_id, BAT_SAMPLE_EVT);
    tmos_stop_task(s_task_id, BAT_PERIODIC_EVT);
    tmos_stop_task(s_task_id, BAT_CHARGE_POLL_EVT);
  }
#if !KBD_VBAT_DIVIDER_ALWAYS_ON
  /* 低功耗版在休眠时关闭分压。 */
  GPIOA_ResetBits(KBD_VBAT_EN_PIN);
#endif
  s_sample_pending = FALSE;
}

void KBD_Battery_Resume(void)
{
#if KBD_VBAT_DIVIDER_ALWAYS_ON
  GPIOA_SetBits(KBD_VBAT_EN_PIN);
#endif
  KBD_Battery_RequestRefresh();
  battery_schedule_periodic_refresh();
  battery_schedule_charge_poll();
}

static uint16_t KBD_Battery_ProcessEvent(uint8_t task_id, uint16_t events)
{
  (void)task_id;

  if (events & BAT_SAMPLE_EVT)
  {
    battery_finish_sample();
    return (events ^ BAT_SAMPLE_EVT);
  }

  if (events & BAT_PERIODIC_EVT)
  {
    if (!s_sample_pending)
    {
      battery_start_sample();
    }
    battery_schedule_periodic_refresh();
    return (events ^ BAT_PERIODIC_EVT);
  }

  if (events & BAT_CHARGE_POLL_EVT)
  {
    battery_poll_charge_state();
    battery_schedule_charge_poll();
    return (events ^ BAT_CHARGE_POLL_EVT);
  }

  return 0;
}
