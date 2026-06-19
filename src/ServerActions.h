// 服务端业务动作接口。
// 串口命令、Web 控制页和实体按钮都通过这些函数触发同一套业务逻辑。
#pragma once

#include <Arduino.h>

// 处理绑定动作，供串口命令和 Web 控制页复用。
// args[0]：目标 deviceId；args[1]：目标裁判位 client1/client2/client3。
// argc：参数数量，必须为 2；否则只打印用法，不修改状态。
void handleBindCommand(const String args[], uint8_t argc);

// 处理解绑动作。
// args[0]：要解绑的裁判位 client1/client2/client3。
// argc：参数数量，必须为 1；成功时清 NVS、清在线状态，并向裁判端发送 UNBIND。
void handleUnbindCommand(const String args[], uint8_t argc);

// 打印当前绑定表、未绑定设备列表和轮次状态到 USB 调试串口。
void handleListCommand();

// 进入下一轮，不启动倒计时。供实体按钮和无参数串口命令使用。
void handleNextRoundCommand();

// 进入下一轮并启动倒计时。
// countdownSeconds：倒计时时长，单位秒；0 表示不显示倒计时。
void handleNextRoundCommand(uint32_t countdownSeconds);

// 重置比赛比分状态：总比分归零、轮号回 1、清空本轮提交和倒计时；不清绑定表和队名。
void handleResetCommand();

// 驱动下一轮/重置后的 STATUS 重复广播队列。
// 非阻塞；每次到发送间隔只给一个裁判发一帧，避免网页 POST 被 LoRa 发送长时间拖住。
void driveStatusBroadcast();
