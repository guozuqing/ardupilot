# SKY_PMU_80 PMU外设编译与执行逻辑详解

## 📋 目录
1. [编译流程分析](#编译流程分析)
2. [硬件定义解析](#硬件定义解析)
3. [主函数执行逻辑](#主函数执行逻辑)
4. [PMU功能链路](#PMU功能链路)
5. [代码执行时序](#代码执行时序)

---

## 编译流程分析

### 1. 编译命令执行路径
```bash
./waf configure --board SKY_PMU_80
./waf AP_Periph
```

### 2. hwdef.dat解析过程 (chibios_pins.py)

#### 步骤1: 硬件定义文件解析
```python
# 位置: Tools/scripts/chibios_pins.py
# 读取: libraries/AP_HAL_ChibiOS/hwdef/SKY_PMU_80/hwdef.dat

解析顺序：
1. MCU定义 → STM32L431 STM32L431xx
2. Flash配置 → FLASH_RESERVE_START_KB 40
3. 引脚定义 → SPI1总线 (PA5/PA6/PA7/PB0)
4. 外设启用 → AP_PERIPH_BATTERY_ENABLED 1
5. 自定义宏 → AP_PERIPH_BAT_*_DEFAULT 值
```

#### 步骤2: 生成编译配置
```cpp
// 生成: build/SKY_PMU_80/hwdef.h
#define AP_PERIPH_BATTERY_ENABLED 1
#define AP_BATTERY_INA239_ENABLED 1
#define AP_PERIPH_BAT_CELL_NUM_DEFAULT 6
#define AP_PERIPH_BAT_FULL_VOLTAGE_DEFAULT 4200
// ... 其他宏定义
```

#### 步骤3: 参数文件处理
```bash
# 输入: libraries/AP_HAL_ChibiOS/hwdef/SKY_PMU_80/defaults.parm
# 输出: build/SKY_PMU_80/processed_defaults.parm
# 嵌入: 作为ROMFS文件嵌入到固件中
```

### 3. 编译依赖链
```
hwdef.dat → hwdef.h → 条件编译 → 目标文件 → 链接 → 固件
    ↓         ↓         ↓          ↓        ↓       ↓
  硬件配置   编译宏   选择性编译   .o文件   ELF   .hex/.bin
```

---

## 硬件定义解析

### 1. MCU和Flash配置
```cpp
// hwdef.dat中的定义
MCU STM32L431 STM32L431xx              // MCU类型
FLASH_RESERVE_START_KB 40              // 应用程序起始地址
FLASH_SIZE_KB 256                      // Flash总大小

// 编译时生成的配置
#define STM32L431xx 1
#define FLASH_TOTAL 221184              // (256-40)*1024
#define FLASH_RESERVE_START_KB 40
```

### 2. SPI总线配置 (INA239传感器)
```cpp
// hwdef.dat中的定义
PA5 SPI1_SCK SPI1                      // SPI时钟
PA6 SPI1_MISO SPI1                     // SPI数据输入
PA7 SPI1_MOSI SPI1                     // SPI数据输出
PB0 INA239_CS CS                       // 片选信号
SPIDEV INA23X SPI1 DEVID1 INA239_CS MODE1 10*MHZ 10*MHZ

// 编译时生成的SPI设备表
static const struct spi_device_table {
    {"INA23X", SPI1, DEVID1, PB0, MODE1, 10000000, 10000000}
};
```

### 3. CAN总线配置
```cpp
// hwdef.dat中的定义
PB8 CAN1_RX CAN1                       // CAN接收
PB9 CAN1_TX CAN1                       // CAN发送
CAN_ORDER 1                            // CAN总线顺序

// 编译时生成的CAN配置
#define HAL_NUM_CAN_IFACES 1
#define CAN1_RX_PIN PB8
#define CAN1_TX_PIN PB9
```

### 4. 自定义BAT参数宏定义
```cpp
// hwdef.dat中的定义
define AP_PERIPH_BAT_CELL_NUM_DEFAULT 6
define AP_PERIPH_BAT_FULL_VOLTAGE_DEFAULT 4200
define AP_PERIPH_BAT_LOW_VOLTAGE_DEFAULT 3500
define AP_PERIPH_BAT_CAPACITY_DEFAULT 5000

// 编译时在bat_params.cpp中使用
#ifndef AP_PERIPH_BAT_CELL_NUM_DEFAULT
#define AP_PERIPH_BAT_CELL_NUM_DEFAULT 6
#endif
```

---

## 主函数执行逻辑

### 1. 启动入口点
```cpp
// 位置: Tools/AP_Periph/AP_Periph.cpp
// 函数: int main(void)

int main(void)
{
    hal.init(0, nullptr);           // HAL层初始化
    
    // 创建主程序实例
    static AP_Periph_FW periph;
    
    // 初始化外设功能
    periph.init();
    
    // 进入主循环
    periph.update();                // 永不返回
    
    return 0;
}
```

### 2. 初始化流程 (periph.init())
```cpp
void AP_Periph_FW::init(void)
{
    // 1. 基础系统初始化
    AP_Param::setup_sketch_defaults();  // 加载参数默认值
    AP_Param::load_all();               // 从Flash加载参数
    
    // 2. CAN总线初始化
    can_start();                        // 启动CAN接口
    
    // 3. 传感器初始化
#if AP_PERIPH_BATTERY_ENABLED
    battery_lib.init();                 // 初始化电池监控库
    // INA239传感器会在这里被检测和配置
#endif
    
    // 4. BAT参数初始化
    // bat_params实例已在构造函数中初始化
    // 参数值从defaults.parm或Flash中加载
    
    // 5. 其他外设初始化...
}
```

### 3. 主循环执行 (periph.update())
```cpp
void AP_Periph_FW::update(void)
{
    while (true) {                      // 无限循环
        uint32_t now = AP_HAL::millis();
        
        // 处理CAN消息接收
        can_update();
        
        // 电池数据更新 (10Hz)
        static uint32_t last_battery_update_ms;
        if (now - last_battery_update_ms >= 100) {
            last_battery_update_ms = now;
            can_battery_update();       // 核心PMU功能
        }
        
        // 其他外设更新...
        
        // 让出CPU时间
        hal.scheduler->delay(1);
    }
}
```

---

## PMU功能链路

### 1. INA239传感器初始化链路
```
hwdef.dat定义 → SPI设备表 → battery_lib.init() → INA239驱动初始化
     ↓              ↓              ↓                    ↓
SPIDEV INA23X → spi_device_table → AP_BattMonitor → AP_BattMonitor_INA239
```

#### 详细代码路径：
```cpp
// 1. hwdef.dat解析后生成SPI设备表
// 位置: build/SKY_PMU_80/hwdef.h
static const struct spi_device_table spi_device_table[] = {
    {"INA23X", 1, 1, PB0, 1, 10000000, 10000000}
};

// 2. 电池监控库初始化
// 位置: libraries/AP_BattMonitor/AP_BattMonitor.cpp
void AP_BattMonitor::init() {
    // 根据HAL_BATT_MONITOR_DEFAULT=26创建INA239实例
    drivers[0] = NEW_NOTHROW AP_BattMonitor_INA239(*this, state[0], _params[0]);
}

// 3. INA239驱动初始化
// 位置: libraries/AP_BattMonitor/AP_BattMonitor_INA239.cpp
void AP_BattMonitor_INA239::init(void) {
    dev = hal.spi->get_device_ptr("INA23X");  // 获取SPI设备
    dev->register_periodic_callback(25000, timer); // 25ms周期读取
}
```

### 2. BAT参数系统链路
```
hwdef.dat宏定义 → bat_params.cpp默认值 → Parameters.cpp注册 → 用户界面显示
      ↓                  ↓                    ↓               ↓
默认值宏定义 → AP_GROUPINFO参数定义 → GOBJECT注册 → 参数可配置
```

#### 详细代码路径：
```cpp
// 1. 默认值定义 (编译时)
// 位置: Tools/AP_Periph/bat_params.cpp
#ifndef AP_PERIPH_BAT_CELL_NUM_DEFAULT
#define AP_PERIPH_BAT_CELL_NUM_DEFAULT 6    // 来自hwdef.dat
#endif

// 2. 参数组定义
const AP_Param::GroupInfo BATParams::var_info[] = {
    AP_GROUPINFO("_CELL_NUM", 1, BATParams, cell_num, AP_PERIPH_BAT_CELL_NUM_DEFAULT),
    // ... 其他参数
};

// 3. 参数系统注册
// 位置: Tools/AP_Periph/Parameters.cpp
GOBJECT(bat_params, "BAT", BATParams),  // 注册为BAT参数组

// 4. 参数实例创建
// 位置: Tools/AP_Periph/AP_Periph.h
class AP_Periph_FW {
    BATParams bat_params;               // 参数实例
};
```

### 3. 电量计算链路 (核心PMU功能)
```
INA239读取 → 电压数据 → BAT参数计算 → SOC结果 → DroneCAN消息 → 主飞控
    ↓          ↓          ↓           ↓          ↓           ↓
25ms周期 → 24.34V → calculate_soc → 80% → BatteryInfo → battery_status
```

#### 详细代码执行：
```cpp
// 1. 传感器数据读取 (25ms周期)
// 位置: libraries/AP_BattMonitor/AP_BattMonitor_INA239.cpp
void AP_BattMonitor_INA239::timer(void) {
    int16_t bus_voltage, current;
    read_word(REG_BUS_VOLTAGE, bus_voltage);  // 读取电压寄存器
    read_word(REG_CURRENT, current);          // 读取电流寄存器
    
    // 转换为实际值
    voltage = bus_voltage * 3.125e-3;         // 24.34V
    current_amps = current * current_LSB;     // 0.071A
}

// 2. 电量计算调用 (100ms周期)
// 位置: Tools/AP_Periph/battery.cpp
void AP_Periph_FW::can_battery_update(void) {
    float voltage = battery_lib.voltage(i);   // 获取电压: 24.34V
    
    // 核心计算调用
    float soc = periph.bat_params.calculate_soc_from_voltage(voltage);
    
    // 详细计算过程:
    // cell_voltage = 24.34V ÷ 6 = 4.057V
    // 根据7段算法: 4.057V → 80%电量
}

// 3. 7段SOC算法执行
// 位置: Tools/AP_Periph/bat_params.cpp
float BATParams::calculate_soc_from_voltage(float voltage) const {
    float cell_voltage = voltage / cell_num;  // 4.057V
    
    // 判断电压段
    if (cell_voltage >= 4.05V) {             // 80%-95%段
        soc = 80% + (4.057-4.05)/(4.18-4.05) × 15% = 80.8%
    }
    
    return 80%;  // 返回计算结果
}

// 4. DroneCAN消息构建和发送
uavcan_equipment_power_BatteryInfo pkt;
pkt.voltage = 24.34;                         // 总电压
pkt.current = 0.071;                         // 电流
pkt.state_of_charge_pct = 80;                // 计算的电量
pkt.full_charge_capacity_wh = 121.75;        // 容量信息

canard_broadcast(...);                       // 通过CAN发送
```

---

## 硬件定义解析

### 1. chibios_pins.py解析逻辑
```python
# 工具位置: Tools/scripts/chibios_pins.py
# 执行时机: ./waf configure --board SKY_PMU_80

def process_hwdef_file():
    # 1. 读取hwdef.dat文件
    lines = read_file("libraries/AP_HAL_ChibiOS/hwdef/SKY_PMU_80/hwdef.dat")
    
    # 2. 解析每行配置
    for line in lines:
        if line.startswith('MCU'):
            setup_mcu_config()           # 配置MCU类型
        elif line.startswith('PA5 SPI1_SCK'):
            setup_spi_pin()              # 配置SPI引脚
        elif line.startswith('define'):
            add_macro_define()           # 添加宏定义
        elif line.startswith('SPIDEV'):
            add_spi_device()             # 添加SPI设备
    
    # 3. 生成hwdef.h文件
    generate_hwdef_header()
    
    # 4. 生成引脚映射表
    generate_pin_tables()
```

### 2. 关键配置项解析

#### SPI设备配置解析：
```cpp
// hwdef.dat输入:
SPIDEV INA23X SPI1 DEVID1 INA239_CS MODE1 10*MHZ 10*MHZ

// 解析结果 (hwdef.h):
#define HAL_SPI_DEVICE_LIST \
    {{"INA23X", 1, 1, PB0, 1, 10000000, 10000000}}

// 运行时使用:
hal.spi->get_device_ptr("INA23X")  // 获取INA239设备句柄
```

#### 宏定义处理：
```cpp
// hwdef.dat输入:
define AP_PERIPH_BAT_CELL_NUM_DEFAULT 6

// 直接输出到hwdef.h:
#define AP_PERIPH_BAT_CELL_NUM_DEFAULT 6

// 在源码中使用:
#ifndef AP_PERIPH_BAT_CELL_NUM_DEFAULT
#define AP_PERIPH_BAT_CELL_NUM_DEFAULT 6  // 如果未定义则使用默认值
#endif
```

---

## 主函数执行逻辑

### 1. 程序入口点
```cpp
// 位置: Tools/AP_Periph/AP_Periph.cpp
// 编译后的main函数执行顺序

int main(void)
{
    // === 第一阶段：HAL初始化 ===
    hal.init(0, nullptr);
    ↓
    // HAL_ChibiOS初始化
    // - 时钟配置 (8MHz外部晶振)
    // - GPIO初始化
    // - SPI/CAN外设初始化
    // - 中断系统配置
    
    // === 第二阶段：AP_Periph实例创建 ===
    static AP_Periph_FW periph;
    ↓
    // 构造函数执行：
    // - 参数系统初始化
    // - bat_params实例创建
    // - 其他外设实例创建
    
    // === 第三阶段：功能初始化 ===
    periph.init();
    ↓
    // 详见下面init()详解
    
    // === 第四阶段：主循环 ===
    periph.update();  // 永不返回
}
```

### 2. 初始化详解 (periph.init())
```cpp
void AP_Periph_FW::init(void)
{
    // === 参数系统初始化 ===
    AP_Param::setup_sketch_defaults();
    ↓
    // 1. 加载ROMFS中的defaults.parm
    // 2. 设置BAT参数默认值:
    //    BAT_CELL_NUM = 6
    //    BAT_FULL_VOLTAGE = 4200
    //    BAT_LOW_VOLTAGE = 3500
    //    BAT_CAPACITY = 5000
    
    AP_Param::load_all();
    ↓
    // 从Flash加载用户保存的参数值
    
    // === CAN系统初始化 ===
    can_start();
    ↓
    // 1. 初始化CAN1接口 (PB8/PB9)
    // 2. 设置节点ID (自动分配为125)
    // 3. 启动DroneCAN协议栈
    
    // === 电池系统初始化 ===
#if AP_PERIPH_BATTERY_ENABLED
    battery_lib.init();
    ↓
    // 1. 根据HAL_BATT_MONITOR_DEFAULT=26创建INA239实例
    // 2. 初始化SPI通信
    // 3. 配置INA239寄存器:
    //    - 分流电阻: 0.0002Ω
    //    - 最大电流: 90A
    //    - ADC配置: 连续转换模式
    // 4. 启动25ms周期定时器
#endif
    
    // === BAT参数初始化 ===
    // bat_params在构造函数中已完成:
    // - 参数组注册到AP_Param系统
    // - 默认值设置完成
    // - 用户界面可见
}
```

### 3. 主循环详解 (periph.update())
```cpp
void AP_Periph_FW::update(void)
{
    while (true) {
        uint32_t now = AP_HAL::millis();
        
        // === CAN消息处理 (高优先级) ===
        can_update();
        ↓
        // 处理接收到的CAN消息
        // - 参数读写请求
        // - 节点状态查询
        // - 时间同步等
        
        // === 电池数据更新 (10Hz) ===
        static uint32_t last_battery_ms;
        if (now - last_battery_ms >= 100) {
            last_battery_ms = now;
            
#if AP_PERIPH_BATTERY_ENABLED
            can_battery_update();       // 详见下面PMU链路
#endif
        }
        
        // === 其他外设更新 ===
        // GPS、磁罗盘、气压计等...
        
        // === 系统维护 ===
        hal.scheduler->delay(1);        // 让出CPU，防止看门狗复位
    }
}
```

---

## PMU功能链路 (核心)

### 1. 数据采集链路 (25ms周期)
```cpp
// === 硬件层：INA239传感器 ===
INA239芯片 (SPI地址: INA23X)
↓ (SPI通信: PA5/PA6/PA7/PB0)
STM32L431 SPI1外设
↓
// === 驱动层：定时器中断 ===
// 位置: libraries/AP_BattMonitor/AP_BattMonitor_INA239.cpp
void AP_BattMonitor_INA239::timer(void) {  // 25ms执行一次
    // 1. 读取电压寄存器
    read_word(REG_BUS_VOLTAGE, bus_voltage);
    voltage = bus_voltage * 3.125e-3;       // 转换为实际电压
    
    // 2. 读取电流寄存器  
    read_word(REG_CURRENT, current);
    current_amps = current * current_LSB;   // 转换为实际电流
    
    // 3. 累积数据供主循环使用
    accumulate.volt_sum += voltage;
    accumulate.current_sum += current_amps;
    accumulate.count++;
}
```

### 2. 数据处理链路 (100ms周期)
```cpp
// === 应用层：电池数据更新 ===
// 位置: Tools/AP_Periph/battery.cpp
void AP_Periph_FW::can_battery_update(void) {
    // 1. 获取传感器数据
    float voltage = battery_lib.voltage(i);      // 24.34V (来自INA239)
    float current = battery_lib.current_amps(i); // 0.071A (来自INA239)
    
    // 2. 核心计算：电压→电量转换
    float soc = periph.bat_params.calculate_soc_from_voltage(voltage);
    ↓
    // 详细计算过程:
    // - 获取BAT参数: cell_num=6, full_voltage=4200, low_voltage=3500
    // - 计算单体电压: 24.34V ÷ 6 = 4.057V
    // - 7段算法判断: 4.057V在80%-95%段
    // - 线性插值: soc = 80% + (4.057-4.05)/(4.18-4.05) × 15% ≈ 80%
    
    // 3. 构建DroneCAN消息
    uavcan_equipment_power_BatteryInfo pkt;
    pkt.battery_id = 125;                     // 节点ID
    pkt.voltage = 24.34;                      // 总电压
    pkt.current = 0.071;                      // 电流
    pkt.state_of_charge_pct = 80;             // 计算的电量
    pkt.full_charge_capacity_wh = 121.75;     // 容量 (5000mAh×24.34V÷1000)
    
    // 4. CAN消息发送
    canard_broadcast(UAVCAN_EQUIPMENT_POWER_BATTERYINFO_SIGNATURE,
                    UAVCAN_EQUIPMENT_POWER_BATTERYINFO_ID,
                    &pkt, sizeof(pkt));
}
```

### 3. 通信链路
```cpp
// === CAN发送链路 ===
DroneCAN消息 → CAN1发送缓冲区 → STM32L431 CAN1外设 → PB9引脚 → CAN总线
     ↓              ↓                 ↓              ↓        ↓
BatteryInfo → canard_broadcast → CAN1_TX → 物理信号 → 主飞控接收

// === 主飞控接收处理 ===
主飞控CAN接收 → DroneCAN解析 → AP_BattMonitor_DroneCAN → battery_status消息
      ↓              ↓               ↓                    ↓
   CAN总线 → BatteryInfo消息 → 电池状态更新 → 地面站显示
```

---

## 代码执行时序

### 启动时序 (前5秒)
```
时间    事件                           代码位置
0ms     系统复位                       硬件
10ms    HAL初始化                      hal.init()
50ms    参数系统初始化                  AP_Param::setup_sketch_defaults()
100ms   CAN总线启动                    can_start()
200ms   电池监控初始化                  battery_lib.init()
300ms   INA239传感器检测               AP_BattMonitor_INA239::init()
500ms   开始周期性数据采集              timer注册成功
1000ms  首次电池数据发送               can_battery_update()
```

### 运行时序 (稳定运行)
```
每25ms:  INA239数据读取               AP_BattMonitor_INA239::timer()
         ├─ 读取电压寄存器 (5ms)
         ├─ 读取电流寄存器 (5ms)  
         └─ 数据累积处理 (1ms)

每100ms: 电池状态更新                 can_battery_update()
         ├─ 获取传感器数据 (1ms)
         ├─ BAT参数电量计算 (2ms)
         ├─ DroneCAN消息构建 (2ms)
         └─ CAN总线发送 (5ms)

每1ms:   主循环调度                   update()
         ├─ CAN消息处理
         ├─ 系统维护
         └─ 任务调度
```

### 关键时序点
```cpp
// === INA239数据读取时序 ===
void AP_BattMonitor_INA239::timer(void) {
    // 执行频率: 40Hz (25ms)
    // 执行时间: ~10ms (SPI通信时间)
    // 优先级: 高 (定时器中断)
}

// === 电量计算时序 ===  
void AP_Periph_FW::can_battery_update(void) {
    // 执行频率: 10Hz (100ms)
    // 执行时间: ~15ms (计算+CAN发送)
    // 优先级: 中 (主循环调用)
}

// === CAN消息发送时序 ===
canard_broadcast() {
    // 执行时间: ~5ms (取决于CAN总线负载)
    // 消息大小: ~32字节
    // 优先级: 低 (CANARD_TRANSFER_PRIORITY_LOW)
}
```

---

## 数据流向图

### 完整数据链路
```
[物理层]
INA239传感器 ←→ SPI总线 ←→ STM32L431
   ↓ 25ms          ↓          ↓
电压/电流数据 → SPI寄存器 → 中断处理

[驱动层]  
AP_BattMonitor_INA239::timer()
   ↓ 数据转换
24.34V / 0.071A

[应用层]
can_battery_update() → BAT参数计算 → calculate_soc_from_voltage()
   ↓ 100ms              ↓              ↓
电压数据获取 → 4.057V单体电压 → 80%电量

[通信层]
DroneCAN消息构建 → CAN总线发送 → 主飞控接收
        ↓              ↓           ↓
   BatteryInfo → 节点125发送 → battery_status

[显示层]
地面站界面显示
   ↓
电池125: 80%电量, 24.34V, 0.071A
```

### 关键决策点
```cpp
// 1. 电量计算方式选择
if (voltage > 0) {
    // 优先使用电压计算 (我们的创新)
    soc = bat_params.calculate_soc_from_voltage(voltage);
} else {
    // 后备方案：电流累积
    battery_lib.capacity_remaining_pct(percentage, i);
}

// 2. 参数来源选择
#ifndef AP_PERIPH_BAT_CELL_NUM_DEFAULT
    // 如果hwdef.dat未定义，使用代码默认值
    #define AP_PERIPH_BAT_CELL_NUM_DEFAULT 6
#endif

// 3. 功能启用选择
#if AP_PERIPH_BATTERY_ENABLED
    // 只有在hwdef.dat中启用时才编译电池功能
    can_battery_update();
#endif
```

---

## 总结

SKY_PMU_80的编译和执行遵循ArduPilot的标准架构，但通过巧妙的自定义参数系统和电量计算算法创新，实现了：

1. **编译时配置**: hwdef.dat定义硬件和默认值
2. **运行时参数**: BAT参数组提供用户配置接口  
3. **实时计算**: 基于电压的精确电量算法
4. **标准通信**: 兼容DroneCAN协议的数据传输

整个系统形成了从硬件定义到用户界面的完整闭环，实现了高精度、易配置的电池管理功能。
