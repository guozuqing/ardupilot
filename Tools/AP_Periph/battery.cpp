#include "AP_Periph.h"

#if AP_PERIPH_BATTERY_ENABLED

/*
  电池（Battery）经由 DroneCAN/UAVCAN 上报的支持逻辑
  - 从 ArduPilot 的 Battery library 读取电压/电流/温度等
  - 组帧为 UAVCAN BatteryInfo / ArduPilot BatteryInfoAux 并广播
*/

#include <dronecan_msgs.h>

extern const AP_HAL::HAL &hal;

#ifndef AP_PERIPH_BATTERY_MODEL_NAME
// 默认使用节点名作为电池“型号名”前缀
#define AP_PERIPH_BATTERY_MODEL_NAME CAN_APP_NODE_NAME
#endif

/*
  定期更新并发送 CAN 电池信息
*/
void AP_Periph_FW::can_battery_update(void)
{
    const uint32_t now_ms = AP_HAL::millis();

    // 发送节流：小于 100ms 则本次不发（~10Hz 发送速率）
    if (now_ms - battery.last_can_send_ms < 100) {
        return;
    }
    battery.last_can_send_ms = now_ms;

    // Battery library 中的电池实例数（支持多块电池）
    const uint8_t battery_instances = battery_lib.num_instances();

    for (uint8_t i = 0; i < battery_instances; i++) {

        // 根据掩码隐藏某个实例（例如用户不希望某块电池对外广播）
        if (BIT_IS_SET(g.battery_hide_mask, i)) {
            continue;
        }

        // 仅对“健康”的电池实例发送
        if (!battery_lib.healthy(i)) {
            continue;
        }

        // UAVCAN v0 消息：uavcan.equipment.power.BatteryInfo
        uavcan_equipment_power_BatteryInfo pkt {};

        // battery_id：优先用电池序列号（>=0 有效），否则用实例序号 i+1
        const int32_t serial_number = battery_lib.get_serial_number(i);
        pkt.battery_id = (serial_number >= 0) ? serial_number : i + 1;

        // 总电压（单位 V）
        pkt.voltage = battery_lib.voltage(i);

        // 电流（单位 A，放电为正；库函数返回是否可用）
        float current;
        if (battery_lib.current_amps(current, i)) {
            pkt.current = current;
        }

        // 温度：Battery library 以摄氏度给出，UAVCAN 要求以开尔文发送
        float temperature;
        if (battery_lib.get_temperature(temperature, i)) {
            pkt.temperature = C_TO_KELVIN(temperature); // K = °C + 273.15
        }

        // ====== 利用本地 BAT 参数填充附加信息（容量、串数等）======
        // 注意：以下为“软信息”，实际单体电压仍取决于硬件/上游是否提供
        uint8_t cell_count = periph.bat_params.get_cell_num();
        if (cell_count > 0) {

            // 额定容量（mAh）；用于给 full_charge_capacity_wh 做一个估算
            uint16_t capacity_mah = periph.bat_params.get_capacity();
            if (capacity_mah > 0) {
                // Wh ≈ mAh * V / 1000；这里用当前总电压近似估算满充容量
                // 严格来说“满充容量”应为额定容量 * 额定电压，此处用当前电压是折中做法
                pkt.full_charge_capacity_wh = (capacity_mah * pkt.voltage) / 1000.0f;
            }

            // 说明：当前硬件 INA239 只测总电压；单体电压需要额外电路。
            // SOC 计算中如需 per-cell，可用“总电压/串数”粗略估计单体电压。
        }

        // ====== 健康度（State of Health, SOH）======
        // 默认未知；如果上游提供，就覆盖
        pkt.state_of_health_pct = UAVCAN_EQUIPMENT_POWER_BATTERYINFO_STATE_OF_HEALTH_UNKNOWN;
        uint8_t state_of_health_pct = 0;
        if (battery_lib.get_state_of_health_pct(i, state_of_health_pct)) {
            pkt.state_of_health_pct = state_of_health_pct; // 0–100 (%)
        }

        // ====== 电量（State of Charge, SOC）策略 ======
        // 优先采用“电压法”估算 SOC，避免依赖电流积分（电流积分漂移/复位依赖较强）
        float voltage = battery_lib.voltage(i);
        if (voltage > 0) {
            // periph.bat_params.calculate_soc_from_voltage(v)
            // 典型实现：基于每芯满/空电压、内阻补偿等近似出 0–100%
            float soc = periph.bat_params.calculate_soc_from_voltage(voltage);
            pkt.state_of_charge_pct = (uint8_t)soc; // 取整到 0–100
        } else {
            // 若电压不可用，退回 Battery library 的剩余电量百分比（如果有）
            uint8_t percentage = 0;
            if (battery_lib.capacity_remaining_pct(percentage, i)) {
                pkt.state_of_charge_pct = percentage;
            }
        }

        // model_instance_id：用于区分同型号的多块电池
        pkt.model_instance_id = i + 1;

#if !defined(HAL_PERIPH_BATTERY_SKIP_NAME)
        // model_name：可读性更好的“型号/标识”，如 "org.ardupilot.ap_periph SN 123"
        // 注意：此处包含序列号 long int 格式化，若 serial_number<0 则打印 -1
        hal.util->snprintf((char*)pkt.model_name.data,
                           sizeof(pkt.model_name.data),
                           "%s %ld",
                           AP_PERIPH_BATTERY_MODEL_NAME,
                           (long int)serial_number);
        pkt.model_name.len = strnlen((char*)pkt.model_name.data,
                                     sizeof(pkt.model_name.data));
#endif // HAL_PERIPH_BATTERY_SKIP_NAME

        // ====== 编码并广播 BatteryInfo ======
        // buffer 大小使用生成的最大尺寸常量
        uint8_t buffer[UAVCAN_EQUIPMENT_POWER_BATTERYINFO_MAX_SIZE];

        // canfdout()==false -> 遵循经典 CAN MTU；true -> CAN FD（更大帧）
        const uint16_t total_size =
            uavcan_equipment_power_BatteryInfo_encode(&pkt, buffer, !periph.canfdout());

        // 以“低优先级”广播（电池信息通常不需要高优先级）
        canard_broadcast(UAVCAN_EQUIPMENT_POWER_BATTERYINFO_SIGNATURE,
                         UAVCAN_EQUIPMENT_POWER_BATTERYINFO_ID,
                         CANARD_TRANSFER_PRIORITY_LOW,
                         &buffer[0],
                         total_size);

        // 如果有单体电压，就顺带发扩展消息（ArduPilot 自定义 BatteryInfoAux）
        if (battery_lib.has_cell_voltages(i)) {
            can_battery_send_cells(i);
        }
    }
}

/*
  发送单体电压（ArduPilot 扩展：ardupilot.equipment.power.BatteryInfoAux）
  - 该包可能较大，避免放栈上，因此使用动态分配
*/
void AP_Periph_FW::can_battery_send_cells(uint8_t instance)
{
    // 动态分配大包；若分配失败，立即清理并返回
    auto* pkt = NEW_NOTHROW ardupilot_equipment_power_BatteryInfoAux;
    uint8_t* buffer = NEW_NOTHROW uint8_t[ARDUPILOT_EQUIPMENT_POWER_BATTERYINFOAUX_MAX_SIZE];
    if (pkt == nullptr || buffer == nullptr) {
        delete pkt;          // pkt 可能是 nullptr，delete 安全
        delete [] buffer;    // 同上
        return;
    }

    // 从 Battery library 取单体电压（通常以 mV 存入 16-bit）
    const auto &cell_voltages = battery_lib.get_cell_voltages(instance);

    // 逐个拷贝到 UAVCAN 消息（要求单位 V）
    // 0xFFFFU 作为“终止”标记（无效/结束）
    for (uint8_t i = 0; i < ARRAY_SIZE(cell_voltages.cells); i++) {
        if (cell_voltages.cells[i] == 0xFFFFU) {
            break;
        }
        pkt->voltage_cell.data[i] = cell_voltages.cells[i] * 0.001f; // mV -> V
        pkt->voltage_cell.len = i + 1;                                // 已填入的长度
    }

    // 可选字段：最大放电电流、标称电压（此处未提供，用 NaN 占位）
    pkt->max_current     = nanf("");
    pkt->nominal_voltage = nanf("");

    // 编码与发送
    const uint16_t total_size =
        ardupilot_equipment_power_BatteryInfoAux_encode(pkt, buffer, !periph.canfdout());

    canard_broadcast(ARDUPILOT_EQUIPMENT_POWER_BATTERYINFOAUX_SIGNATURE,
                     ARDUPILOT_EQUIPMENT_POWER_BATTERYI_
