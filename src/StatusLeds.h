// 服务端状态灯模块接口。
// LoRa 模块触发 TX/RX 脉冲，主循环负责周期性更新灯的到期熄灭和工作闪烁。
#pragma once

#include <Arduino.h>

// 初始化服务端 4 个状态灯 GPIO。
// 电源灯常亮；工作灯、TX、RX 初始熄灭。
void setupStatusLeds();

// 触发一次 LoRa 发送指示灯脉冲。
// 由 sendLoraLine 调用，点亮后由 updateStatusLeds 到期熄灭。
void pulseTxLed();

// 触发一次 LoRa 接收指示灯脉冲。
// 由 readLoraFrame 在收到完整帧时调用。
void pulseRxLed();

// 驱动服务端状态灯的非阻塞状态机。
// 工作灯按 1Hz 闪烁；TX/RX 脉冲灯到期自动熄灭。
void updateStatusLeds();
