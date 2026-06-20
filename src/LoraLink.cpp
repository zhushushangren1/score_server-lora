// 服务端 LoRa UART 链路模块实现。
// 负责 E22 透传串口初始化、按行收发协议帧，并构造 STATUS/ASSIGN/UNBIND/ACK 下行帧。
#include "LoraLink.h"

#include <ScoreProtocol.h>

#include "StatusLeds.h"

namespace {

// E22-400T22D UART 透传接线。这里按 ESP32 方向命名：
// LORA_TX_PIN=ESP32 TX -> E22 RXD，LORA_RX_PIN=ESP32 RX <- E22 TXD。
constexpr int LORA_TX_PIN = 40;
constexpr int LORA_RX_PIN = 41;
constexpr int LORA_AUX_PIN = 42;
constexpr int LORA_M0_PIN = 38;
constexpr int LORA_M1_PIN = 39;
// 服务端和裁判端必须使用相同 UART 波特率，否则双方会看到乱码或无有效 CRC 帧。
constexpr uint32_t LORA_UART_BAUD = 9600;
// 保护行缓冲，超长时丢弃等待下一行，避免串口噪声撑爆 String。
constexpr size_t LORA_LINE_MAX = 120;

String loraLine;
bool loraDebugEnabled = false;
unsigned long lastLoraDebugPrintMs = 0;
uint32_t loraRawByteCount = 0;
uint32_t loraFrameCount = 0;
uint32_t loraOverflowCount = 0;

void waitForLoraReady() {
    const unsigned long start = millis();
    // AUX=LOW 表示 E22 忙，等待其恢复；1 秒超时防止硬件故障拖死主循环。
    while (digitalRead(LORA_AUX_PIN) == LOW && millis() - start < 1000) {
        delay(1);
    }
}

}  // namespace

void setupLoraLink() {
    pinMode(LORA_M0_PIN, OUTPUT);
    pinMode(LORA_M1_PIN, OUTPUT);
    // AUX 没接好时内部上拉可减少误判为 LOW 的概率。
    pinMode(LORA_AUX_PIN, INPUT_PULLUP);

    // M0=0/M1=0：普通透明传输模式，Serial1 写入的文本会直接通过 LoRa 发出。
    digitalWrite(LORA_M0_PIN, LOW);
    digitalWrite(LORA_M1_PIN, LOW);

    // Serial1 参数顺序是 RX,TX：GPIO41 接 E22 TXD，GPIO40 接 E22 RXD。
    Serial1.begin(LORA_UART_BAUD, SERIAL_8N1, LORA_RX_PIN, LORA_TX_PIN);
    // 给模块切模式和串口稳定留时间，避免启动立即发包丢首字节。
    delay(500);
}

bool readLoraFrame(String& frameText) {
    while (Serial1.available() > 0) {
        const char c = static_cast<char>(Serial1.read());
        loraRawByteCount++;
        if (c == '\r') {
            // 兼容 CRLF 行尾；协议实际以 '\n' 判断一帧结束。
            continue;
        }

        if (c == '\n') {
            if (loraLine.length() == 0) {
                // 空行直接跳过，避免上层把空字符串当成非法协议反复打印。
                continue;
            }
            pulseRxLed();
            loraFrameCount++;
            // 到达换行才返回完整帧，frameText 不包含 CR/LF。
            frameText = loraLine;
            loraLine = "";
            return true;
        }

        if (loraLine.length() < LORA_LINE_MAX) {
            loraLine += c;
        } else {
            // 超长通常来自乱码或缺失换行，清空后等待下一帧重新同步。
            loraLine = "";
            loraOverflowCount++;
        }
    }

    return false;
}

void setLoraDebugEnabled(bool enabled) {
    loraDebugEnabled = enabled;
    lastLoraDebugPrintMs = 0;
    Serial.print("LoRa debug ");
    Serial.println(enabled ? "ON" : "OFF");
}

void updateLoraDebug() {
    if (!loraDebugEnabled) {
        return;
    }

    const unsigned long now = millis();
    if (now - lastLoraDebugPrintMs < 1000) {
        return;
    }
    lastLoraDebugPrintMs = now;

    Serial.print("LoRa debug: raw=");
    Serial.print(loraRawByteCount);
    Serial.print(" frames=");
    Serial.print(loraFrameCount);
    Serial.print(" partialLen=");
    Serial.print(loraLine.length());
    Serial.print(" overflow=");
    Serial.print(loraOverflowCount);
    Serial.print(" avail=");
    Serial.print(Serial1.available());
    Serial.print(" AUX=");
    Serial.print(digitalRead(LORA_AUX_PIN));
    Serial.print(" M0=");
    Serial.print(digitalRead(LORA_M0_PIN));
    Serial.print(" M1=");
    Serial.print(digitalRead(LORA_M1_PIN));
    Serial.println();
}

void sendLoraLine(const String& text) {
    // 先点亮 TX 指示灯，即使 AUX 等待较短也能看到发送动作。
    pulseTxLed();
    // 尊重 E22 AUX 忙信号，降低 UART 写入时模块未准备好的风险。
    waitForLoraReady();
    // text 已由 ScoreProtocol::buildFrame 追加 '\n'，这里必须用 print 而不是 println。
    Serial1.print(text);
    Serial.print("LoRa TX: ");
    Serial.print(text);
}

void sendStatus(const String& deviceId, const String& clientId, uint32_t roundId,
                bool roundOpen, bool submitted) {
    // STATUS 字段：目标设备、服务端认为的 clientId、当前轮号、本轮是否开放、该裁判是否已提交。
    String fields[] = {
        "STATUS",
        deviceId,
        clientId,
        String(roundId),
        roundOpen ? "1" : "0",
        submitted ? "1" : "0"
    };

    sendLoraLine(ScoreProtocol::buildFrame(fields, 6));
}

void sendAssign(const String& deviceId, const String& clientId) {
    // ASSIGN 要求裁判端把 clientId 保存到 NVS，并立刻回 ASSIGN_ACK。
    String fields[] = {
        "ASSIGN",
        deviceId,
        clientId
    };
    sendLoraLine(ScoreProtocol::buildFrame(fields, 3));
}

void sendUnbind(const String& deviceId) {
    // UNBIND 只按 deviceId 点名，裁判端收到后删除本地 clientId。
    String fields[] = {
        "UNBIND",
        deviceId
    };
    sendLoraLine(ScoreProtocol::buildFrame(fields, 2));
}

void sendAck(const String& deviceId, const String& clientId,
             const String& roundIdText, const String& msgIdText,
             const char* status) {
    // ACK 原样带回客户端提交的 roundId/msgId，客户端用它们精确匹配当前 pending。
    String fields[] = {
        "ACK",
        deviceId,
        clientId,
        roundIdText,
        msgIdText,
        String(status)
    };
    sendLoraLine(ScoreProtocol::buildFrame(fields, 6));
}
