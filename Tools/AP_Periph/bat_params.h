#pragma once
// 头文件只会被编译器包含一次，避免重复定义

#include <AP_Param/AP_Param.h>
// 引入 ArduPilot 参数系统（AP_Param）
// 该系统负责把类成员与参数存储/地面站同步等功能关联起来

// 自定义 BAT 参数类 —— 用于把“电池端电压”换算为“剩余电量(SoC)”
// 典型用途：DroneCAN/AP_Periph 电池节点本地估算 SoC，或在主控侧做电压法 SoC。
class BATParams {
public:
    // AP_Param 元信息表：把类成员映射为可配置参数（名称、默认值、范围等）
    // 必须在 .cpp 中提供定义（含默认值），并在构造函数里调用 setup_object_defaults()
    static const struct AP_Param::GroupInfo var_info[];

    BATParams(void);
    // 构造函数：通常会调用 AP_Param::setup_object_defaults(this, var_info)
    // 以便把 var_info 中的默认值写入成员变量，并向参数系统注册

    /* 禁止拷贝（防止参数对象被无意复制，保持与 AP_Param 的单实例绑定） */
    CLASS_NO_COPY(BATParams);

    // —— 只读访问接口（用于其他模块读取当前配置）——
    // 电池串数（S 数），例如 4S/6S；范围通常 1–14
    uint8_t get_cell_num()      const { return cell_num; }

    // 单体“满电”电压阈值（建议单位：V）
    // 典型：LiPo 4.20V，Li-ion 4.10–4.20V。
    // ⚠ 你的注释写了 mV，但类型为 AP_Float（浮点），易混淆。
    //    建议统一用“V”作单位；若坚持 mV，请把变量名改 *_mv，并使用整型。
    float   get_full_voltage()  const { return full_voltage; }

    // 单体“低电”电压阈值（单位：V）
    // 典型：3.50V（负载下可 3.45–3.60V 视工况与压降而定）
    float   get_low_voltage()   const { return low_voltage; }

    // 额定容量（单位：mAh），用于展示/估算（若采用纯电压法也可只做参考）
    // 典型：5000（=5Ah）、10000（=10Ah）
    uint16_t get_capacity()     const { return capacity; }

    // 通过“总电压（包电压，单位：V）”估算 SoC（0–100%）
    // 计算思路：
    //   v_cell = V_pack / cell_num
    //   在 [low_voltage, full_voltage] 区间按锂电池典型 OCV 放电曲线插值到 [0, 100]，并钳制
    float calculate_soc_from_voltage(float voltage) const;

    // 发布链路调用的 SoC 更新入口（约 10Hz）：
    //   上电稳定期判据 -> 电压一阶低通滤波 -> OCV 曲线映射 -> 输出变化限速 -> 整数上报滞回
    // 返回 true 时 soc_pct 为可直接填入 state_of_charge_pct 的百分比（0–100）；
    // 返回 false 表示上电初期电压采样尚未稳定（INA238 首次转换可能读到 0/偏低），
    // 此时不应发布该电池的 BatteryInfo
    bool update_soc(float voltage, float dt, uint8_t instance, float &soc_pct);

private:
    // ===== 电压法 SoC 估算的运行状态（按电池实例区分） =====
    static constexpr uint8_t SOC_MAX_INSTANCES = 2;
    struct SocState {
        float v_filt;        // 低通滤波后的包电压（V）
        float v_last;        // 上一次采样电压（稳定期判据用）
        float soc;           // 连续 SoC 估计（%）
        float soc_reported;  // 最近一次上报的整数百分比
        float run_s;         // 落位后的运行时间（s，用于快速收敛窗口）
        uint8_t settle_count; // 连续稳定采样计数
        bool  initialised;   // 稳定落位后置位
    } _soc_state[SOC_MAX_INSTANCES];

    // ===== 与 AP_Param 绑定的参数成员 =====
    // 注意：成员的“类型”决定了参数的存储/序列化方式。

    AP_Int8  cell_num;      // 电池串数（S）；默认可设 6（6S）
                            // 取值 <=0 时建议在实现里做保护/估算

    AP_Float full_voltage;  // 单体满电电压阈值（建议单位：V；典型 4.20）
                            // ⚠ 若按 mV 使用，请在 .cpp 的 GroupInfo 与注释中明确单位，并配合整型类型

    AP_Float low_voltage;   // 单体低电电压阈值（建议单位：V；典型 3.50）
                            // 低于该阈值时 SoC 应逐步接近 0%，并触发低电逻辑（上层）

    AP_Int16 capacity;      // 额定容量（mAh）；典型 5000/10000
                            // 若需要 >32Ah，可改为 AP_Int32
};
