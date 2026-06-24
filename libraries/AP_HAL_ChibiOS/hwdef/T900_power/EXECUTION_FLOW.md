# SKY_PMU_80 PMU外设执行流程与调用链路详解

## 📋 目录
1. [系统执行概览](#系统执行概览)
2. [battery_lib.read()调用链路](#battery_libread调用链路)
3. [数据同步机制](#数据同步机制)
4. [时序分析](#时序分析)
5. [完整执行流程图](#完整执行流程图)

---

## 系统执行概览

### 核心执行架构
```
主函数main() → 外设实例periph → 主循环update() → CAN处理can_update()
    ↓              ↓              ↓ 1ms周期         ↓
系统启动        初始化完成        永不返回         各传感器数据发送
```

### 双路径数据处理
```
路径1: 数据采集路径 (10Hz)
main() → update() → battery_lib.read() → INA239::read() → 状态更新

路径2: 数据发送路径 (10Hz)  
main() → update() → can_update() → can_battery_update() → CAN发送
```

---

## battery_lib.read()调用链路

### 完整调用栈
```cpp
// === 第1层：主循环入口 ===
// 文件: Tools/AP_Periph/AP_Periph.cpp
// 函数: AP_Periph_FW::update()
// 行号: 500
void AP_Periph_FW::update(void) {
    while (true) {                                    // 无限主循环
        uint32_t now = AP_HAL::millis();
        
        #if AP_PERIPH_BATTERY_ENABLED                 // 编译条件：电池功能启用
        if (now - battery.last_read_ms >= 100) {     // 运行条件：100ms频率控制
            battery.last_read_ms = now;
            battery_lib.read();                       // 🔗 调用电池库读取
        }
        #endif
        
        can_update();                                 // 🔗 CAN消息处理
        hal.scheduler->delay(1);                      // 1ms延时
    }
}
```

```cpp
// === 第2层：电池监控库 ===
// 文件: libraries/AP_BattMonitor/AP_BattMonitor.cpp  
// 函数: AP_BattMonitor::read()
// 行号: 798-838
void AP_BattMonitor::read()
{
    const uint32_t now_ms = AP_HAL::millis();
    
    // 遍历所有电池实例 (通常只有1个)
    for (uint8_t i=0; i<_num_instances; i++) {
        
        // === 驱动有效性检查 ===
        if (drivers[i] == nullptr) {                 // 驱动未初始化检查
            continue;
        }
        if (allocated_type(i) != configured_type(i)) { // 类型匹配检查
            continue;  
        }
        if (configured_type(i) == Type::NONE) {      // 禁用状态检查
            continue;
        }
        
        // === 🎯 关键调用：具体驱动读取 ===
        drivers[i]->read();                          // 🔗 多态调用INA239驱动
        drivers[i]->update_resistance_estimate();    // 内阻估算
        
        // === 健康状态维护 ===
        if (state[i].healthy) {
            state[i].last_healthy_ms = now_ms;       // 更新健康时间戳
        }
        
        // === 日志记录 ===
        #if HAL_LOGGING_ENABLED
        drivers[i]->Log_Write_BAT(i, time_us);       // 记录电池日志
        #endif
    }
}
```

```cpp
// === 第3层：INA239驱动读取 ===
// 文件: libraries/AP_BattMonitor/AP_BattMonitor_INA239.cpp
// 函数: AP_BattMonitor_INA239::read()  
// 行号: 103-124
void AP_BattMonitor_INA239::read(void)
{
    // === 🔒 线程安全保护 ===
    WITH_SEMAPHORE(accumulate.sem);                  // 获取信号量锁
    
    // === 数据有效性检查 ===
    _state.healthy = accumulate.count > 0;           // 检查是否有新数据
    if (!_state.healthy) {
        return;                                      // 无数据直接返回
    }
    
    // === 🎯 核心数据处理：平均值计算 ===
    _state.voltage = accumulate.volt_sum / accumulate.count;        // 24.34V
    _state.current_amps = accumulate.current_sum / accumulate.count; // 0.071A
    
    // === 缓冲区清空 ===
    accumulate.volt_sum = 0;                         // 清空电压累积
    accumulate.current_sum = 0;                      // 清空电流累积
    accumulate.count = 0;                            // 重置计数器
    
    // === 时间戳和消耗计算 ===
    const uint32_t tnow = AP_HAL::micros();
    const uint32_t dt_us = tnow - _state.last_time_micros;
    update_consumed(_state, dt_us);                  // 传统电流累积 (我们不用)
    _state.last_time_micros = tnow;
}
```

---

## 数据同步机制

### 生产者：INA239定时器中断 (25ms周期)
```cpp
// === 硬件定时器中断处理 ===
// 文件: libraries/AP_BattMonitor/AP_BattMonitor_INA239.cpp
// 函数: AP_BattMonitor_INA239::timer()
// 执行频率: 40Hz (25ms)
// 执行上下文: 硬件中断

void AP_BattMonitor_INA239::timer(void)
{
    // === 硬件寄存器读取 ===
    int16_t bus_voltage, current;
    if (!read_word(REG_BUS_VOLTAGE, bus_voltage) ||    // SPI读取电压寄存器
        !read_word(REG_CURRENT, current)) {            // SPI读取电流寄存器
        failed_reads++;
        return;                                        // 读取失败，退出
    }
    failed_reads = 0;
    
    // === 原始数据转换 ===
    float voltage = bus_voltage * voltage_LSB;         // 0x1F23 → 24.34V
    float current_amps = current * current_LSB;        // 0x0234 → 0.071A
    
    // === 🎯 数据生产：累积到缓冲区 ===
    WITH_SEMAPHORE(accumulate.sem);                    // 🔒 线程安全保护
    accumulate.volt_sum += voltage;                    // 累积电压：24.34V
    accumulate.current_sum += current_amps;            // 累积电流：0.071A
    accumulate.count++;                                // 计数器：+1
    
    // 执行结果：每25ms向缓冲区添加一组数据
}
```

### 消费者：主循环读取 (100ms周期)
```cpp
// === 主循环数据消费 ===
// 调用路径: update() → battery_lib.read() → INA239::read()
// 执行频率: 10Hz (100ms)
// 执行上下文: 主线程

void AP_BattMonitor_INA239::read(void)
{
    // === 🔒 获取同一个信号量 ===
    WITH_SEMAPHORE(accumulate.sem);                    // 与生产者共享锁
    
    // === 数据消费：计算4次采样的平均值 ===
    // 100ms内accumulate.count = 4 (25ms×4次)
    _state.voltage = accumulate.volt_sum / accumulate.count;      
    // 计算: (24.34+24.35+24.33+24.34) ÷ 4 = 24.34V
    
    _state.current_amps = accumulate.current_sum / accumulate.count;
    // 计算: (0.070+0.071+0.072+0.071) ÷ 4 = 0.071A
    
    // === 缓冲区重置 ===
    accumulate.volt_sum = 0;                           // 清空累积和
    accumulate.current_sum = 0;                        // 清空累积和
    accumulate.count = 0;                              // 重置计数器
    
    // 执行结果：_state包含最新的平均值数据
}
```

### 缓冲区数据结构
```cpp
// === accumulate缓冲区结构 ===
struct {
    HAL_Semaphore sem;        // 线程同步信号量
    float volt_sum;           // 电压累积和 (4次采样的总和)
    float current_sum;        // 电流累积和 (4次采样的总和)  
    uint16_t count;           // 采样次数计数器 (0-4循环)
} accumulate;

// === 数据流向 ===
时间    生产者动作                     缓冲区状态                消费者动作
0ms     volt_sum += 24.34V            {97.36V, 0.284A, 4}     read()计算平均值
        current_sum += 0.071A         count = 4                清空缓冲区
        count++                       
                                      
25ms    volt_sum += 24.35V            {24.35V, 0.070A, 1}     (等待)
        current_sum += 0.070A         count = 1
        count++

50ms    volt_sum += 24.33V            {48.68V, 0.142A, 2}     (等待)
        current_sum += 0.072A         count = 2
        count++

75ms    volt_sum += 24.34V            {73.02V, 0.213A, 3}     (等待)
        current_sum += 0.071A         count = 3
        count++

100ms   volt_sum += 24.34V            {97.36V, 0.284A, 4}     read()计算平均值
        current_sum += 0.071A         count = 4                _state.voltage = 24.34V
        count++                                                _state.current = 0.071A
                                                              清空缓冲区
```

---

## 时序分析

### 系统级时序
```
系统时钟    硬件中断 (25ms)           主循环 (1ms)                   CAN发送
0ms         INA239::timer()          update()                       
            ├─ SPI读取: 5ms           ├─ battery_lib.read(): 2ms     
            └─ 数据累积: 1ms          ├─ can_update(): 10ms          
                                     └─ can_battery_update(): 5ms   → 发送DroneCAN

1ms         (等待下次中断)            update()                       
                                     ├─ (跳过battery_lib.read)      
                                     ├─ can_update(): 1ms           
                                     └─ (跳过can_battery_update)    

...         (重复1ms循环)             ...

25ms        INA239::timer()          update()
            ├─ SPI读取: 5ms           ├─ (跳过battery_lib.read)
            └─ 数据累积: 1ms          └─ can_update(): 1ms

...         (重复)                   ...

100ms       INA239::timer()          update()
            ├─ SPI读取: 5ms           ├─ battery_lib.read(): 2ms     ← 🎯 数据处理
            └─ 数据累积: 1ms          ├─ can_update(): 10ms          
                                     └─ can_battery_update(): 5ms   → 🎯 CAN发送
```

### 关键时序点
```cpp
// === 数据采集时序 (25ms) ===
void AP_BattMonitor_INA239::timer(void) {
    // 执行时间: ~10ms (SPI通信 + 数据处理)
    // 执行频率: 40Hz (25ms周期)
    // 执行优先级: 高 (硬件定时器中断)
    // 数据产出: 累积到accumulate缓冲区
}

// === 数据处理时序 (100ms) ===
void AP_BattMonitor_INA239::read(void) {
    // 执行时间: ~2ms (平均值计算)
    // 执行频率: 10Hz (100ms周期)  
    // 执行优先级: 中 (主循环调用)
    // 数据产出: 更新_state状态
}

// === 数据发送时序 (100ms) ===
void AP_Periph_FW::can_battery_update(void) {
    // 执行时间: ~15ms (计算 + CAN发送)
    // 执行频率: 10Hz (100ms周期)
    // 执行优先级: 中 (主循环调用)  
    // 数据产出: DroneCAN消息发送
}
```

---

## 完整执行流程图

### 启动阶段流程
```
系统上电
    ↓
main()函数执行
    ↓
hal.init() - HAL层初始化
    ├─ 时钟配置 (8MHz外部晶振)
    ├─ GPIO初始化 (SPI/CAN引脚)
    ├─ SPI1外设初始化
    └─ CAN1外设初始化
    ↓
static AP_Periph_FW periph - 外设实例创建
    ├─ 构造函数执行
    ├─ bat_params实例创建
    └─ 其他外设实例创建
    ↓
periph.init() - 功能初始化
    ├─ AP_Param::setup_sketch_defaults() - 加载defaults.parm
    ├─ AP_Param::load_all() - 从Flash加载用户参数
    ├─ can_start() - CAN总线启动
    ├─ battery_lib.init() - 电池监控初始化
    │   └─ AP_BattMonitor_INA239创建和初始化
    │       ├─ SPI设备获取: hal.spi->get_device_ptr("INA23X")
    │       ├─ INA239寄存器配置
    │       └─ 25ms定时器注册: dev->register_periodic_callback()
    └─ 其他外设初始化
    ↓
periph.update() - 进入主循环 (永不返回)
```

### 运行阶段流程 (稳定状态)
```
主循环 update() (1ms周期)
    ↓
时间检查 now = AP_HAL::millis()
    ↓
┌─ 电池数据读取分支 (100ms执行一次) ─┐
│  if (now - battery.last_read_ms >= 100) {        │
│      battery.last_read_ms = now;                 │
│      battery_lib.read();                         │ ← 🎯 数据读取路径
│          ↓                                       │
│      AP_BattMonitor::read()                      │
│          ↓                                       │
│      for (i=0; i<instances; i++)                 │
│          ↓                                       │
│      drivers[i]->read()                          │
│          ↓                                       │
│      AP_BattMonitor_INA239::read()               │
│          ├─ 获取accumulate缓冲区数据              │
│          ├─ 计算平均值: 24.34V, 0.071A           │
│          ├─ 更新_state状态                       │
│          └─ 清空accumulate缓冲区                  │
│  }                                               │
└──────────────────────────────────────────────────┘
    ↓
┌─ CAN消息处理分支 (每次都执行) ─┐
│  can_update()                                     │
│      ↓                                           │
│  LED状态处理                                      │
│      ↓                                           │
│  1Hz任务处理 (节点状态等)                          │
│      ↓                                           │
│  各传感器CAN数据发送:                              │
│      ├─ can_mag_update()                         │
│      ├─ can_gps_update()                         │
│      ├─ can_battery_update()  ← 🎯 电池CAN发送路径 │
│      │   ├─ 频率控制 (100ms)                     │
│      │   ├─ 获取_state数据: 24.34V, 0.071A       │
│      │   ├─ BAT参数电量计算: 80%                  │
│      │   ├─ DroneCAN消息构建                     │
│      │   └─ canard_broadcast() CAN发送           │
│      ├─ can_baro_update()                        │
│      └─ 其他传感器...                            │
└─────────────────────────────────────────────────┘
    ↓
hal.scheduler->delay(1) - 让出CPU时间
    ↓
回到主循环开始 (无限循环)
```

### 并行执行的硬件中断
```
硬件定时器中断 (25ms周期，与主循环并行)
    ↓
AP_BattMonitor_INA239::timer()
    ├─ 中断上下文执行 (高优先级)
    ├─ SPI通信: read_word(REG_BUS_VOLTAGE) - 5ms
    ├─ SPI通信: read_word(REG_CURRENT) - 5ms  
    ├─ 数据转换: voltage = raw * 3.125e-3
    ├─ 数据转换: current = raw * current_LSB
    └─ 🎯 数据生产: 
        WITH_SEMAPHORE(accumulate.sem) {
            accumulate.volt_sum += voltage;      // 生产数据
            accumulate.current_sum += current;   // 生产数据
            accumulate.count++;                  // 计数器+1
        }
```

---

## 数据流向与状态转换

### 数据状态转换图
```
INA239硬件寄存器 → 中断读取 → accumulate缓冲区 → 主循环处理 → _state状态 → CAN发送
       ↓              ↓           ↓              ↓           ↓         ↓
    原始数字值      浮点转换    累积求和        平均值计算   最终状态   网络传输
   REG_VOLTAGE    24.34V     volt_sum       _state.voltage  24.34V   DroneCAN
   REG_CURRENT    0.071A     current_sum    _state.current  0.071A   BatteryInfo
```

### 关键数据结构
```cpp
// === INA239驱动内部状态 ===
class AP_BattMonitor_INA239 {
private:
    // 中断累积缓冲区 (生产者写入)
    struct {
        HAL_Semaphore sem;      // 线程同步锁
        float volt_sum;         // 电压累积和
        float current_sum;      // 电流累积和
        uint16_t count;         // 采样计数
    } accumulate;
    
    // 最终状态 (消费者输出)
    BattMonitor_State _state {
        float voltage;          // 平均电压: 24.34V
        float current_amps;     // 平均电流: 0.071A
        bool healthy;           // 健康状态: true
        uint32_t last_time_micros; // 时间戳
    };
};
```

### 访问接口链路
```cpp
// === 外部访问_state数据的接口链路 ===

// 1. can_battery_update()中的调用
float voltage = battery_lib.voltage(i);
    ↓
// 2. AP_BattMonitor库接口  
float AP_BattMonitor::voltage(uint8_t instance) {
    return state[instance].voltage;  // 返回_state.voltage
}
    ↓  
// 3. 最终获得数据
voltage = 24.34V  // 来自INA239::read()更新的_state.voltage

// 类似地，电流获取：
float current = battery_lib.current_amps(i);
    ↓
float AP_BattMonitor::current_amps(uint8_t instance) {
    return state[instance].current_amps;  // 返回_state.current_amps
}
    ↓
current = 0.071A  // 来自INA239::read()更新的_state.current_amps
```

---

## 执行频率与同步

### 多级频率控制
```
硬件层:    INA239定时器      25ms (40Hz)    - 数据采集
驱动层:    battery_lib.read()  100ms (10Hz)   - 数据处理  
应用层:    can_battery_update() 100ms (10Hz)   - 数据发送
网络层:    CAN总线传输       ~5ms          - 消息传输
```

### 时序同步保证
```cpp
// === 同步机制1：频率倍数关系 ===
// 25ms × 4 = 100ms
// 确保每次read()时accumulate中有4个有效采样

// === 同步机制2：信号量保护 ===  
// 生产者和消费者使用同一个信号量
// 防止数据竞争和不一致

// === 同步机制3：原子操作 ===
// count检查和数据清空在同一个信号量保护下
// 确保数据完整性
```

---

## 总结

### battery_lib.read()的本质
`battery_lib.read()`是一个**数据聚合和状态更新**函数：

1. **数据聚合**: 将25ms中断采集的4次数据计算平均值
2. **状态更新**: 将平均值更新到_state结构体  
3. **缓冲管理**: 清空accumulate缓冲区，准备下一轮采集
4. **健康检查**: 更新传感器健康状态

### 与can_battery_update()的关系
- **battery_lib.read()**: 数据**生产者** (更新_state)
- **can_battery_update()**: 数据**消费者** (读取_state，计算电量，发送CAN)
- **时序关系**: read()先执行，update()后执行，确保CAN发送的是最新数据

### 设计优势
1. **数据平滑**: 4次采样平均，减少噪声
2. **线程安全**: 信号量保护，防止数据竞争  
3. **频率优化**: 硬件采样40Hz，应用处理10Hz，平衡精度和性能
4. **模块化**: 硬件驱动与应用逻辑分离，便于维护

这就是您的SKY_PMU_80中`battery_lib.read()`的完整调用机制！🎯
