// 服务端实体按钮模块实现。
// 负责 GPIO5 下一轮按钮和 GPIO12 长按重置按钮的初始化、去抖和事件触发。
#include "ServerButtons.h"

#include <Arduino.h>

#include "ServerActions.h"

namespace {

constexpr int BUTTON_NEXT_PIN = 5;
constexpr int BUTTON_RESET_PIN = 12;
constexpr unsigned long BUTTON_DEBOUNCE_MS = 20;
constexpr unsigned long BUTTON_RESET_LONG_PRESS_MS = 3000;

struct ButtonState {
    // INPUT_PULLUP 下 true=松开/HIGH，false=按下/LOW。
    bool stableLevel = true;
    // 未去抖的最新读数，用来检测抖动边沿。
    bool lastRawLevel = true;
    // raw 电平最后变化时间；只有稳定超过 BUTTON_DEBOUNCE_MS 才更新 stableLevel。
    unsigned long lastRawChangeMs = 0;
    // reset 按钮稳定按下的起始时间，用于 3 秒长按判断。
    unsigned long pressedAtMs = 0;
    // 本次按住期间是否已经触发重置，避免长按持续期间重复 reset。
    bool longFired = false;
};

ButtonState nextButton;
ButtonState resetButton;

}  // namespace

void setupServerButtons() {
    // 两个按钮均采用一端接 GPIO、一端接 GND 的接法，依赖内部上拉。
    pinMode(BUTTON_NEXT_PIN, INPUT_PULLUP);
    pinMode(BUTTON_RESET_PIN, INPUT_PULLUP);
}

void pollServerButtons() {
    const unsigned long now = millis();

    {
        ButtonState& button = nextButton;
        // raw=true 表示松开，raw=false 表示按下。
        const bool raw = digitalRead(BUTTON_NEXT_PIN) != LOW;
        if (raw != button.lastRawLevel) {
            // 原始电平发生变化时重启去抖计时。
            button.lastRawLevel = raw;
            button.lastRawChangeMs = now;
        }
        if (now - button.lastRawChangeMs >= BUTTON_DEBOUNCE_MS &&
            raw != button.stableLevel) {
            // 电平稳定后才承认按键状态变化。
            button.stableLevel = raw;
            if (!raw) {
                // next 按钮按下沿触发，不等松手；无倒计时参数，等价 next-round 0。
                Serial.println("BUTTON: next-round");
                handleNextRoundCommand();
            }
        }
    }

    {
        ButtonState& button = resetButton;
        // reset 使用长按，短按不做任何事，降低误触清分风险。
        const bool raw = digitalRead(BUTTON_RESET_PIN) != LOW;
        if (raw != button.lastRawLevel) {
            // 任意抖动都会重置去抖计时，直到电平稳定。
            button.lastRawLevel = raw;
            button.lastRawChangeMs = now;
        }
        if (now - button.lastRawChangeMs >= BUTTON_DEBOUNCE_MS &&
            raw != button.stableLevel) {
            button.stableLevel = raw;
            if (!raw) {
                // 稳定按下后开始计长按时长。
                button.pressedAtMs = now;
                button.longFired = false;
            }
        }
        if (!button.stableLevel && !button.longFired &&
            now - button.pressedAtMs >= BUTTON_RESET_LONG_PRESS_MS) {
            // 长按达到 3 秒立即触发重置，不需要等松开。
            button.longFired = true;
            Serial.println("BUTTON: reset (long press 3s)");
            handleResetCommand();
        }
    }
}
