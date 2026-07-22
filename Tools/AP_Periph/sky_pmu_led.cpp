#include "AP_Periph.h"

// SKY PMU 自定义 LED 状态指示逻辑
// 状态定义：
//   异常（PG 引脚低电平）        -> 红灯快闪（5Hz）
//   正常且已连接飞控（CAN 有数据）-> 蓝灯闪烁（1Hz）
//   正常但未连接飞控             -> 白灯（红+绿+蓝）闪烁（1Hz）

#if AP_PERIPH_SKY_PMU_LED_ENABLED

#include <hal.h>

#ifndef SKY_PMU_LED_ON
#define SKY_PMU_LED_ON 1
#endif
#define SKY_PMU_LED_OFF (!SKY_PMU_LED_ON)

// 飞控连接超时时间：超过该时间未收到 CAN 报文视为断开
#ifndef SKY_PMU_FC_TIMEOUT_MS
#define SKY_PMU_FC_TIMEOUT_MS 3000
#endif

// PG 低电平去抖时间
#ifndef SKY_PMU_PG_DEBOUNCE_MS
#define SKY_PMU_PG_DEBOUNCE_MS 50
#endif

// MP9931 使能最早时间：上电后至少等待该时间，让电源/传感器稳定
#ifndef SKY_PMU_MP9931_EN_DELAY_MS
#define SKY_PMU_MP9931_EN_DELAY_MS 1000
#endif

// MP9931 兑底使能时间：自检一直不通过（如 INA238 故障）时，
// 超过该时间仍强制使能，避免因传感器故障导致输出无电；设为 0 = 严格模式（自检不过永不使能）
#ifndef SKY_PMU_MP9931_FALLBACK_MS
#define SKY_PMU_MP9931_FALLBACK_MS 5000
#endif

// ---------- 安全保护判定常数（可在 hwdef 中 define 覆盖） ----------
// 过压触发去抖时间
#ifndef SKY_PMU_OV_DEBOUNCE_MS
#define SKY_PMU_OV_DEBOUNCE_MS 100
#endif
// 过功率触发去抖时间（较长，跳过飞控上电浪涌）
#ifndef SKY_PMU_OP_DEBOUNCE_MS
#define SKY_PMU_OP_DEBOUNCE_MS 500
#endif
// 故障恢复判定时间（低于恢复阈值需持续该时间）
#ifndef SKY_PMU_PROT_CLEAR_MS
#define SKY_PMU_PROT_CLEAR_MS 1000
#endif
// 过压恢复滞回（V）：电压需降到 阈值-此值 以下才判恢复
#ifndef SKY_PMU_OV_HYST_V
#define SKY_PMU_OV_HYST_V 2.0f
#endif
// 过功率恢复滞回比例：功率需降到 阈值×此比例 以下才判恢复
#ifndef SKY_PMU_OP_HYST_RATIO
#define SKY_PMU_OP_HYST_RATIO 0.9f
#endif
// 故障持续期间 CAN 告警重发间隔
#ifndef SKY_PMU_WARN_REPEAT_MS
#define SKY_PMU_WARN_REPEAT_MS 5000
#endif
// 短路切断判定去抖时间（尽量短，但需过滤单次采样毛刺）
#ifndef SKY_PMU_SC_DEBOUNCE_MS
#define SKY_PMU_SC_DEBOUNCE_MS 100
#endif

#ifdef HAL_GPIO_PIN_MP9931_EN
// MCU 启动并自检正常后，拉高 PA6 使能 MP9931 降压芯片（只使能一次，不回退）
void AP_Periph_FW::sky_pmu_power_update(void)
{
    if (mp9931_cut) {
        // 短路保护已切断并锁存，禁止重新使能（只能重新上电恢复）
        return;
    }
    if (mp9931_enabled) {
        return;
    }
    const uint32_t now_ms = AP_HAL::millis();
    if (now_ms < SKY_PMU_MP9931_EN_DELAY_MS) {
        // 等待上电稳定
        return;
    }

    // 自检：电池监控（INA238）健康、能正常读到电压
    bool checks_ok = true;
#if AP_PERIPH_BATTERY_ENABLED
    checks_ok = battery_lib.healthy(0);
#endif

    if (!checks_ok) {
#if SKY_PMU_MP9931_FALLBACK_MS > 0
        if (now_ms < SKY_PMU_MP9931_FALLBACK_MS) {
            return;
        }
        // 自检超时，兑底强制使能，避免输出无电
        can_printf_severity(MAV_SEVERITY_WARNING, "MP9931: enable (self-check timeout)");
#else
        return;
#endif
    } else {
        can_printf("MP9931: enable");
    }

    palWriteLine(HAL_GPIO_PIN_MP9931_EN, 1);
    mp9931_enabled = true;
}
#endif // HAL_GPIO_PIN_MP9931_EN

// 安全保护：过压 / 飞控供电过功率监测（只告警，不切断供电）
// 返回 true 表示存在过压/过功率故障；同时维护 CAN 告警与 NodeStatus 健康状态
bool AP_Periph_FW::sky_pmu_protection_update(uint32_t now_ms, bool pg_fault)
{
    static struct {
        uint32_t ov_trip_ms;    // 过压超限起始时刻（去抖）
        uint32_t ov_clear_ms;   // 过压恢复起始时刻
        uint32_t op_trip_ms;    // 过功率超限起始时刻
        uint32_t op_clear_ms;   // 过功率恢复起始时刻
        uint32_t sc_trip_ms;    // 短路超限起始时刻（去抖）
        uint32_t last_warn_ms;  // 上次 CAN 告警时刻
        bool ov_fault;
        bool op_fault;
        bool pg_fault_prev;
    } st;

    // ---------- 采样（INA238 飞控供电支路：电池电压 + 支路电流） ----------
    float volts = 0.0f;
    float power_w = 0.0f;
    bool have_meas = false;
#if AP_PERIPH_BATTERY_ENABLED
    if (battery_lib.healthy(0)) {
        volts = battery_lib.voltage(0);
        float amps;
        if (battery_lib.current_amps(amps, 0)) {
            power_w = volts * amps;
        }
        have_meas = true;
    }
#endif

    // ---------- 过压判定（去抖 + 滞回） ----------
    const float ov_limit = bat_params.get_ov_volt();
    if (!have_meas || !(ov_limit > 0)) {
        st.ov_fault = false;
        st.ov_trip_ms = st.ov_clear_ms = 0;
    } else if (!st.ov_fault) {
        if (volts > ov_limit) {
            if (st.ov_trip_ms == 0) {
                st.ov_trip_ms = now_ms;
            }
            if (now_ms - st.ov_trip_ms >= SKY_PMU_OV_DEBOUNCE_MS) {
                st.ov_fault = true;
                st.ov_clear_ms = 0;
                st.last_warn_ms = 0;    // 触发后立即告警
            }
        } else {
            st.ov_trip_ms = 0;
        }
    } else {
        if (volts < ov_limit - SKY_PMU_OV_HYST_V) {
            if (st.ov_clear_ms == 0) {
                st.ov_clear_ms = now_ms;
            }
            if (now_ms - st.ov_clear_ms >= SKY_PMU_PROT_CLEAR_MS) {
                st.ov_fault = false;
                st.ov_trip_ms = 0;
                can_printf_severity(MAV_SEVERITY_INFO, "SKY_PMU: overvolt cleared (%.1fV)", (double)volts);
            }
        } else {
            st.ov_clear_ms = 0;
        }
    }

    // ---------- 过功率判定（去抖 + 滞回） ----------
    const float op_limit = bat_params.get_max_power();
    if (!have_meas || !(op_limit > 0)) {
        st.op_fault = false;
        st.op_trip_ms = st.op_clear_ms = 0;
    } else if (!st.op_fault) {
        if (power_w > op_limit) {
            if (st.op_trip_ms == 0) {
                st.op_trip_ms = now_ms;
            }
            if (now_ms - st.op_trip_ms >= SKY_PMU_OP_DEBOUNCE_MS) {
                st.op_fault = true;
                st.op_clear_ms = 0;
                st.last_warn_ms = 0;    // 触发后立即告警
            }
        } else {
            st.op_trip_ms = 0;
        }
    } else {
        if (power_w < op_limit * SKY_PMU_OP_HYST_RATIO) {
            if (st.op_clear_ms == 0) {
                st.op_clear_ms = now_ms;
            }
            if (now_ms - st.op_clear_ms >= SKY_PMU_PROT_CLEAR_MS) {
                st.op_fault = false;
                st.op_trip_ms = 0;
                can_printf_severity(MAV_SEVERITY_INFO, "SKY_PMU: overpower cleared (%.1fW)", (double)power_w);
            }
        } else {
            st.op_clear_ms = 0;
        }
    }

#ifdef HAL_GPIO_PIN_MP9931_EN
    // ---------- 短路保护：超过阈值切断 MP9931，锁存直到重新上电 ----------
    const float sc_limit = bat_params.get_sc_power();
    if (!mp9931_cut && have_meas && sc_limit > 0) {
        if (power_w > sc_limit) {
            if (st.sc_trip_ms == 0) {
                st.sc_trip_ms = now_ms;
            }
            if (now_ms - st.sc_trip_ms >= SKY_PMU_SC_DEBOUNCE_MS) {
                palWriteLine(HAL_GPIO_PIN_MP9931_EN, 0);   // 立即切断输出
                mp9931_cut = true;
                can_printf_severity(MAV_SEVERITY_ERROR, "SKY_PMU: SHORT %.0fW>%.0fW cut",
                                    (double)power_w, (double)sc_limit);
            }
        } else {
            st.sc_trip_ms = 0;
        }
    }
#endif // HAL_GPIO_PIN_MP9931_EN

    // ---------- PG 故障边沿告警 ----------
    if (pg_fault && !st.pg_fault_prev) {
        st.last_warn_ms = 0;            // PG 新触发，立即告警
    } else if (!pg_fault && st.pg_fault_prev) {
        can_printf_severity(MAV_SEVERITY_INFO, "SKY_PMU: PG fault cleared");
    }
    st.pg_fault_prev = pg_fault;

    // ---------- CAN 告警（进入立即发，持续期间周期重发） ----------
#ifdef HAL_GPIO_PIN_MP9931_EN
    const bool sc_cut = mp9931_cut;
#else
    const bool sc_cut = false;
#endif
    const bool any_fault = st.ov_fault || st.op_fault || pg_fault || sc_cut;
    if (any_fault &&
        (st.last_warn_ms == 0 || now_ms - st.last_warn_ms >= SKY_PMU_WARN_REPEAT_MS)) {
        st.last_warn_ms = now_ms;
        if (sc_cut) {
            can_printf_severity(MAV_SEVERITY_WARNING, "SKY_PMU: MP9931 off (short latched)");
        }
        if (st.ov_fault) {
            can_printf_severity(MAV_SEVERITY_WARNING, "SKY_PMU: OVERVOLT %.1fV > %.1fV",
                                (double)volts, (double)ov_limit);
        }
        if (st.op_fault) {
            can_printf_severity(MAV_SEVERITY_WARNING, "SKY_PMU: OVERPOWER %.1fW > %.1fW",
                                (double)power_w, (double)op_limit);
        }
        if (pg_fault) {
            can_printf_severity(MAV_SEVERITY_WARNING, "SKY_PMU: PG fault");
        }
    }
    if (!any_fault) {
        st.last_warn_ms = 0;
    }

    // ---------- NodeStatus 健康状态（飞控/调参工具可见） ----------
    set_node_health_warning(any_fault);

    return st.ov_fault || st.op_fault || sc_cut;
}

void AP_Periph_FW::sky_pmu_led_update(void)
{
    const uint32_t now_ms = AP_HAL::millis();

    // ---------- 状态判断 ----------
    // PG 低电平持续超过去抖时间视为异常
    static uint32_t pg_low_start_ms;
    bool pg_fault = false;
    if (palReadLine(HAL_GPIO_PIN_PG) == 0) {
        if (pg_low_start_ms == 0) {
            pg_low_start_ms = now_ms;
        }
        pg_fault = (now_ms - pg_low_start_ms) >= SKY_PMU_PG_DEBOUNCE_MS;
    } else {
        pg_low_start_ms = 0;
    }

    // 已完成节点 ID 分配且近期收到过 CAN 报文，视为已连接飞控
    const bool fc_connected = !no_iface_finished_dna &&
        last_fc_msg_ms != 0 &&
        (now_ms - last_fc_msg_ms) < SKY_PMU_FC_TIMEOUT_MS;

    // 安全保护：过压 / 过功率监测（含 CAN 告警与 NodeStatus 健康状态维护）
    const bool prot_fault = sky_pmu_protection_update(now_ms, pg_fault);

    bool red = false, green = false, blue = false;

    if (pg_fault || prot_fault) {
        // 红灯快闪：5Hz（100ms 亮 / 100ms 灭），故障态优先级最高
        red = (now_ms % 200U) < 100U;
    } else if (fc_connected) {
        // 蓝灯闪烁：1Hz（500ms 亮 / 500ms 灭）
        blue = (now_ms % 1000U) < 500U;
    } else {
        // 白灯闪烁：1Hz（红+绿+蓝同时亮为白色）
        const bool on = (now_ms % 1000U) < 500U;
        red = green = blue = on;
    }

    // ---------- 输出 ----------
    palWriteLine(HAL_GPIO_PIN_LED_RED,   red   ? SKY_PMU_LED_ON : SKY_PMU_LED_OFF);
    palWriteLine(HAL_GPIO_PIN_LED_GREEN, green ? SKY_PMU_LED_ON : SKY_PMU_LED_OFF);
    palWriteLine(HAL_GPIO_PIN_LED_BLUE,  blue  ? SKY_PMU_LED_ON : SKY_PMU_LED_OFF);
}

#endif // AP_PERIPH_SKY_PMU_LED_ENABLED
