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

#ifndef AP_PERIPH_BAT_OV_VOLT_DEFAULT
#define AP_PERIPH_BAT_OV_VOLT_DEFAULT 60
#endif

#ifndef AP_PERIPH_BAT_MAX_POWER_DEFAULT
#define AP_PERIPH_BAT_MAX_POWER_DEFAULT 30
#endif

#ifndef AP_PERIPH_BAT_SC_POWER_DEFAULT
#define AP_PERIPH_BAT_SC_POWER_DEFAULT 100
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

    // @Param: _OV_VOLT
    // @DisplayName: Over voltage protection threshold
    // @Description: Bus voltage above this triggers red LED fast blink and CAN warning. 0 disables the check
    // @Units: V
    // @Range: 0 100
    // @Increment: 0.5
    // @User: Standard
    AP_GROUPINFO("_OV_VOLT", 5, BATParams, ov_volt, AP_PERIPH_BAT_OV_VOLT_DEFAULT),

    // @Param: _MAX_POWER
    // @DisplayName: FC supply max power
    // @Description: FC supply branch power (VxI from INA238) above this triggers red LED fast blink and CAN warning. 0 disables the check
    // @Units: W
    // @Range: 0 500
    // @Increment: 1
    // @User: Standard
    AP_GROUPINFO("_MAX_POWER", 6, BATParams, max_power, AP_PERIPH_BAT_MAX_POWER_DEFAULT),

    // @Param: _SC_POWER
    // @DisplayName: Short circuit cutoff power
    // @Description: FC supply branch power above this cuts off the MP9931 buck converter output (latched until reboot) and sends an error to the GCS. 0 disables the check
    // @Units: W
    // @Range: 0 500
    // @Increment: 1
    // @User: Standard
    AP_GROUPINFO("_SC_POWER", 7, BATParams, sc_power, AP_PERIPH_BAT_SC_POWER_DEFAULT),

    AP_GROUPEND
};

BATParams::BATParams(void)
{
    AP_Param::setup_object_defaults(this, var_info);
}

// SoC 估算行为常数
static constexpr float SOC_VOLT_FILT_TAU = 4.0f;   // 电压低通滤波时间常数（s）
static constexpr float SOC_RISE_RATE     = 2.0f;   // SoC 上升限速（%/s，抑制回跳）
static constexpr float SOC_FALL_RATE     = 5.0f;   // SoC 下降限速（%/s，平滑阶跃）
static constexpr float SOC_REPORT_HYST   = 0.6f;   // 整数上报滞回（%，防止相邻百分比跳动）

// 上电稳定期判据：INA238 刚配置完成、首次转换未结束时寄存器可能读到 0/偏低电压，
// 若直接用其落位，SoC 会被种在低值后受上升限速拖累，表现为"电量从低慢慢变高"
static constexpr float SOC_MIN_CELL_VOLT   = 2.5f;  // 单体电压低于此值视为采样无效（V）
static constexpr float SOC_SETTLE_BAND     = 0.02f; // 相邻采样单体电压差阈值（V）
static constexpr uint8_t SOC_SETTLE_COUNT  = 5;     // 需连续稳定的采样个数（10Hz 下约 0.5s）
static constexpr float SOC_FAST_WINDOW_S   = 10.0f; // 落位后的快速收敛窗口（s）
static constexpr float SOC_FAST_RATE       = 20.0f; // 快速收敛窗口内的限速（%/s）

// 通过电压计算电量百分比：锂电池典型 OCV 放电曲线插值
// 本板只给飞控供电，负载电流小且恒定，端电压≈开路电压，电压法 SoC 可信
float BATParams::calculate_soc_from_voltage(float voltage) const
{
    if (cell_num <= 0) {
        return 0.0f;
    }

    // 计算单体电池电压
    const float cell_voltage = voltage / cell_num;

    // 参数为 mV，转换为 V
    const float full_v = full_voltage / 1000.0f; // 满电电压（SoC=100% 锚点）
    const float low_v  = low_voltage / 1000.0f;  // 低电压（SoC=0% 锚点）
    const float span   = full_v - low_v;

    if (span < 0.05f) {
        // 参数配置异常，退化为阈值判断
        return (cell_voltage >= full_v) ? 100.0f : 0.0f;
    }

    // 归一化电压：0 = BAT_LOW_VOLTAGE，1 = BAT_FULL_VOLTAGE
    const float x = (cell_voltage - low_v) / span;

    // 典型锂电池静态放电曲线，按 [低电压, 满电电压] 区间归一化
    // 注释中的绝对电压对应默认参数 3.500V ~ 4.200V
    struct CurvePoint { float x; float soc; };
    static const CurvePoint curve[] = {
        {0.000f,   0.0f},   // 3.500V
        {0.157f,   5.0f},   // 3.610V
        {0.271f,  10.0f},   // 3.690V
        {0.329f,  20.0f},   // 3.730V
        {0.386f,  30.0f},   // 3.770V
        {0.429f,  40.0f},   // 3.800V
        {0.486f,  50.0f},   // 3.840V
        {0.529f,  60.0f},   // 3.870V
        {0.643f,  70.0f},   // 3.950V
        {0.743f,  80.0f},   // 4.020V
        {0.871f,  90.0f},   // 4.110V
        {0.929f,  95.0f},   // 4.150V
        {1.000f, 100.0f},   // 4.200V
    };
    const uint8_t n = ARRAY_SIZE(curve);

    if (x <= curve[0].x) {
        return 0.0f;
    }
    if (x >= curve[n - 1].x) {
        return 100.0f;
    }

    // 分段线性插值
    for (uint8_t k = 1; k < n; k++) {
        if (x < curve[k].x) {
            const float f = (x - curve[k - 1].x) / (curve[k].x - curve[k - 1].x);
            return curve[k - 1].soc + f * (curve[k].soc - curve[k - 1].soc);
        }
    }
    return 100.0f;
}

// 发布链路的 SoC 更新：稳定期判据 + 滤波 + 曲线映射 + 限速 + 上报滞回
bool BATParams::update_soc(float voltage, float dt, uint8_t instance, float &soc_pct)
{
    if (instance >= SOC_MAX_INSTANCES) {
        // 超出状态槽位时退化为无状态曲线映射
        soc_pct = calculate_soc_from_voltage(voltage);
        return true;
    }
    SocState &s = _soc_state[instance];

    if (!s.initialised) {
        // 上电稳定期：要求电压合理且连续 SOC_SETTLE_COUNT 个采样保持稳定后才落位，
        // 避免用 INA238 上电初期的 0/偏低读数为 SoC 播种
        const float cells = (cell_num > 0) ? (float)cell_num : 1.0f;
        const bool plausible = (voltage > SOC_MIN_CELL_VOLT * cells);
        const bool stable = (fabsf(voltage - s.v_last) < SOC_SETTLE_BAND * cells);
        s.v_last = voltage;

        if (!plausible || !stable) {
            s.settle_count = 0;
            return false;
        }
        if (++s.settle_count < SOC_SETTLE_COUNT) {
            return false;
        }

        // 稳定后直接落位，立即显示真实电量
        s.v_filt = voltage;
        s.soc = calculate_soc_from_voltage(voltage);
        s.soc_reported = roundf(s.soc);
        s.run_s = 0.0f;
        s.initialised = true;
        soc_pct = s.soc_reported;
        return true;
    }

    dt = constrain_float(dt, 0.0f, 1.0f);
    if (s.run_s < SOC_FAST_WINDOW_S) {
        s.run_s += dt;
    }

    // 电压一阶低通滤波，抑制采样噪声
    const float alpha = dt / (SOC_VOLT_FILT_TAU + dt);
    s.v_filt += (voltage - s.v_filt) * alpha;

    // OCV 曲线映射 + 输出变化限速（上升慢、下降稍快）；
    // 落位后的短暂窗口内放宽限速，快速修正可能残留的落位偏差
    const float soc_target = calculate_soc_from_voltage(s.v_filt);
    const bool fast = (s.run_s < SOC_FAST_WINDOW_S);
    const float rise = (fast ? SOC_FAST_RATE : SOC_RISE_RATE) * dt;
    const float fall = (fast ? SOC_FAST_RATE : SOC_FALL_RATE) * dt;
    s.soc += constrain_float(soc_target - s.soc, -fall, rise);

    // 整数上报滞回：变化超过阈值才更新对外百分比
    if (fabsf(s.soc - s.soc_reported) > SOC_REPORT_HYST) {
        s.soc_reported = roundf(s.soc);
    }
    soc_pct = s.soc_reported;
    return true;
}
