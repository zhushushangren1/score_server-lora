// 服务端 LoRa 入站协议处理接口。
// 主循环调用该入口即可持续读取并分发 E22 收到的客户端协议帧。
#pragma once

// 处理 LoRa 入站协议帧。
// 从 LoraLink 读取完整文本帧，完成 CRC/字段解析，并分发到 HELLO/HEARTBEAT/SUBMIT/ACK 等业务处理。
// 该函数非阻塞，应在 loop() 中高频调用，避免 E22 UART 缓冲积压。
void handleLoraInput();
