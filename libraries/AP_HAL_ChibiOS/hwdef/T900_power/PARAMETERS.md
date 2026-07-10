# SKY_PMU (T900_power) 参数说明

固件：AP_Periph（DroneCAN 外设），通过 DroneCAN GUI / Mission Planner CAN 参数页配置。
标注"**需重启**"的参数修改后需给节点重新上电才生效。

---

## 1. 自定义电量参数（BAT_，SOC 电压算法）

用于通过电压估算电量百分比（`state_of_charge_pct`），定义于 `Tools/AP_Periph/bat_params.cpp`，
由 `battery.cpp` 发布链路调用（10Hz）。本板只给飞控供电，电流积分法几乎不消耗容量（恒显 99%），
因此 SOC 完全由电压法计算。

算法流程：

1. 上电稳定期判据：单体电压 > 2.5V 且相邻采样差 < 0.02V/芯、连续 5 个采样（约 0.5s）后才落位；
   未稳定前**不发布 BatteryInfo**（避免 INA238 上电初期的 0/偏低读数把 SOC 种在低值，导致电量从低慢慢爬升）；
2. 包电压一阶低通滤波（时间常数 4s），抑制采样噪声；
3. 换算单体电压后在 `[BAT_LOW_VOLTAGE, BAT_FULL_VOLTAGE]` 区间归一化，按锂电池典型 OCV 放电曲线
   分段线性插值得到 SOC（13 个曲线点，默认参数下 3.50V→0%、3.69V→10%、3.84V→50%、4.02V→80%、4.20V→100%）；
4. 输出变化限速（上升 2%/s、下降 5%/s；落位后 10s 内为快速收敛窗口 20%/s，用于修正残留落位偏差）；
5. 整数上报滞回（0.6%），防止相邻百分比之间来回跳动。

稳定后首帧直接显示真实电量（上电后约 0.5~1s 开始发布）；因本板由被测电池供电，换电池必然重新上电，滤波状态自动复位。

| 参数 | 作用 | 可选内容 | 默认值 |
|---|---|---|---|
| `BAT_CELL_NUM` | 电池串数（S 数），用于把总电压换算为单体电压；≤0 时回退到容量积分法 | 1 ~ 24，步进 1 | 6 |
| `BAT_FULL_VOLTAGE` | 单体满电电压（mV），SOC=100% 的曲线锚点 | 3000 ~ 5000，步进 10 | 4200 |
| `BAT_LOW_VOLTAGE` | 单体低电压（mV），SOC=0% 的曲线锚点 | 2500 ~ 4000，步进 10 | 3500 |
| `BAT_CAPACITY` | 电池标称容量（mAh），用于折算 BatteryInfo 的 Wh 容量字段 | 100 ~ 50000，步进 100 | 5000 |

---

## 2. 电池监控参数（BATT_，AP_BattMonitor / INA238）

| 参数 | 作用 | 可选内容 | 默认值 |
|---|---|---|---|
| `BATT_MONITOR` | 电池监控类型（**需重启**） | 0:禁用，21:INA2XX(I2C)，26:INA239(SPI)，8:DroneCAN-BatteryInfo 等（本板固定用 21） | 21 |
| `BATT_I2C_BUS` | INA238 所在 I2C 总线号（**需重启**） | 0 ~ 3（本板 I2C1 = 总线 0） | 0 |
| `BATT_I2C_ADDR` | INA238 I2C 地址（**需重启**） | 0 ~ 127；0 = 自动探测支持的地址列表；64 = 0x40（A0/A1 接地） | 64 |
| `BATT_SHUNT` | 分流电阻阻值（Ω），电流换算依据 | 0.0001 ~ 0.01；本板 R22 = 5mΩ → 0.005 | 0.005 |
| `BATT_MAX_AMPS` | INA2XX 电流量程上限（A），影响内部换算精度 | 1 ~ 400；注意 5mΩ 分流电阻下 INA238 硬件最大可测约 ±32.7A | 90 |
| `BATT_CAPACITY` | 电池容量（mAh），仅用于容量积分回退路径；Wh 容量字段改用 `BAT_CAPACITY` 折算 | ≥0，步进 50 | 5000 |
| `BATT_SERIAL_NUM` | 电池序列号；≥0 时作为 DroneCAN `battery_id` 上报，飞控端按此 ID 匹配 | -1 = 未指定（用实例号做 ID）；≥0 = 指定序列号 | -1 |
| `BATT_OPTIONS` | 电池监控行为选项（位掩码） | bit0:忽略 DroneCAN SoC，bit6:向 GCS 发送内阻补偿电压，bit7:允许 InfoAux 来自其他 CAN 节点，bit8:电池仅供飞控内部使用（其余位为 MPPT 专用，本板无效） | 0 |

> 说明：本固件已裁剪为**只发布电压**（`HAL_PERIPH_BATTERY_SKIP_CURRENT`），BatteryInfo 中电流恒为 0，
> 但 `BATT_SHUNT` / `BATT_MAX_AMPS` 仍影响 INA238 本地采样配置，建议保持默认。

---

## 3. CAN 总线参数

| 参数 | 作用 | 可选内容 | 默认值 |
|---|---|---|---|
| `CAN_NODE` | 本节点 DroneCAN 节点 ID（**需重启**） | 0 = 由飞控 DNA 服务器动态分配；1 ~ 127 = 静态指定（忽略 DNA） | 0 |
| `CAN_BAUDRATE` | CAN 总线波特率（bit/s）（**需重启**） | 10000 ~ 1000000，需与飞控 `CAN_P1_BITRATE` 一致 | 1000000 |

---

## 4. 系统参数

| 参数 | 作用 | 可选内容 | 默认值 |
|---|---|---|---|
| `FORMAT_VERSION` | 参数存储格式版本号，与固件不匹配时会**清空全部参数恢复默认**，一般不要手动修改 | 只读性质（当前为 2） | 2 |
| `BRD_SERIAL_NUM` | 设备序列号，>0 时会附加显示在 CAN App Name 字符串中（如 "org.ardupilot.sky_pmu SN 123"） | 0 ~ 2147483648 | 0 |
| `FLASH_BOOTLOADER` | 触发 Bootloader 更新：设为 1 后固件把内嵌的新 Bootloader 写入引导区（危险操作，写入期间勿断电），完成后自动归零 | 0 = 不操作；1 = 执行刷写 | 0 |
| `DEBUG` | 调试选项（位掩码） | bit0:通过 CAN 打印各线程剩余栈空间，bit1:上电 15 秒后自动重启（测试用），bit2:发送 CAN 协议统计报文 | 0 |
| `OPTIONS` | AP_Periph 行为选项（位掩码） | bit0:持续探测传感器（上电未找到 INA238 时不断重试） | 0 |

---

## 发送的 CAN 协议内容（DroneCAN）

### 周期性广播

| 消息类型 | 消息 ID | 频率 | 内容 |
|---|---|---|---|
| `uavcan.protocol.NodeStatus` | 341 | 1Hz | 节点心跳：运行时间 `uptime_sec`、健康状态 `health`（OK）、模式 `mode`（OPERATIONAL）、厂商状态码（剩余内存字节数） |
| `uavcan.equipment.power.BatteryInfo` | 1092 | 10Hz | 电池信息，字段明细见下表 |

`BatteryInfo` 字段明细：

| 字段 | 内容 |
|---|---|
| `voltage` | 母线总电压（V），来自 INA238 VBUS 采样 |
| `current` | **恒为 0**（本固件已裁剪电流发布，`HAL_PERIPH_BATTERY_SKIP_CURRENT`） |
| `temperature` | INA238 芯片温度（单位开尔文，非电池温度） |
| `state_of_charge_pct` | 电量百分比，由 `BAT_` 参数组的电压法 SOC 算法计算（OCV 曲线 + 滤波 + 限速，见第 1 节）；仅当 `BAT_CELL_NUM` ≤ 0 时回退到 AP_BattMonitor 容量积分 |
| `state_of_health_pct` | 127（UNKNOWN，不支持健康度） |
| `battery_id` | `BATT_SERIAL_NUM` ≥ 0 时为该值，否则为实例号 1 |
| `model_instance_id` | 1 |
| `model_name` | `"SKY_PMU_80 <序列号>"` |
| `full_charge_capacity_wh` | 满电容量（Wh）= `BAT_CAPACITY`/1000 × `BAT_CELL_NUM` × 3.7V 标称单体电压 |
| `remaining_capacity_wh` | 剩余容量（Wh）= 满电容量 × SOC |
| 其余字段 | `status_flags`、`average_power_10sec`、`hours_to_full_charge` 均为 0 |

> `ardupilot.equipment.power.BatteryInfoAux`（单体电压）仅在监控芯片支持单体采样时发送，INA238 不支持，本板**不发送**。

### 启动阶段

| 消息类型 | 消息 ID | 说明 |
|---|---|---|
| `uavcan.protocol.dynamic_node_id.Allocation` | 1 | `CAN_NODE=0` 时广播，向飞控 DNA 服务器请求节点 ID，分配完成后停止 |

### 服务应答（响应飞控 / 调参工具请求）

| 服务 | 服务 ID | 功能 |
|---|---|---|
| `uavcan.protocol.GetNodeInfo` | 1 | 返回节点名 `org.ardupilot.sky_pmu`、固件版本、Git 哈希、硬件唯一 ID |
| `uavcan.protocol.param.GetSet` | 11 | 参数读取 / 写入（本文档所有参数即通过此服务配置） |
| `uavcan.protocol.param.ExecuteOpcode` | 10 | 参数保存 / 恢复出厂默认 |
| `uavcan.protocol.RestartNode` | 5 | 远程重启节点 |
| `uavcan.protocol.file.BeginFirmwareUpdate` | 40 | 启动 CAN 固件升级（升级过程中以客户端身份发送 `file.Read`（48）请求拉取固件数据） |

### 调试 / 可选发送

| 消息类型 | 消息 ID | 触发条件 | 内容 |
|---|---|---|---|
| `uavcan.protocol.debug.LogMessage` | 16383 | 有调试输出时 | `can_printf` 文本（内部错误 IERR、栈使用情况等） |
| `dronecan.protocol.Stats` / `CanStats` | 342 / 343 | `DEBUG` bit2 置位时随心跳发送 | CAN 收发/错误统计 |

### 接收的报文（供参考）

| 消息类型 | 消息 ID | 用途 |
|---|---|---|
| `uavcan.protocol.NodeStatus` | 341 | 检测飞控在线状态（驱动 LED 蓝/白切换，3 秒超时） |
| `uavcan.equipment.safety.ArmingStatus` | 1100 | 飞控解锁状态广播（当前未使用） |
| `uavcan.protocol.dynamic_node_id.Allocation` | 1 | DNA 分配应答（仅在未获得节点 ID 时接收） |
| 上表 5 个服务的请求帧 | — | 参数配置、重启、固件升级等 |

---

## 常用配置示例

- **改用 12S 电池、16000mAh**：`BAT_CELL_NUM=12`，`BAT_CAPACITY=16000`，`BATT_CAPACITY=16000`
- **总线上有多个 PMU**：分别设置不同 `BATT_SERIAL_NUM`（如 1、2），飞控端按 battery_id 区分
- **固定节点 ID**：`CAN_NODE=125`（避开飞控常用的 10 及其他节点）
- **升级 Bootloader**：先烧录新固件（内含新 Bootloader），再设 `FLASH_BOOTLOADER=1`，等待 CAN 上报 "Flash bootloader OK" 后重新上电
