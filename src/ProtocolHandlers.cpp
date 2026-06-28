// 服务端 LoRa 入站协议处理实现。
// 负责处理裁判端发来的 HELLO、HEARTBEAT、SUBMIT、ASSIGN_ACK、UNBIND_ACK。
#include "ProtocolHandlers.h"

#include <Arduino.h>
#include <ScoreProtocol.h>

#include "LoraLink.h"
#include "ServerState.h"

namespace {

constexpr unsigned long SUBMIT_STATUS_FOLLOWUP_DELAY_MS = 80;

void sendCurrentStatus(const String& deviceId, const String& clientId) {
    bool submitted = false;
    const int slot = bindingSlotIndex(clientId);
    if (slot >= 0 && roundSubmissions[slot].submitted) {
        // submitted 告诉客户端“本轮你已经交过”，客户端重启后可以保持锁定。
        submitted = true;
    }
    sendStatus(deviceId, clientId, currentRoundId, roundOpen, submitted);
}

void sendAckWithStatusFollowup(const String& deviceId, const String& clientId,
                               const String& roundIdText, const String& msgIdText,
                               const char* status) {
    sendAck(deviceId, clientId, roundIdText, msgIdText, status);
    // ACK 是客户端清 pending 的主确认；STATUS 是 ACK 丢包时的兜底确认。
    // 短暂等待后再发 STATUS，避免两帧过近导致客户端刚完成发射切回接收时漏掉。
    delay(SUBMIT_STATUS_FOLLOWUP_DELAY_MS);
    sendCurrentStatus(deviceId, clientId);
}

void printParsedFrame(const ScoreProtocol::ParsedFrame& frame) {
    // 收到合法 CRC 帧后打印字段，方便现场核对客户端到底发了什么。
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

void respondToPresence(const String& deviceId, const String& clientId,
                       const String& battText, const char* frameLabel) {
    int battMv = 0;
    // 电池字段异常不会丢弃整帧，只会在网页显示 "--"。
    parseBatteryMv(battText, battMv);

    if (clientId == "UNASSIGNED") {
        const int existingSlot = findBindingByDevice(deviceId);
        if (existingSlot >= 0) {
            // 客户端可能刚擦过 NVS 或没收到 ASSIGN；服务端以绑定表为准，主动重发 ASSIGN。
            Serial.print(frameLabel);
            Serial.print(": ");
            Serial.print(deviceId);
            Serial.print(" claims UNASSIGNED but server has it bound to ");
            Serial.print(BINDING_SLOT_NAMES[existingSlot]);
            Serial.println(", resending ASSIGN");
            updateJudgeStatus(static_cast<uint8_t>(existingSlot), battMv);
            sendAssign(deviceId, BINDING_SLOT_NAMES[existingSlot]);
            return;
        }

        const bool isNewUnbound = findUnboundDevice(deviceId) < 0;
        if (!upsertUnboundDevice(deviceId, battMv)) {
            // 未绑定表容量有限，满了以后只打印丢弃，不影响已绑定裁判通信。
            Serial.print("Unbound table full, dropped: ");
            Serial.println(deviceId);
            appendEventLog(String("未绑定设备表已满，丢弃：") + deviceId);
        } else {
            if (isNewUnbound) {
                appendEventLog(String("发现未绑定裁判端：") + deviceId);
            }
            // 每次刷新都打印，便于串口无网页时看到新设备上线。
            printUnboundDevices();
        }
        // 未绑定设备也回 STATUS，客户端可借此知道服务端当前轮号，但仍不能提交。
        sendCurrentStatus(deviceId, clientId);
        return;
    }

    const int slot = bindingSlotIndex(clientId);
    if (slot < 0) {
        // 客户端自报了未知身份，要求它清空本地 clientId，回到可重新绑定状态。
        Serial.print(frameLabel);
        Serial.print(": unknown clientId '");
        Serial.print(clientId);
        Serial.println("', sending UNBIND to correct");
        appendEventLog(String(frameLabel) + " 身份非法：" + deviceId + " -> " + clientId);
        sendUnbind(deviceId);
        return;
    }

    if (bindings[slot] != deviceId) {
        // 客户端自报的 clientId 和服务端绑定表不一致，服务端绑定表优先。
        Serial.print(frameLabel);
        Serial.print(": ");
        Serial.print(deviceId);
        Serial.print(" claims ");
        Serial.print(clientId);
        Serial.print(" but server has '");
        Serial.print(bindings[slot]);
        Serial.println("', sending UNBIND to correct");
        appendEventLog(String(frameLabel) + " 绑定不匹配：" + deviceId + " claims " + clientId);
        sendUnbind(deviceId);
        return;
    }

    // 身份一致时刷新在线状态，并把当前轮状态回给客户端。
    updateJudgeStatus(static_cast<uint8_t>(slot), battMv);
    sendCurrentStatus(deviceId, clientId);
}

void handleHello(const ScoreProtocol::ParsedFrame& frame) {
    if (frame.fieldCount != 4) {
        // HELLO,deviceId,clientId,batteryMv,CRC。
        Serial.println("HELLO: bad field count, ignored");
        return;
    }
    respondToPresence(frame.fields[1], frame.fields[2], frame.fields[3], "HELLO");
}

void handleHeartbeat(const ScoreProtocol::ParsedFrame& frame) {
    if (frame.fieldCount != 5) {
        // HEARTBEAT,deviceId,clientId,batteryMv,msgId,CRC；msgId 仅用于日志，不参与业务。
        Serial.println("HEARTBEAT: bad field count, ignored");
        return;
    }
    respondToPresence(frame.fields[1], frame.fields[2], frame.fields[3], "HEARTBEAT");
}

void handleSubmit(const ScoreProtocol::ParsedFrame& frame) {
    if (frame.fieldCount != 8) {
        // SUBMIT,deviceId,clientId,roundId,msgId,red,blue,batteryMv,CRC。
        Serial.println("SUBMIT: bad field count, ignored");
        return;
    }

    const String& subDevice = frame.fields[1];
    const String& subClient = frame.fields[2];
    const String& subRoundText = frame.fields[3];
    const String& subMsgText = frame.fields[4];
    const String& subRedText = frame.fields[5];
    const String& subBlueText = frame.fields[6];
    const String& subBattText = frame.fields[7];

    const int slot = bindingSlotIndex(subClient);
    if (slot < 0) {
        // SUBMIT 携带非法 clientId 时不能入账，先让客户端清绑定。
        Serial.print("SUBMIT: unknown clientId '");
        Serial.print(subClient);
        Serial.println("', sending UNBIND to correct");
        appendEventLog(String("SUBMIT 身份非法：") + subDevice + " -> " + subClient);
        sendUnbind(subDevice);
        return;
    }
    if (bindings[slot] != subDevice) {
        // 防止旧绑定客户端或误烧录设备冒用某个裁判位提交分数。
        Serial.print("SUBMIT: ");
        Serial.print(subDevice);
        Serial.print(" claims ");
        Serial.print(subClient);
        Serial.print(" but server has '");
        Serial.print(bindings[slot]);
        Serial.println("', sending UNBIND to correct");
        appendEventLog(String("SUBMIT 绑定不匹配：") + subDevice + " claims " + subClient);
        sendUnbind(subDevice);
        return;
    }

    int battMv = 0;
    // SUBMIT 也刷新电量和在线状态，哪怕后续轮号/分数校验失败。
    parseBatteryMv(subBattText, battMv);
    updateJudgeStatus(static_cast<uint8_t>(slot), battMv);

    unsigned long subRound = 0;
    if (!ScoreProtocol::parseUnsignedLong(subRoundText, subRound)) {
        // 轮号不可解析时无法回 ACK_BAD_ROUND，因为客户端也可能无法匹配；直接丢弃。
        Serial.println("SUBMIT: bad roundId, ignored");
        return;
    }
    unsigned long subMsgId = 0;
    if (!ScoreProtocol::parseUnsignedLong(subMsgText, subMsgId)) {
        // msgId 是去重关键字段，非法时不能接受。
        Serial.println("SUBMIT: bad msgId, ignored");
        return;
    }
    int red = 0;
    int blue = 0;
    if (!ScoreProtocol::parseIntInRange(subRedText, 0, 99, red) ||
        !ScoreProtocol::parseIntInRange(subBlueText, 0, 99, blue)) {
        // 分数越界说明客户端状态异常或串口输入错误，服务端不入账。
        Serial.println("SUBMIT: bad red/blue, ignored");
        return;
    }

    if (subRound != currentRoundId) {
        // 客户端轮号落后/超前时拒绝，并回 ACK 让客户端停止重传该旧提交。
        Serial.print("SUBMIT: roundId ");
        Serial.print(subRound);
        Serial.print(" != current ");
        Serial.println(currentRoundId);
        appendEventLog(String("提交轮次错误：") + subClient + " round " + String(subRound) +
                       "，当前 Round " + String(currentRoundId));
        sendAckWithStatusFollowup(subDevice, subClient, subRoundText, subMsgText, "ERR_BAD_ROUND");
        return;
    }

    PerJudgeSubmission& submission = roundSubmissions[slot];
    if (submission.submitted && submission.lastMsgId == subMsgId) {
        // 同一个 msgId 重复到达通常是 ACK 丢了；不重复入账，只补发 OK_DUPLICATE。
        Serial.print("SUBMIT: duplicate msgId=");
        Serial.print(subMsgId);
        Serial.println(", ack only");
        appendEventLog(String("重复提交确认：") + subClient + " msgId=" + String(subMsgId));
        sendAckWithStatusFollowup(subDevice, subClient, subRoundText, subMsgText, "OK_DUPLICATE");
        return;
    }
    if (submission.submitted) {
        // 本轮已经收过该裁判成绩，但 msgId 不同，视为重复改分尝试，不覆盖原分。
        Serial.print("SUBMIT: ");
        Serial.print(subClient);
        Serial.println(" already submitted this round");
        appendEventLog(String("重复改分被拒绝：") + subClient + " Round " + String(currentRoundId));
        sendAckWithStatusFollowup(subDevice, subClient, subRoundText, subMsgText,
                                  "ERR_ALREADY_SUBMITTED");
        return;
    }

    // 所有校验通过后才写入本轮提交表。
    submission.submitted = true;
    submission.lastMsgId = subMsgId;
    submission.red = red;
    submission.blue = blue;

    Serial.print("SUBMIT accepted: ");
    Serial.print(subClient);
    Serial.print(" round=");
    Serial.print(subRound);
    Serial.print(" msgId=");
    Serial.print(subMsgId);
    Serial.print(" red=");
    Serial.print(red);
    Serial.print(" blue=");
    Serial.println(blue);
    appendEventLog(String("收到提交：") + subClient + " Round " + String(subRound) +
                   " 红=" + String(red) + " 蓝=" + String(blue));

    // 先 ACK 客户端，再尝试结算本轮；即使结算打印较慢，也不影响客户端尽快锁定。
    sendAck(subDevice, subClient, subRoundText, subMsgText, "OK");
    applyCompletedRoundScoreIfReady();
    // ACK 可能因下行瞬时丢包没到客户端；补发 STATUS 让客户端可用 submitted=1 兜底锁定。
    delay(SUBMIT_STATUS_FOLLOWUP_DELAY_MS);
    sendCurrentStatus(subDevice, subClient);
    printRoundState();
}

void handleAssignAck(const ScoreProtocol::ParsedFrame& frame) {
    if (frame.fieldCount != 3) {
        // ASSIGN_ACK,deviceId,clientId,CRC。
        Serial.println("ASSIGN_ACK: bad field count, ignored");
        return;
    }
    Serial.print("ASSIGN_ACK from ");
    Serial.print(frame.fields[1]);
    Serial.print(" as ");
    Serial.println(frame.fields[2]);
}

void handleUnbindAck(const ScoreProtocol::ParsedFrame& frame) {
    if (frame.fieldCount != 2) {
        // UNBIND_ACK,deviceId,CRC。
        Serial.println("UNBIND_ACK: bad field count, ignored");
        return;
    }
    Serial.print("UNBIND_ACK from ");
    Serial.println(frame.fields[1]);
}

void dispatchFrame(const ScoreProtocol::ParsedFrame& frame) {
    // 服务端只处理客户端上行消息；STATUS/ACK 是服务端下行消息，收到也忽略。
    switch (frame.type) {
        case ScoreProtocol::MessageType::Hello:
            handleHello(frame);
            break;
        case ScoreProtocol::MessageType::Heartbeat:
            handleHeartbeat(frame);
            break;
        case ScoreProtocol::MessageType::Submit:
            handleSubmit(frame);
            break;
        case ScoreProtocol::MessageType::AssignAck:
            handleAssignAck(frame);
            break;
        case ScoreProtocol::MessageType::UnbindAck:
            handleUnbindAck(frame);
            break;
        default:
            break;
    }
}

}  // namespace

void handleLoraInput() {
    String line;
    while (readLoraFrame(line)) {
        // 一次 loop 尽量清空 Serial1 中已经凑齐的所有完整帧。
        Serial.print("LoRa RX: ");
        Serial.println(line);

        ScoreProtocol::ParsedFrame frame;
        if (ScoreProtocol::parseFrame(line, frame)) {
            // 共享协议库已完成 CRC 校验和消息类型识别，业务处理只关心字段。
            printParsedFrame(frame);
            dispatchFrame(frame);
        } else {
            // 乱码、CRC 错误、字段超长、未知消息类型都不会进入业务状态机。
            Serial.println("Invalid protocol frame");
            appendEventLog("收到无效 LoRa 帧");
        }
    }
}
