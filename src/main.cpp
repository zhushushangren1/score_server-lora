// 服务端程序入口。
// 负责初始化串口、NVS 状态、状态灯、LoRa、实体按钮和 Web 控制页，
// 并在 loop() 中调度 LoRa、HTTP、串口、按钮和状态灯模块。
#include <Arduino.h>

#include "LoraLink.h"
#include "ProtocolHandlers.h"
#include "SerialConsole.h"
#include "ServerButtons.h"
#include "ServerState.h"
#include "ServerWeb.h"
#include "StatusLeds.h"

void setup() {
    // 打开 USB 调试串口。服务端启动横幅、LoRa 收发、网页动作、按钮动作都会打印到这里。
    Serial.begin(115200);

    // USB CDC 枚举最多等 5 秒；脱机运行时不会无限等待。
    const unsigned long serialWaitStart = millis();
    while (!Serial && millis() - serialWaitStart < 5000) {
        delay(10);
    }
    delay(500);

    // 从 NVS 恢复绑定表、队名、总比分。
    // 必须早于 printBindings/printRoundState 和 WebUi 初始化，否则启动页面会显示默认值。
    loadBindingsFromNvs();

    Serial.println();
    Serial.println("score_server-lora boot");
    Serial.print("Millis: ");
    Serial.println(millis());
    printBindings();
    printRoundState();

    setupStatusLeds();    // 初始化电源/工作/TX/RX 四个状态灯。
    setupLoraLink();      // 初始化 E22 UART 和 M0/M1/AUX，进入透明传输模式。
    setupServerButtons(); // 初始化 GPIO5/GPIO12 实体按钮。
    setupServerWeb();     // 启动 WiFi AP 和 /score、/control HTTP 路由。

    Serial.println("E22 UART transparent ready");
    Serial.println("Serial commands: bind <deviceId> client1|2|3 / unbind clientX / list / next-round [seconds] / reset");
    Serial.println("Buttons: GPIO5 short = next-round, GPIO12 hold 3s = reset");
}

void loop() {
    // 优先消化 LoRa 收包，避免 E22 UART 缓冲积压导致丢帧。
    handleLoraInput();

    // 处理手机/电脑浏览器发来的 HTTP 请求。
    handleServerWebClient();

    // 处理 USB 串口命令，和网页/按钮共用同一套 ServerActions。
    handleSerialCommand();

    // 扫描服务端实体按钮：GPIO5 下一轮，GPIO12 长按重置。
    pollServerButtons();

    // 驱动工作灯心跳和 TX/RX 瞬时闪烁，全部非阻塞。
    updateStatusLeds();
}
