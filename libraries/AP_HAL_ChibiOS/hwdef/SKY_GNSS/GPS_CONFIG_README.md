# SKY_GNSS M9N GPS 配置说明

## 硬件连接

```
M9N GPS模块 → STM32F412
TX  → PA3 (USART2_RX)
RX  → PA2 (USART2_TX)
VCC → 5V或3.3V
GND → GND
```

## 当前配置状态

### hwdef.dat 配置
```c
// Serial3 = USART2 用于GPS
SERIAL_ORDER USART1 EMPTY EMPTY USART2

// GPS端口默认值
define HAL_PERIPH_GPS_PORT_DEFAULT 3  // Serial3

// GPS优化设置
define GPS_MAX_RATE_MS 200            // 5Hz更新率
define GPS_MAX_RECEIVERS 1            // 单GPS
define GPS_MAX_INSTANCES 1            // 单实例
```

### defaults.parm 配置
默认参数会被编译到固件中，但可以通过地面站或DroneCAN GUI Tool修改。

## M9N GPS 特性

### 支持的GNSS系统
- GPS (美国)
- GLONASS (俄罗斯)
- Galileo (欧洲)
- BeiDou (中国北斗)
- QZSS (日本)
- SBAS (增强系统)

### M9N 重要特性
1. **更新率**: 推荐5Hz (200ms)，最高可到10Hz但会影响性能
2. **波特率**: 默认9600，ArduPilot会自动切换到230400
3. **搜星时间**: 
   - 冷启动: 26秒
   - 温启动: 3秒
   - 热启动: 1秒

## 参数说明

### 可通过地面站设置的参数

#### GPS_TYPE (GPS1_TYPE)
- 0 = None (禁用)
- 1 = AUTO (自动检测，**推荐**)
- 2 = uBlox (手动指定为uBlox)

#### GPS_PORT
- 指定GPS连接的串口
- 对于SKY_GNSS: **3** (Serial3 = USART2)

#### GPS_GNSS_MODE (GPS1_GNSS_MODE)
启用的GNSS系统位掩码:
```
Bit 0 (1)   = GPS
Bit 1 (2)   = SBAS
Bit 2 (4)   = Galileo
Bit 3 (8)   = BeiDou
Bit 4 (16)  = IMES
Bit 5 (32)  = QZSS
Bit 6 (64)  = GLONASS

推荐值:
- 79  = GPS+SBAS+Galileo+BeiDou+QZSS+GLONASS (全系统)
- 5   = GPS+Galileo (双系统，功耗更低)
- 0   = 保持GPS模块出厂设置
```

#### GPS_RATE_MS (GPS1_RATE_MS)
- 100 = 10Hz (**不推荐M9N使用，会严重影响性能**)
- 125 = 8Hz
- 200 = 5Hz (**M9N推荐值**)

## 故障排查

### 1. 检查硬件连接
```bash
# 连接USART1调试串口 (PB6/PB7)，波特率57600
# 查看启动日志
```

应该看到类似：
```
AP_Periph GCS Initialised!
GPS: probing for GPS at 9600 baud
GPS: detected at 230400 baud
u-blox M9N 1 HW: 00190000 SW: EXT CORE 1.00
```

### 2. 数据正常但无卫星的原因

#### A. 环境因素
- ✗ 在室内
- ✗ 窗边但被建筑物遮挡
- ✗ GPS天线朝下或被金属屏蔽
- ✗ 附近有强电磁干扰源

✅ **解决方法**: 将设备放到室外空旷处，天线朝上，等待5-10分钟

#### B. GNSS配置问题
如果 `GPS_GNSS_MODE` 设置不当，可能导致搜星困难。

✅ **验证方法**:
```
# 通过DroneCAN GUI Tool查看参数
GPS1_GNSS_MODE = ?

# 推荐设置
GPS1_GNSS_MODE = 79  (启用所有系统)
# 或
GPS1_GNSS_MODE = 0   (使用GPS出厂设置)
```

#### C. GPS端口配置错误
```
# 确认以下参数
GPS_PORT = 3           # 必须是3 (USART2)
SERIAL3_PROTOCOL = 5   # 5 = GPS
```

#### D. GPS_TYPE = 0 (禁用)
```
# 检查GPS是否被禁用
GPS1_TYPE = ?

# 应该是
GPS1_TYPE = 1  (AUTO) 或 2 (uBlox)
```

### 3. 调试信息启用

#### 方法1: 查看CAN总线消息
使用DroneCAN GUI Tool连接到CAN总线，查看:
- `uavcan.equipment.gnss.Fix2` - GPS定位数据
- `uavcan.equipment.gnss.Auxiliary` - 卫星数据

#### 方法2: 查看串口日志
连接USART1 (PB6=TX, PB7=RX)，波特率57600:
```bash
# Linux
screen /dev/ttyUSB0 57600

# Windows
使用PuTTY或串口助手
```

查找关键日志:
```
GPS: detected at 230400 baud       # GPS检测成功
u-blox M9N 1 HW: ...                # GPS型号识别
GPS: status=3, sats=12, hdop=0.8   # 定位状态
```

### 4. 常见状态码

#### GPS Status (Fix Type)
```
0 = NO_GPS      - 无GPS
1 = NO_FIX      - 无定位
2 = GPS_OK_FIX_2D - 2D定位
3 = GPS_OK_FIX_3D - 3D定位 ✓
4 = GPS_OK_FIX_3D_DGPS - DGPS
5 = GPS_OK_FIX_3D_RTK_FLOAT - RTK浮点解
6 = GPS_OK_FIX_3D_RTK_FIXED - RTK固定解
```

#### 卫星数量
```
sats < 4   - 无法定位
sats = 4-6 - 可能定位 (精度低)
sats >= 8  - 良好定位 ✓
sats >= 12 - 优秀定位 ✓✓
```

#### HDOP (水平精度因子)
```
< 1.0  - 理想 ✓✓
< 2.0  - 优秀 ✓
< 5.0  - 良好
< 10.0 - 中等
>= 10.0 - 较差
```

## 参数设置步骤

### 使用DroneCAN GUI Tool

1. 连接到CAN总线
2. 找到SKY_GNSS节点
3. 进入"Parameters"标签
4. 设置以下参数:
   ```
   GPS_PORT = 3
   GPS1_TYPE = 1
   GPS1_GNSS_MODE = 79
   GPS1_RATE_MS = 200
   ```
5. 点击"Write"保存
6. 重启设备

### 使用Mission Planner

1. 连接飞控
2. 进入"Setup" → "Optional Hardware" → "UAVCAN"
3. 找到SKY_GNSS节点
4. 设置参数（同上）
5. 写入并重启

## 性能优化建议

### 室外使用
```
GPS1_GNSS_MODE = 79    # 全系统，最快定位
GPS1_RATE_MS = 200     # 5Hz
```

### 功耗优化
```
GPS1_GNSS_MODE = 5     # 仅GPS+Galileo
GPS1_RATE_MS = 200     # 5Hz
```

### 高精度应用
```
GPS1_GNSS_MODE = 79    # 全系统
GPS1_RATE_MS = 200     # 5Hz (不要更高)
```

## 验证GPS工作正常

### 检查项目
- [ ] 能看到GPS模块版本信息
- [ ] Status从0变为1 (有数据)
- [ ] 室外等待2-5分钟
- [ ] 卫星数 > 4
- [ ] Status变为3 (3D定位)
- [ ] HDOP < 2.0
- [ ] 位置数据在地面站上显示正确

## 常见问题

### Q: 有数据但没有卫星
**A**: 
1. 确认在室外空旷环境
2. 检查GPS_GNSS_MODE设置
3. 等待至少5-10分钟（冷启动）
4. 检查天线连接

### Q: 完全无数据
**A**: 
1. 检查硬件连接（TX/RX是否接反）
2. 检查GPS_PORT=3
3. 检查GPS1_TYPE不为0
4. 查看串口日志确认GPS检测

### Q: 卫星数量不稳定
**A**: 
1. 检查天线位置
2. 远离WiFi路由器、电机等干扰源
3. 确保天线有清晰的天空视野

### Q: 定位后漂移严重
**A**: 
1. 等待HDOP降到<2.0
2. 检查是否启用多GNSS系统
3. 确认GPS固定良好，无松动

## 技术支持

- ArduPilot论坛: https://discuss.ardupilot.org/
- ArduPilot Discord: https://ardupilot.org/discord
- 文档: https://ardupilot.org/copter/docs/common-gps.html

## 版本信息

- 固件: AP_Periph
- MCU: STM32F412Rx
- GPS: u-blox M9N
- CAN协议: DroneCAN (UAVCANv0)

