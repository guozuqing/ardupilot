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

    bool red = false, green = false, blue = false;

    if (pg_fault) {
        // 红灯快闪：5Hz（100ms 亮 / 100ms 灭）
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
