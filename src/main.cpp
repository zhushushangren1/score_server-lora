#include <Arduino.h>
#include <ScoreProtocol.h>

// 服务端 E22-400T22D 串口接线：
// ESP32-S3 GPIO17 TX -> E22 RXD
// ESP32-S3 GPIO18 RX <- E22 TXD
// ESP32-S3 GPIO11     <- E22 AUX
// ESP32-S3 GPIO12     -> E22 M0
// ESP32-S3 GPIO13     -> E22 M1
//
// M0=LOW 且 M1=LOW 时，E22 进入普通透明传输模式。
// E22 模块出厂串口波特率通常是 9600。
constexpr int LORA_TX_PIN = 17;
constexpr int LORA_RX_PIN = 18;
constexpr int LORA_AUX_PIN = 11;
constexpr int LORA_M0_PIN = 12;
constexpr int LORA_M1_PIN = 13;
constexpr uint32_t LORA_UART_BAUD = 9600;

// LoRa 接收行缓冲，handleLoraInput 按字节追加，遇到 '\n' 即视为一帧结束。
String loraLine;

// 等待 E22 进入空闲状态再发送，避免在模块忙时数据被吞掉。
// E22 的 AUX：HIGH=空闲可发送，LOW=正在收发或初始化。
// 1 秒超时是兜底，防止 AUX 接线错误（悬空或始终被拉低）让程序永久卡死。
void waitForLoraReady() {
    const unsigned long start = millis();
    while (digitalRead(LORA_AUX_PIN) == LOW && millis() - start < 1000) {
        delay(1);
    }
}

// 通过 Serial1 把一行已组好的协议数据写到 E22 进行无线发送，并在调试串口回显。
// text：已经包含 CRC 和末尾 '\n' 的完整协议行（通常来自 ScoreProtocol::buildFrame）。
void sendLoraLine(const String& text) {
    waitForLoraReady();
    Serial1.print(text);
    Serial.print("LoRa TX: ");
    Serial.print(text);
}

// 组装并发送一帧 STATUS，作为对裁判机 HELLO 的应答（联通测试用最小实现）。
// 帧格式：STATUS,deviceId,clientId,roundId,roundOpen,submitted,crc16。
// deviceId：从收到的 HELLO 取来，原样回填，便于裁判机识别这是发给自己的应答。
// clientId：裁判机当前自报的 ID（联通测试阶段通常是 "UNASSIGNED"）。
// 其余 roundId/roundOpen/submitted 暂时硬编码为 "1"/"1"/"0"，等绑定与轮次逻辑接入后再换成真值。
void sendStatus(const String& deviceId, const String& clientId) {
    String fields[] = {
        "STATUS",
        deviceId,
        clientId,
        "1",  // roundId
        "1",  // roundOpen: 1=true
        "0"   // submitted: 0=false
    };

    sendLoraLine(ScoreProtocol::buildFrame(fields, 6));
}

// 把已成功解析的协议帧打印到调试串口，便于联通测试时观察字段内容。
// frame：parseFrame 返回 true 时填好的结构体。
void printParsedFrame(const ScoreProtocol::ParsedFrame& frame) {
    Serial.print("Parsed type: ");
    Serial.println(ScoreProtocol::messageTypeToString(frame.type));
    Serial.print("Field count: ");
    Serial.println(frame.fieldCount);

    for (uint8_t i = 0; i < frame.fieldCount; i++) {
        Serial.print("  [");
        Serial.print(i);
        Serial.print("] ");
        Serial.println(frame.fields[i]);
    }
}

// 从 Serial1（E22 透传口）按字节读入，遇到 '\n' 视为一行结束，然后做 CRC 校验、字段解析、类型识别、打印和业务处理。
// 当前业务非常简单：识别到 HELLO 就回一帧 STATUS。
// 行长度上限 120 字节，超长直接丢弃，防止异常输入把 String 无限撑大。
void handleLoraInput() {
    while (Serial1.available() > 0) {
        const char c = static_cast<char>(Serial1.read());
        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            if (loraLine.length() > 0) {
                Serial.print("LoRa RX: ");
                Serial.println(loraLine);

                ScoreProtocol::ParsedFrame frame;
                if (ScoreProtocol::parseFrame(loraLine, frame)) {
                    printParsedFrame(frame);

                    // 最小协议测试：收到 HELLO 后回复 STATUS。
                    // HELLO 帧格式为 HELLO,deviceId,clientId,battMv，fieldCount 必须等于 4。
                    if (frame.type == ScoreProtocol::MessageType::Hello && frame.fieldCount == 4) {
                        const String deviceId = frame.fields[1];
                        const String clientId = frame.fields[2];
                        sendStatus(deviceId, clientId);
                    }
                } else {
                    Serial.println("Invalid protocol frame");
                }

                loraLine = "";
            }
            continue;
        }

        if (loraLine.length() < 120) {
            loraLine += c;
        } else {
            // 丢弃过长数据，避免异常输入导致 String 无限增长。
            loraLine = "";
        }
    }
}

// Arduino 启动钩子，上电后只运行一次。
// 职责：初始化调试串口、把 E22 控制脚切到透传模式（M0=M1=LOW）、打开 Serial1 与 E22 通信、并打印启动横幅。
void setup() {
    Serial.begin(115200);
    const unsigned long serialWaitStart = millis();
    while (!Serial && millis() - serialWaitStart < 5000) {
        delay(10);
    }
    delay(500);

    Serial.println();
    Serial.println("score_server-lora boot");
    Serial.print("Millis: ");
    Serial.println(millis());

    pinMode(LORA_M0_PIN, OUTPUT);
    pinMode(LORA_M1_PIN, OUTPUT);
    pinMode(LORA_AUX_PIN, INPUT_PULLUP);

    // M0=LOW、M1=LOW：E22 进入正常透明传输模式（详见设计文档 4.3 节）。
    digitalWrite(LORA_M0_PIN, LOW);
    digitalWrite(LORA_M1_PIN, LOW);

    Serial1.begin(LORA_UART_BAUD, SERIAL_8N1, LORA_RX_PIN, LORA_TX_PIN);
    delay(500);

    Serial.println("E22 UART transparent test ready");
    Serial.println("Server replies STATUS when it receives HELLO.");
}

// Arduino 主循环，会被反复调用。
// 服务端当前只做一件事：把 LoRa 收到的数据消化掉（解析 + 回 STATUS）。
void loop() {
    handleLoraInput();
}
