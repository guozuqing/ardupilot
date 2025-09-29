#include "AP_Periph.h"
#include "bat_params.h"

// BAT参数组始终启用 (独立于AP_PERIPH_BATTERY_ENABLED)

// 默认值定义
#ifndef AP_PERIPH_BAT_CELL_NUM_DEFAULT
#define AP_PERIPH_BAT_CELL_NUM_DEFAULT 6
#endif

#ifndef AP_PERIPH_BAT_FULL_VOLTAGE_DEFAULT
#define AP_PERIPH_BAT_FULL_VOLTAGE_DEFAULT 4200
#endif

#ifndef AP_PERIPH_BAT_LOW_VOLTAGE_DEFAULT
#define AP_PERIPH_BAT_LOW_VOLTAGE_DEFAULT 3500
#endif

#ifndef AP_PERIPH_BAT_CAPACITY_DEFAULT
#define AP_PERIPH_BAT_CAPACITY_DEFAULT 5000
#endif

const AP_Param::GroupInfo BATParams::var_info[] = {
    // @Param: _CELL_NUM
    // @DisplayName: Number of battery cells
    // @Description: Number of cells in series in the battery pack
    // @Range: 1 24
    // @Increment: 1
    // @User: Standard
    AP_GROUPINFO("_CELL_NUM", 1, BATParams, cell_num, AP_PERIPH_BAT_CELL_NUM_DEFAULT),

    // @Param: _FULL_VOLTAGE
    // @DisplayName: Full cell voltage
    // @Description: Voltage of a single cell when fully charged
    // @Units: mV
    // @Range: 3000 5000
    // @Increment: 10
    // @User: Standard
    AP_GROUPINFO("_FULL_VOLTAGE", 2, BATParams, full_voltage, AP_PERIPH_BAT_FULL_VOLTAGE_DEFAULT),

    // @Param: _LOW_VOLTAGE
    // @DisplayName: Low cell voltage
    // @Description: Voltage of a single cell when considered low
    // @Units: mV
    // @Range: 2500 4000
    // @Increment: 10
    // @User: Standard
    AP_GROUPINFO("_LOW_VOLTAGE", 3, BATParams, low_voltage, AP_PERIPH_BAT_LOW_VOLTAGE_DEFAULT),

    // @Param: _CAPACITY
    // @DisplayName: Battery capacity
    // @Description: Battery capacity in mAh
    // @Units: mAh
    // @Range: 100 50000
    // @Increment: 100
    // @User: Standard
    AP_GROUPINFO("_CAPACITY", 4, BATParams, capacity, AP_PERIPH_BAT_CAPACITY_DEFAULT),

    AP_GROUPEND
};

BATParams::BATParams(void)
{
    AP_Param::setup_object_defaults(this, var_info);
}

// 通过电压计算电量百分比 (精确的锂电池SOC算法)
float BATParams::calculate_soc_from_voltage(float voltage) const
{
    if (cell_num <= 0) {
        return 0.0f;
    }
    
    // 计算单体电池电压
    float cell_voltage = voltage / cell_num;
    
    // 转换为V
    float full_v = full_voltage / 1000.0f; // 满电电压
    float low_v = low_voltage / 1000.0f;   // 低电压
    
    // 边界检查
    if (cell_voltage >= full_v) {
        return 100.0f;
    }
    if (cell_voltage <= low_v) {
        return 0.0f;
    }
    
    // 基于实际锂电池放电曲线的精确SOC计算
    // 使用更准确的电压-SOC对应关系
    float soc;
    
    // 锂电池标准电压点 (基于实测数据)
    float v_95 = full_v - 0.02f;  // 95%: ~4.18V
    float v_80 = full_v - 0.15f;  // 80%: ~4.05V  
    float v_60 = full_v - 0.25f;  // 60%: ~3.95V
    float v_40 = full_v - 0.35f;  // 40%: ~3.85V
    float v_20 = full_v - 0.50f;  // 20%: ~3.70V
    float v_10 = full_v - 0.60f;  // 10%: ~3.60V
    
    if (cell_voltage >= v_95) {
        // 95%-100%: 满电段
        soc = 95.0f + (cell_voltage - v_95) / (full_v - v_95) * 5.0f;
    } else if (cell_voltage >= v_80) {
        // 80%-95%: 高电量段
        soc = 80.0f + (cell_voltage - v_80) / (v_95 - v_80) * 15.0f;
    } else if (cell_voltage >= v_60) {
        // 60%-80%: 中高电量段
        soc = 60.0f + (cell_voltage - v_60) / (v_80 - v_60) * 20.0f;
    } else if (cell_voltage >= v_40) {
        // 40%-60%: 中等电量段
        soc = 40.0f + (cell_voltage - v_40) / (v_60 - v_40) * 20.0f;
    } else if (cell_voltage >= v_20) {
        // 20%-40%: 中低电量段
        soc = 20.0f + (cell_voltage - v_20) / (v_40 - v_20) * 20.0f;
    } else if (cell_voltage >= v_10) {
        // 10%-20%: 低电量段
        soc = 10.0f + (cell_voltage - v_10) / (v_20 - v_10) * 10.0f;
    } else {
        // 0%-10%: 极低电量段
        soc = (cell_voltage - low_v) / (v_10 - low_v) * 10.0f;
    }
    
    return constrain_float(soc, 0.0f, 100.0f);
}
