# 解决地面站显示磁罗盘类型不正确的问题

## 问题描述

**硬件**: RM3100 (SPI接口)  
**地面站显示**: HMC5883_OLD  
**原因**: 参数中保存了旧的设备ID

## 设备类型对应表

```c
// 来自 AP_Compass_Backend.h
DEVTYPE_HMC5883_OLD = 0x01  // 旧的HMC5883
DEVTYPE_HMC5883     = 0x07  // HMC5883
DEVTYPE_RM3100      = 0x11  // RM3100 ✓ (正确的)
DEVTYPE_RM3100_2    = 0x12  // RM3100备用ID
```

## 解决方法

### 方法1: 重新校准罗盘（推荐）

通过重新校准，系统会自动更新 `COMPASS_DEV_ID` 为正确的值。

#### 使用Mission Planner
1. 连接到飞控（通过CAN总线）
2. 进入 `Setup` → `Mandatory Hardware` → `Compass`
3. 找到SKY_GNSS节点的罗盘
4. 点击 `Start` 开始校准
5. 按提示旋转设备
6. 完成后，`COMPASS_DEV_ID` 会自动更新

#### 使用DroneCAN GUI Tool
1. 连接到CAN总线
2. 找到SKY_GNSS节点
3. 进入 `Magnetometer` 标签
4. 执行校准流程
5. 保存参数

### 方法2: 手动清除设备ID

如果无法进行校准，可以手动清除旧的设备ID。

#### 步骤：

1. **连接DroneCAN GUI Tool或Mission Planner**

2. **找到以下参数并设置为0**：
   ```
   COMPASS_DEV_ID  = 0
   COMPASS_DEV_ID2 = 0  (如有)
   COMPASS_DEV_ID3 = 0  (如有)
   ```

3. **清除校准数据**（可选）：
   ```
   COMPASS_OFS_X = 0
   COMPASS_OFS_Y = 0
   COMPASS_OFS_Z = 0
   COMPASS_DIA_X = 1
   COMPASS_DIA_Y = 1
   COMPASS_DIA_Z = 1
   COMPASS_ODI_X = 0
   COMPASS_ODI_Y = 0
   COMPASS_ODI_Z = 0
   ```

4. **保存参数并重启设备**

5. **重新上电后检查**：
   - 串口日志应该显示：
     ```
     RM3100: Found at address 0x... as compass 0
     ```
   - 新的 `COMPASS_DEV_ID` 应该包含 `0x11` (DEVTYPE_RM3100)

### 方法3: 通过defaults.parm预设（开发阶段）

在固件编译时预设正确的参数。

编辑 `defaults.parm`:
```bash
# 强制使用RM3100（仅在开发调试时使用）
# COMPASS_DEV_ID 的值需要根据实际总线ID计算
# 格式: (DEVTYPE << 24) | (BUS_TYPE << 16) | (BUS << 8) | ADDR

# 对于 SPI1 的 RM3100:
# DEVTYPE = 0x11 (RM3100)
# BUS_TYPE = 0x03 (SPI)
# BUS = 0x01 (SPI1)
# ADDR = 0x00 (片选0)
# 
# COMPASS_DEV_ID = 0x11030100
# 
# ⚠️ 这个值会随着硬件配置变化，不推荐固定！
# 最好让系统自动检测和设置
```

## 验证设备ID是否正确

### 通过串口日志验证

连接USART1 (PB6=TX, PB7=RX)，波特率57600：

```bash
# 应该看到
RM3100: Found at address 0x0 as compass 0

# 而不是
HMC5883: Found at address ...
```

### 通过参数验证

查看 `COMPASS_DEV_ID` 参数：

```python
# 获取参数值（十六进制）
COMPASS_DEV_ID = 0x????????

# 提取设备类型（高8位）
DEVTYPE = (COMPASS_DEV_ID >> 24) & 0xFF

# 检查DEVTYPE
如果 DEVTYPE == 0x11:  # RM3100
    print("✓ 正确的设备类型")
elif DEVTYPE == 0x01:  # HMC5883_OLD
    print("✗ 旧的设备ID，需要清除")
```

### 通过地面站验证

**Mission Planner**:
- 进入 `Setup` → `Optional Hardware` → `Compass`
- 查看罗盘类型显示
- 应该显示 "RM3100" 而不是 "HMC5883"

**DroneCAN GUI Tool**:
- 连接到CAN总线
- 查看 `uavcan.equipment.ahrs.MagneticFieldStrength` 消息
- 检查磁场数据是否正常

## 为什么会显示错误的设备类型？

### 原因1: 参数持久化
```
旧固件/设备 → 保存了 HMC5883 的 DEV_ID
更换为RM3100 → 参数仍然保存旧值
地面站读取 → 显示为 HMC5883_OLD
```

### 原因2: 校准数据关联
```
COMPASS_DEV_ID 与校准数据绑定
如果DEV_ID不匹配 → 使用旧的校准数据
地面站显示 → 显示旧设备类型
```

### 原因3: 设备ID格式

设备ID是一个32位值，包含：
```c
[31:24] = DevType (0x11 for RM3100)
[23:16] = BusType (0x03 for SPI)
[15:8]  = Bus Number (0x01 for SPI1)
[7:0]   = Bus Address (0x00 for chip select 0)

示例: 0x11030100
  ↑      ↑  ↑  ↑
  |      |  |  └─ 片选0
  |      |  └──── SPI1总线
  |      └─────── SPI类型
  └────────────── RM3100设备类型
```

## 完整排查流程

### 步骤1: 检查硬件配置

```bash
# 查看 hwdef.dat
COMPASS RM3100 SPI:rm3100 false ROTATION_PITCH_180

# 确认SPI配置
SPIDEV rm3100 SPI1 DEVID1 MAG_CS MODE0 1*MHZ 1*MHZ
```

### 步骤2: 查看启动日志

```bash
# 连接串口，查看启动信息
RM3100: Found at address 0x0 as compass 0

# 如果看到其他类型，说明硬件配置错误
```

### 步骤3: 检查参数

```bash
# 使用DroneCAN GUI Tool或Mission Planner
COMPASS_DEV_ID = ?

# 计算设备类型
DevType = (COMPASS_DEV_ID >> 24) & 0xFF
```

### 步骤4: 清除并重新初始化

```bash
# 设置为0让系统重新检测
COMPASS_DEV_ID = 0

# 重启设备
# 系统会自动设置正确的DEV_ID
```

### 步骤5: 重新校准

```bash
# 进行罗盘校准
# 校准过程会更新所有相关参数
```

### 步骤6: 验证

```bash
# 检查参数
COMPASS_DEV_ID = 0x11?????? (高字节应该是0x11)

# 检查地面站显示
# 应该显示 "RM3100"
```

## 常见问题

### Q1: 清除COMPASS_DEV_ID后设备不工作？
**A**: 正常现象，重启设备后会自动重新检测并设置。

### Q2: 重新校准后还是显示HMC5883？
**A**: 
1. 检查固件是否正确编译（包含RM3100配置）
2. 检查串口日志确认硬件检测
3. 尝试参数复位后重新配置

### Q3: COMPASS_DEV_ID一直是0？
**A**: 
1. 检查SPI连接是否正常
2. 检查RM3100是否正确焊接
3. 查看启动日志是否有错误信息

### Q4: 地面站不显示罗盘数据？
**A**: 
1. 确认 `COMPASS_USE` = 1
2. 确认 `COMPASS_ENABLE` = 1
3. 检查CAN总线是否正常通信

### Q5: 如何确认当前使用的罗盘类型？
**A**: 
```bash
# 方法1: 串口日志
RM3100: Found at address ...

# 方法2: 参数
COMPASS_DEV_ID 高字节 = 0x11

# 方法3: 地面站传感器状态
Mission Planner → Status → Sensors
```

## 参数参考

### 主要参数
```
COMPASS_USE      = 1     # 启用罗盘
COMPASS_ENABLE   = 1     # 启用罗盘
COMPASS_DEV_ID   = 自动  # 设备ID（自动检测）
COMPASS_ORIENT   = 0     # 旋转方向（由hwdef.dat设置）
```

### 校准参数
```
COMPASS_OFS_X    = xxx   # X轴偏移
COMPASS_OFS_Y    = xxx   # Y轴偏移
COMPASS_OFS_Z    = xxx   # Z轴偏移
COMPASS_DIA_X    = xxx   # X轴对角线元素
COMPASS_DIA_Y    = xxx   # Y轴对角线元素
COMPASS_DIA_Z    = xxx   # Z轴对角线元素
COMPASS_ODI_X    = xxx   # X轴非对角线偏移
COMPASS_ODI_Y    = xxx   # Y轴非对角线偏移
COMPASS_ODI_Z    = xxx   # Z轴非对角线偏移
```

### 高级参数
```
COMPASS_EXTERNAL = 0     # 0=内部, 1=外部
COMPASS_AUTODEC  = 1     # 自动磁偏角
COMPASS_MOTCT    = 0     # 电机补偿类型
```

## 推荐流程（首次使用）

1. **烧录固件** （包含正确的hwdef.dat配置）

2. **上电检查**
   - 连接串口查看启动日志
   - 确认看到 "RM3100: Found..."

3. **参数初始化**
   - 如果是新设备，COMPASS_DEV_ID自动设置
   - 如果是从旧设备升级，清除COMPASS_DEV_ID

4. **罗盘校准**
   - 使用Mission Planner或DroneCAN GUI Tool
   - 完整的360度旋转校准

5. **验证**
   - 地面站检查罗盘类型显示
   - 检查磁场数据是否合理（200-600mGauss）

## 关键发现：为什么每次ID都在变化？

### 问题根源分析

**现象**: COMPASS_DEV_ID 每次重启都在变化  
**显示**: 地面站一直显示 HMC5883_OLD  

**原因**:

```c
// 来自 AP_Compass_RM3100.cpp line 147-151
dev->set_device_type(DEVTYPE_RM3100);  // 设置设备类型为 0x11
if (!register_compass(dev->get_bus_id(), compass_instance)) {
    return false;
}
set_dev_id(compass_instance, dev->get_bus_id());
```

问题在于：**`dev->get_bus_id()` 返回的ID可能不稳定！**

### 为什么地面站显示HMC5883_OLD？

**重要发现**: AP_Periph通过CAN发送的罗盘消息**不包含设备类型信息**！

```c
// Tools/AP_Periph/compass.cpp
// CAN消息只包含磁场强度数据，没有设备类型
uavcan_equipment_ahrs_MagneticFieldStrength pkt {};
for (uint8_t i=0; i<3; i++) {
    pkt.magnetic_field_ga[i] = field_Ga[i];  // 只有3个float值
}
// ❌ 没有设备类型字段！
```

**地面站如何知道罗盘类型？**

1. ❌ **不是从CAN消息获取** - CAN消息没有设备类型信息
2. ✅ **从参数COMPASS_DEV_ID读取** - 地面站解析DEV_ID的高8位

### 解码COMPASS_DEV_ID

```python
# COMPASS_DEV_ID是一个32位值
# 格式：[DevType][BusType][Bus][Address]
#       [31:24] [23:16]  [15:8] [7:0]

# 示例1: RM3100 在 SPI1
COMPASS_DEV_ID = 0x11030100
#                 ^^          = 0x11 = DEVTYPE_RM3100 ✓
#                   ^^        = 0x03 = BUS_TYPE_SPI
#                     ^^      = 0x01 = SPI1
#                       ^^    = 0x00 = CS0

# 示例2: HMC5883_OLD
COMPASS_DEV_ID = 0x01??????
#                 ^^          = 0x01 = DEVTYPE_HMC5883_OLD ✗
```

**问题**: 如果DEV_ID高字节不是`0x11`，地面站就会显示错误的类型！

### 为什么DEV_ID会变化？

可能的原因：

#### 1. **参数未保存**
```c
// AP_Periph在某些情况下不保存参数
// 导致每次重启都重新生成DEV_ID
set_dev_id(compass_instance, dev->get_bus_id());
// 这个调用可能只set()不save()
```

#### 2. **SPI设备探测顺序变化**
```c
// 如果SPI设备探测顺序不固定
// get_bus_id() 返回值可能变化
// 导致生成不同的DEV_ID
```

#### 3. **设备初始化失败**
```c
// RM3100初始化失败后重新初始化
// 可能被分配不同的instance ID
// 导致DEV_ID变化
```

## ✅ 正确的解决方案

### 方案A: 强制保存正确的DEV_ID（推荐）

通过defaults.parm预设正确的DEV_ID：

编辑 `/home/gbb/ardupilot/libraries/AP_HAL_ChibiOS/hwdef/SKY_GNSS/defaults.parm`:

```bash
# ============================================================================
# Neopixel LED 配置
# ============================================================================
OUT1_FUNCTION 120
NTF_LED_BRIGHT 2
NTF_LED_LEN 8

# ============================================================================
# 罗盘设备ID配置（强制RM3100）
# ============================================================================
# 对于 RM3100 在 SPI1:
# DevType = 0x11 (RM3100)
# BusType = 0x03 (SPI)
# Bus = 0x01 (SPI1)
# DevID = 1 (第一个设备)
# 
# 完整计算:
# (0x11 << 24) | (0x03 << 16) | (0x01 << 8) | 0x01
# = 0x11030101 = 285282561

COMPASS_DEV_ID 285282561

# 或者让系统自动检测
# COMPASS_DEV_ID 0

# 启用罗盘
COMPASS_USE 1
COMPASS_ENABLE 1
```

### 方案B: 修改代码确保DEV_ID保存

在 `hwdef.dat` 中添加配置，确保参数持久化：

```c
# 确保参数保存
define AP_PARAM_KEY_DUMP 0
define HAL_COMPASS_DEFAULT 1
```

### 方案C: 禁用设备类型检查（临时方案）

如果地面站只是显示问题，实际功能正常，可以暂时忽略显示类型。

**验证罗盘是否真正工作**:
- 检查磁场数据是否合理（200-600 mGauss）
- 检查方位角是否正确
- 检查罗盘是否跟随设备旋转

## 🔍 调试步骤

### 步骤1: 查看串口日志

```bash
# 连接USART1，波特率57600
# 查看启动信息：

RM3100: Found at address 0x0 as compass 0
# 记录这个地址

# 如果看到：
RM3100: Found at address 0x1 as compass 0
# 地址变化说明设备探测不稳定
```

### 步骤2: 监控COMPASS_DEV_ID变化

```python
# 通过DroneCAN GUI Tool监控参数

# 第1次启动
COMPASS_DEV_ID = 0x11030100  # bus_id=0

# 第2次启动
COMPASS_DEV_ID = 0x11030101  # bus_id=1  ⚠️ 变化了！

# 第3次启动
COMPASS_DEV_ID = 0x11030102  # bus_id=2  ⚠️ 又变了！
```

### 步骤3: 检查设备类型字节

```python
# 提取DevType（高8位）
DevType = (COMPASS_DEV_ID >> 24) & 0xFF

if DevType == 0x11:
    print("✓ 设备类型正确 (RM3100)")
elif DevType == 0x01:
    print("✗ 设备类型错误 (HMC5883_OLD)")
elif DevType == 0x00:
    print("⚠️ 设备未初始化")
else:
    print(f"? 未知设备类型: 0x{DevType:02X}")
```

### 步骤4: 验证CAN消息

```bash
# 使用DroneCAN GUI Tool
# 监控消息: uavcan.equipment.ahrs.MagneticFieldStrength

# 正常数据应该是：
magnetic_field_ga[0] = 0.2~0.6  (X轴)
magnetic_field_ga[1] = 0.2~0.6  (Y轴)
magnetic_field_ga[2] = 0.2~0.6  (Z轴)

# ❌ 注意：这个消息不包含设备类型信息！
# 地面站只能从COMPASS_DEV_ID参数判断类型
```

## 🎯 终极解决方案

### 方法1: 使用defaults.parm固定DEV_ID（最简单）

```bash
# 编辑 defaults.parm
COMPASS_DEV_ID 285282561

# 或者使用十六进制（需要转换）
# 0x11030101 = 285282561
```

**优点**:
- ✅ 简单直接
- ✅ 每次刷固件自动设置
- ✅ 不会变化

**缺点**:
- ⚠️ 硬编码，硬件变化需要修改

### 方法2: 让地面站忽略显示类型

实际上，只要罗盘数据正确，显示什么类型并不重要。

**验证方法**:
```bash
1. 检查磁场强度是否合理
2. 检查方位角是否正确旋转
3. 校准后精度是否满足要求

如果以上都正常，显示类型不对不影响使用
```

### 方法3: 修改AP_Periph发送设备类型（需要代码修改）

这需要修改`Tools/AP_Periph/compass.cpp`，在CAN消息中添加设备类型字段。

**不推荐**，因为：
- 需要修改标准DroneCAN消息格式
- 可能不兼容现有地面站

## 总结

**最简单的解决方法**:
```bash
1. 编辑 defaults.parm
2. 添加 COMPASS_DEV_ID 285282561
3. 重新编译固件
4. 刷写固件
5. 完成！
```

**或者接受现状**:
```bash
# 如果罗盘数据正常工作
# 显示类型不对不影响飞行
# 可以不用修复
```

这样可以确保系统使用正确的RM3100设备类型和ID，地面站也会显示正确的罗盘类型。

