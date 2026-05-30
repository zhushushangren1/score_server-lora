#pragma once

#include <Arduino.h>

// 初始化服务端 E22 LoRa 透传链路。
// 负责配置 M0/M1/AUX 引脚、把 E22 切到 M0=LOW/M1=LOW 普通透传模式，并打开 Serial1。
void setupLoraLink();

// 从 Serial1 接收缓冲中读取一帧以 '\n' 结尾的协议文本。
// frameText：输出参数；返回 true 时写入一行不含 CR/LF 的完整 CSV+CRC 文本。
// 返回值：true=读到完整帧；false=当前没有完整帧。该函数非阻塞，适合 loop 高频调用。
bool readLoraFrame(String& frameText);

// 发送一行已经组好 CRC、并带末尾换行的协议文本。
// text：通常来自 ScoreProtocol::buildFrame()，会原样写入 Serial1 并同步打印到 USB 调试串口。
void sendLoraLine(const String& text);

// 发送 STATUS 帧给指定裁判端。
// deviceId：目标裁判端 deviceId。
// clientId：目标裁判位，如 client1/client2/client3；未绑定设备可传 UNASSIGNED。
// roundId：服务端当前轮号。
// roundOpen：当前轮是否仍开放收分。
// submitted：该裁判位本轮是否已经提交，用于客户端重启后恢复锁定状态。
void sendStatus(const String& deviceId, const String& clientId, uint32_t roundId,
                bool roundOpen, bool submitted);

// 发送 ASSIGN 帧，要求目标 deviceId 保存指定 clientId。
// deviceId：未绑定设备列表中选中的裁判端。
// clientId：要分配的裁判位。
void sendAssign(const String& deviceId, const String& clientId);

// 发送 UNBIND 帧，要求目标裁判端清除本地 clientId。
// deviceId：当前绑定表里需要解绑的裁判端 deviceId。
void sendUnbind(const String& deviceId);

// 发送 ACK 帧回应 SUBMIT。
// deviceId/clientId/roundIdText/msgIdText：均回填客户端 SUBMIT 中的对应字段，便于客户端精确匹配。
// status：OK、OK_DUPLICATE、ERR_ALREADY_SUBMITTED、ERR_BAD_ROUND 等协议结果码。
void sendAck(const String& deviceId, const String& clientId,
             const String& roundIdText, const String& msgIdText,
             const char* status);
