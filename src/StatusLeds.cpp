#include "StatusLeds.h"

namespace {

// 服务端状态灯接线：电源常亮、工作闪烁、TX/RX 在 LoRa 收发时短亮。
constexpr int LED_POWER_PIN = 45;
constexpr int LED_WORK_PIN = 48;
constexpr int LED_TX_PIN = 47;
constexpr int LED_RX_PIN = 21;

constexpr unsigned long WORK_BLINK_INTERVAL_MS = 500;
constexpr unsigned long LED_PULSE_MS = 60;

unsigned long lastWorkBlinkMs = 0;
bool workLedOn = false;
unsigned long txLedOffMs = 0;
unsigned long rxLedOffMs = 0;

}  // namespace

void setupStatusLeds() {
    // 所有状态灯均为 GPIO -> 限流电阻 -> LED -> GND，高电平点亮。
    pinMode(LED_POWER_PIN, OUTPUT);
    pinMode(LED_WORK_PIN, OUTPUT);
    pinMode(LED_TX_PIN, OUTPUT);
    pinMode(LED_RX_PIN, OUTPUT);
    // 电源灯上电常亮，用来确认 3.3V/5V 供电和程序已经执行到 setup。
    digitalWrite(LED_POWER_PIN, HIGH);
    digitalWrite(LED_WORK_PIN, LOW);
    digitalWrite(LED_TX_PIN, LOW);
    digitalWrite(LED_RX_PIN, LOW);
}

void pulseTxLed() {
    // 发送指示灯只点亮一小段时间，避免高频发送时常亮看不出动作。
    digitalWrite(LED_TX_PIN, HIGH);
    txLedOffMs = millis() + LED_PULSE_MS;
}

void pulseRxLed() {
    // 收到完整 LoRa 帧时点亮，由 updateStatusLeds 到期熄灭。
    digitalWrite(LED_RX_PIN, HIGH);
    rxLedOffMs = millis() + LED_PULSE_MS;
}

void updateStatusLeds() {
    const unsigned long now = millis();
    if (now - lastWorkBlinkMs >= WORK_BLINK_INTERVAL_MS) {
        // 工作灯周期翻转，证明 loop 没有被 WiFi/LoRa/串口阻塞。
        lastWorkBlinkMs = now;
        workLedOn = !workLedOn;
        digitalWrite(LED_WORK_PIN, workLedOn ? HIGH : LOW);
    }
    if (txLedOffMs != 0 && static_cast<long>(now - txLedOffMs) >= 0) {
        // 使用有符号差值判断到期，兼容 millis 回绕。
        digitalWrite(LED_TX_PIN, LOW);
        txLedOffMs = 0;
    }
    if (rxLedOffMs != 0 && static_cast<long>(now - rxLedOffMs) >= 0) {
        // 到期后清零 offMs，表示当前没有未完成的 RX 脉冲。
        digitalWrite(LED_RX_PIN, LOW);
        rxLedOffMs = 0;
    }
}
