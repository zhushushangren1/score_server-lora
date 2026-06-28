// 服务端 USB 串口调试命令实现。
// 负责解析 bind、unbind、list、next-round、reset、lora-debug 等现场调试命令。
#include "SerialConsole.h"

#include <Arduino.h>

#include "LoraLink.h"
#include "ServerActions.h"

namespace {

String serialLine;

uint8_t tokenizeBySpace(const String& line, String tokens[], uint8_t maxTokens) {
    uint8_t count = 0;
    int start = 0;
    const int len = line.length();

    while (start < len && count < maxTokens) {
        while (start < len && line[start] == ' ') {
            // 跳过连续空格，串口手输时不用严格只输入一个空格。
            start++;
        }
        if (start >= len) {
            break;
        }

        int end = start;
        while (end < len && line[end] != ' ') {
            // 当前命令集没有带空格的参数，因此按空格切 token 即可。
            end++;
        }
        tokens[count++] = line.substring(start, end);
        start = end;
    }
    return count;
}

}  // namespace

void handleSerialCommand() {
    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());

        if (c == '\r' || c == '\n') {
            // CR、LF、CRLF 都作为命令结束；复制后清空缓冲准备接下一条。
            String line = serialLine;
            serialLine = "";

            line.trim();
            if (line.length() == 0) {
                // 过滤 CRLF 中第二个空行，避免重复打印 Unknown command。
                continue;
            }

            Serial.print("CMD: ");
            Serial.println(line);

            constexpr uint8_t MAX_TOKENS = 4;
            String tokens[MAX_TOKENS];
            // 当前最长命令 bind <deviceId> <clientId> 只需要 3 个 token，4 个留一点余量。
            const uint8_t n = tokenizeBySpace(line, tokens, MAX_TOKENS);

            if (n == 0) {
                continue;
            }

            if (tokens[0] == "bind") {
                // 传入命令名之后的参数：deviceId 和 clientId。
                handleBindCommand(&tokens[1], static_cast<uint8_t>(n - 1));
            } else if (tokens[0] == "unbind") {
                // 传入命令名之后的参数：client1/client2/client3。
                handleUnbindCommand(&tokens[1], static_cast<uint8_t>(n - 1));
            } else if (tokens[0] == "list") {
                handleListCommand();
            } else if (tokens[0] == "next-round") {
                // next-round 后可选倒计时秒数；无参数或解析为 0 时不启动倒计时。
                const uint32_t seconds = n >= 2 ? static_cast<uint32_t>(tokens[1].toInt()) : 0;
                handleNextRoundCommand(seconds);
            } else if (tokens[0] == "reset") {
                handleResetCommand();
            } else if (tokens[0] == "lora-debug") {
                const bool enabled = !(n >= 2 && tokens[1] == "off");
                setLoraDebugEnabled(enabled);
            } else {
                Serial.print("Unknown command: ");
                Serial.println(tokens[0]);
                Serial.println("Available: bind / unbind / list / next-round [seconds] / reset / lora-debug [on|off]");
            }

            continue;
        }

        if (serialLine.length() < 80) {
            serialLine += c;
        } else {
            // 超长输入直接丢弃，避免残缺命令误触发绑定/重置。
            serialLine = "";
        }
    }
}
