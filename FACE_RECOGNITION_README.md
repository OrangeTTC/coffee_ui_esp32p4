# 人脸识别功能使用说明

## 功能概述

CoffeeMachine 现已集成人脸识别功能，可以识别最多 3 个用户的人脸，并为识别出的用户自动制作咖啡。

## 使用流程

### 1. 进入摄像头界面
- 点击主界面的第 8 个按钮（右下角）进入摄像头界面

### 2. 启动人脸识别
- 点击摄像头界面左下角的 **"Face ID"** 按钮
- 系统开始实时人脸检测（每 10 帧处理一次以节省性能）

### 3. 首次使用 - 录入新人脸
当检测到**陌生人脸**时（未存储在系统中）：
1. 系统自动弹出人脸命名界面
2. 使用屏幕键盘输入姓名（最多 31 个字符）
3. 点击 **"Save"** 保存，或点击 **"Cancel"** 取消

**注意**：
- 最多可存储 3 个人脸
- 如果存储已满，将替换最早录入的人脸
- 人脸数据存储在 NVS 中，断电不丢失

### 4. 再次使用 - 识别已知人脸
当检测到**已存储的人脸**时：
1. 系统自动识别用户身份
2. 在串口输出：`COFFEE_FOR: 用户名`
3. 自动关闭摄像头，进入咖啡制作流程
4. 显示咖啡制作动画（GIF 5秒 + 完成提示 2秒）

## 串口输出格式

```
COFFEE_FOR: 张三
```

可通过串口监听该输出来获知为谁制作咖啡。

## 技术细节

### 存储方式
- 使用 NVS（非易失性存储）保存人脸数据
- 存储命名空间：`face_storage`
- 存储键：`faces`
- 数据结构：`FaceData[3]`

### 人脸数据结构
```cpp
struct FaceData {
    char name[32];                    // 用户姓名
    float feature[128];               // 人脸特征（预留）
    bool is_used;                     // 是否已使用
};
```

### 性能优化
- 每 10 帧处理一次人脸检测（降低 CPU 负载）
- 使用 ESP-DL 的 HumanFaceDetect 模型
- 摄像头原生分辨率 1280x960，无缩放处理

## 按钮功能说明

### 摄像头界面按钮：
1. **Face ID** - 启动/停止人脸识别
2. **Btn 2** - 预留功能
3. **Btn 3** - 预留功能
4. **Back** - 返回主界面

## 代码文件说明

### 修改的文件：
1. `main/CoffeeMachine.hpp` - 添加人脸识别数据结构和方法声明
2. `main/CoffeeMachine.cpp` - 实现人脸识别逻辑

### 新增功能：
- `loadFacesFromNVS()` - 从 NVS 加载已存储的人脸
- `saveFacesToNVS()` - 保存人脸数据到 NVS
- `recognizeFace()` - 识别人脸（当前为简化版本）
- `saveFaceData()` - 保存新人脸数据
- `showFaceNameScreen()` - 显示人脸命名界面
- `closeFaceNameScreen()` - 关闭人脸命名界面

### 依赖组件：
- `esp_video` - 摄像头驱动
- `app_humanface_detect` - 人脸检测模块（来自 components/apps/camera）
- `nvs_flash` - 非易失性存储
- `esp-dl` - ESP 深度学习库

## 注意事项

1. **内存限制**：最多存储 3 个人脸，超出后替换最旧的
2. **识别简化**：当前版本的 `recognizeFace()` 是简化实现，实际生产环境需要实现特征提取和比对
3. **性能考虑**：人脸检测每 10 帧执行一次，可根据需求调整频率
4. **光照条件**：人脸识别效果受光照条件影响较大，建议在光线充足环境下使用

## 未来改进方向

1. 实现真正的人脸特征提取和比对（使用 ESP-DL 的人脸识别模型）
2. 为每个用户关联不同的咖啡配方
3. 添加人脸管理界面（查看、删除已存储的人脸）
4. 优化识别准确率和速度
5. 添加人脸照片预览功能

## 调试信息

启用人脸识别后，可在串口监控看到以下日志：
```
I (xxxxx) CoffeeMachine: Camera button 1 clicked
I (xxxxx) CoffeeMachine: Activating face recognition mode
I (xxxxx) CoffeeMachine: Face detected!
I (xxxxx) CoffeeMachine: Welcome back, 张三!
COFFEE_FOR: 张三
```
