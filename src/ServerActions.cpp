// 服务端业务动作实现。
// 负责绑定、解绑、查看状态、进入下一轮、重置比分等可由串口/网页/按钮复用的动作。
#include "ServerActions.h"

#include "LoraLink.h"
#include "ServerState.h"

namespace {

// STATUS 广播参数：
// 下一轮/重置后不只发一次 STATUS，而是给每个已绑定裁判重复发送多轮。
// 这样某个裁判漏掉一帧时，后续重复帧仍能让它解锁或同步 reset 后的轮次。
constexpr uint8_t STATUS_BROADCAST_REPEAT_COUNT = 5;
constexpr unsigned long STATUS_BROADCAST_INTERVAL_MS = 160;

bool statusBroadcastActive = false;
uint8_t statusBroadcastRound = 0;
uint8_t statusBroadcastSlot = 0;
unsigned long statusBroadcastNextMs = 0;

void queueStatusBroadcast(const char* reason) {
    statusBroadcastActive = true;
    statusBroadcastRound = 0;
    statusBroadcastSlot = 0;
    statusBroadcastNextMs = 0;

    Serial.print("status broadcast queued: ");
    Serial.print(reason);
    Serial.print(" repeats=");
    Serial.println(STATUS_BROADCAST_REPEAT_COUNT);
}

}  // namespace

void handleBindCommand(const String args[], uint8_t argc) {
    if (argc != 2) {
        // bind 必须明确目标设备和裁判槽位；参数缺失时不猜测，避免误绑定。
        Serial.println("Usage: bind <deviceId> client1|client2|client3");
        return;
    }

    const String& targetDevice = args[0];
    const String& targetSlotName = args[1];

    const int slot = bindingSlotIndex(targetSlotName);
    if (slot < 0) {
        // 只允许绑定到固定的三个槽位，和协议 STATUS/SUBMIT 中的 clientId 保持一致。
        Serial.println("bind: invalid slot, must be client1/client2/client3");
        return;
    }

    if (bindings[slot].length() > 0) {
        // 不覆盖已有绑定，避免网页误点导致原裁判端失去身份。
        Serial.print("bind: ");
        Serial.print(targetSlotName);
        Serial.print(" already bound to ");
        Serial.print(bindings[slot]);
        Serial.println(", unbind first");
        printBindings();
        return;
    }

    if (findUnboundDevice(targetDevice) < 0) {
        // 只允许绑定已经发过 HELLO/HEARTBEAT 的设备，减少手输 deviceId 出错。
        Serial.print("bind: device ");
        Serial.print(targetDevice);
        Serial.println(" not in unbound table; wait for its HELLO first");
        printBindings();
        return;
    }

    const int otherSlot = findBindingByDevice(targetDevice);
    if (otherSlot >= 0) {
        // 同一 deviceId 不能同时占用两个裁判位，否则 SUBMIT 去重和状态显示都会混乱。
        Serial.print("bind: device already bound to ");
        Serial.print(BINDING_SLOT_NAMES[otherSlot]);
        Serial.println(", unbind first");
        printBindings();
        return;
    }

    saveBindingSlot(static_cast<uint8_t>(slot), targetDevice);
    // 新绑定后在线状态从空开始，等待该裁判端回 ASSIGN_ACK/HEARTBEAT 再显示在线。
    judgeStatuses[slot] = JudgeStatus();
    // 从未绑定表移除，控制页不会再把它列为可绑定设备。
    removeUnboundDevice(targetDevice);
    // 通过 LoRa 下发 ASSIGN，裁判端会保存到 NVS 并回 ASSIGN_ACK。
    sendAssign(targetDevice, targetSlotName);

    Serial.print("bind: assigned ");
    Serial.print(targetDevice);
    Serial.print(" -> ");
    Serial.println(targetSlotName);
    printBindings();
}

void handleUnbindCommand(const String args[], uint8_t argc) {
    if (argc != 1) {
        // unbind 只接受槽位名，不接受 deviceId，避免解绑错对象。
        Serial.println("Usage: unbind client1|client2|client3");
        return;
    }

    const int slot = bindingSlotIndex(args[0]);
    if (slot < 0) {
        // 参数必须能映射到 bindings[] 下标。
        Serial.println("unbind: invalid slot");
        return;
    }

    if (bindings[slot].length() == 0) {
        // 幂等处理：空槽位解绑不改状态，只打印当前绑定表。
        Serial.print("unbind: ");
        Serial.print(args[0]);
        Serial.println(" already unbound");
        printBindings();
        return;
    }

    const String deviceId = bindings[slot];
    // 先清服务端持久化状态，再通知裁判端；即使 UNBIND 丢包，后续心跳也会被服务端纠正。
    saveBindingSlot(static_cast<uint8_t>(slot), String());
    judgeStatuses[slot] = JudgeStatus();
    sendUnbind(deviceId);

    Serial.print("unbind: cleared ");
    Serial.print(args[0]);
    Serial.print(" (was ");
    Serial.print(deviceId);
    Serial.println(")");
    printBindings();
}

void handleListCommand() {
    // 串口 list 是现场排查入口，集中打印绑定、未绑定、当前轮提交三类状态。
    printBindings();
    printUnboundDevices();
    printRoundState();
}

void handleNextRoundCommand() {
    // 实体按钮和无参数串口命令默认不启动倒计时。
    handleNextRoundCommand(0);
}

void handleNextRoundCommand(uint32_t countdownSeconds) {
    // 如果当前轮已经满足结算条件，先把轮分计入总比分，再切到下一轮。
    applyCompletedRoundScoreIfReady();
    currentRoundId++;
    // 新轮次重新开放收分，并允许下一次 applyCompletedRoundScoreIfReady 结算。
    roundOpen = true;
    roundScoreApplied = false;
    resetRoundSubmissions();
    // countdownSeconds=0 会停止/不启动倒计时，网页显示 --:--。
    startCountdown(countdownSeconds);

    Serial.print("next-round: advanced to ");
    Serial.println(currentRoundId);
    if (countdownSeconds > 0) {
        Serial.print("countdown: ");
        Serial.print(countdownSeconds);
        Serial.println("s");
    }

    queueStatusBroadcast("next-round");

    printRoundState();
}

void handleResetCommand() {
    // 重置比赛状态但保留绑定表和队名，适合一场比赛重新开始。
    resetMatchScores();
    Serial.println("reset: match state cleared (roundId=1, totals=0)");

    queueStatusBroadcast("reset");

    printRoundState();
}

void driveStatusBroadcast() {
    if (!statusBroadcastActive) {
        return;
    }
    if (statusBroadcastNextMs != 0 &&
        static_cast<long>(millis() - statusBroadcastNextMs) < 0) {
        return;
    }

    while (statusBroadcastRound < STATUS_BROADCAST_REPEAT_COUNT) {
        while (statusBroadcastSlot < BINDING_SLOT_COUNT) {
            const uint8_t slot = statusBroadcastSlot++;
            if (bindings[slot].length() == 0) {
                continue;
            }

            Serial.print("STATUS broadcast ");
            Serial.print(statusBroadcastRound + 1);
            Serial.print("/");
            Serial.print(STATUS_BROADCAST_REPEAT_COUNT);
            Serial.print(" -> ");
            Serial.println(BINDING_SLOT_NAMES[slot]);

            sendStatus(bindings[slot], BINDING_SLOT_NAMES[slot], currentRoundId, roundOpen,
                       roundSubmissions[slot].submitted);
            statusBroadcastNextMs = millis() + STATUS_BROADCAST_INTERVAL_MS;
            return;
        }

        statusBroadcastSlot = 0;
        statusBroadcastRound++;
    }

    statusBroadcastActive = false;
    statusBroadcastNextMs = 0;
    Serial.println("status broadcast complete");
}
