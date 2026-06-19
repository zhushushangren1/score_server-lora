// 服务端全局状态和持久化实现。
// 负责绑定表、未绑定设备、队名、总比分、轮次、提交表、在线状态和倒计时管理。
#include "ServerState.h"

#include <Preferences.h>
#include <ScoreProtocol.h>

namespace {

constexpr const char* NVS_NAMESPACE = "score_server";

// 绑定表按 client1/client2/client3 分别保存，重启后仍能记住裁判端 deviceId。
const char* const NVS_BINDING_KEYS[BINDING_SLOT_COUNT] = {
    "bind_client1", "bind_client2", "bind_client3"
};

// 队名和总比分独立持久化，重置比分不会清绑定关系，改队名也不会影响轮次。
constexpr const char* NVS_TEAM_KEYS[TEAM_COUNT] = {
    "team0", "team1"
};

constexpr const char* NVS_TOTAL_KEYS[TEAM_COUNT] = {
    "total0", "total1"
};

Preferences prefs;

int finalScoreFromThree(int a, int b, int c) {
    // 三裁判规则：有两个相同就采用相同值。
    if (a == b || a == c) {
        return a;
    }
    if (b == c) {
        return b;
    }
    // 三个都不同时取最低值，避免分歧时偏向高分。
    return min(a, min(b, c));
}

int finalScoreFromSubmitted(const int scores[], uint8_t count) {
    if (count == 0) {
        // 没有绑定裁判或没有可用提交时不产生本轮分数。
        return 0;
    }
    if (count == 1) {
        // 现场单裁判测试时允许一人完成一轮，直接采用该裁判分数。
        return scores[0];
    }
    if (count == 2) {
        // 两裁判时相同取相同，不同取较低值，和三裁判全不同策略保持保守。
        return scores[0] == scores[1] ? scores[0] : min(scores[0], scores[1]);
    }
    return finalScoreFromThree(scores[0], scores[1], scores[2]);
}

void saveTotalsToNvs() {
    for (uint8_t i = 0; i < TEAM_COUNT; i++) {
        prefs.putInt(NVS_TOTAL_KEYS[i], totalScores[i]);
    }
}

void saveTeamNamesToNvs() {
    for (uint8_t i = 0; i < TEAM_COUNT; i++) {
        prefs.putString(NVS_TEAM_KEYS[i], teamNames[i]);
    }
}

}  // namespace

UnboundDevice unboundDevices[MAX_UNBOUND_DEVICES];

const char* const BINDING_SLOT_NAMES[BINDING_SLOT_COUNT] = {
    "client1", "client2", "client3"
};

String bindings[BINDING_SLOT_COUNT];
String teamNames[TEAM_COUNT] = {"红方", "蓝方"};
int totalScores[TEAM_COUNT] = {0, 0};
uint32_t currentRoundId = 1;
bool roundOpen = true;
bool roundScoreApplied = false;
PerJudgeSubmission roundSubmissions[BINDING_SLOT_COUNT];
JudgeStatus judgeStatuses[BINDING_SLOT_COUNT];

namespace {

bool countdownActive = false;
uint32_t countdownDurationSec = 0;
unsigned long countdownEndMs = 0;

}  // namespace

int findUnboundDevice(const String& id) {
    for (uint8_t i = 0; i < MAX_UNBOUND_DEVICES; i++) {
        if (unboundDevices[i].id.length() > 0 && unboundDevices[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool upsertUnboundDevice(const String& id, int battMv) {
    const int existing = findUnboundDevice(id);
    if (existing >= 0) {
        // 已存在的未绑定设备只刷新电量和在线时间，不改变表中位置，网页显示更稳定。
        unboundDevices[existing].lastBattMv = battMv;
        unboundDevices[existing].lastSeenMs = millis();
        return true;
    }

    for (uint8_t i = 0; i < MAX_UNBOUND_DEVICES; i++) {
        if (unboundDevices[i].id.length() == 0) {
            // 找到第一个空槽插入；表满时返回 false，由调用方打印提示。
            unboundDevices[i].id = id;
            unboundDevices[i].lastBattMv = battMv;
            unboundDevices[i].lastSeenMs = millis();
            return true;
        }
    }
    return false;
}

bool removeUnboundDevice(const String& id) {
    const int idx = findUnboundDevice(id);
    if (idx < 0) {
        return false;
    }
    unboundDevices[idx] = UnboundDevice();
    return true;
}

void printUnboundDevices() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_UNBOUND_DEVICES; i++) {
        if (unboundDevices[i].id.length() > 0) {
            count++;
        }
    }

    Serial.print("Unbound devices (");
    Serial.print(count);
    Serial.println("):");

    const unsigned long now = millis();
    for (uint8_t i = 0; i < MAX_UNBOUND_DEVICES; i++) {
        if (unboundDevices[i].id.length() == 0) {
            continue;
        }
        Serial.print("  [");
        Serial.print(i);
        Serial.print("] ");
        Serial.print(unboundDevices[i].id);
        Serial.print("  batt=");
        Serial.print(unboundDevices[i].lastBattMv);
        Serial.print("mV  age=");
        Serial.print(now - unboundDevices[i].lastSeenMs);
        Serial.println("ms");
    }
}

int bindingSlotIndex(const String& name) {
    for (uint8_t i = 0; i < BINDING_SLOT_COUNT; i++) {
        if (name == BINDING_SLOT_NAMES[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int findBindingByDevice(const String& id) {
    for (uint8_t i = 0; i < BINDING_SLOT_COUNT; i++) {
        if (bindings[i].length() > 0 && bindings[i] == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void loadBindingsFromNvs() {
    // false 表示读写模式；后续绑定/队名/总比分变化会继续写入同一个命名空间。
    prefs.begin(NVS_NAMESPACE, false);
    for (uint8_t i = 0; i < BINDING_SLOT_COUNT; i++) {
        bindings[i] = prefs.getString(NVS_BINDING_KEYS[i], "");
    }
    for (uint8_t i = 0; i < TEAM_COUNT; i++) {
        // 队名默认值使用全局初始化的“红方/蓝方”；NVS 没有值时保留默认。
        teamNames[i] = prefs.getString(NVS_TEAM_KEYS[i], teamNames[i]);
        totalScores[i] = prefs.getInt(NVS_TOTAL_KEYS[i], 0);
    }
}

void saveBindingSlot(uint8_t slot, const String& deviceId) {
    bindings[slot] = deviceId;
    if (deviceId.length() == 0) {
        // 解绑时删除 NVS key，重启后该槽位仍为空。
        prefs.remove(NVS_BINDING_KEYS[slot]);
    } else {
        // 绑定时持久化 deviceId，裁判端重启后仍能被服务端纠正/识别。
        prefs.putString(NVS_BINDING_KEYS[slot], deviceId);
    }
}

void printBindings() {
    Serial.println("Bindings:");
    for (uint8_t i = 0; i < BINDING_SLOT_COUNT; i++) {
        Serial.print("  ");
        Serial.print(BINDING_SLOT_NAMES[i]);
        Serial.print(": ");
        if (bindings[i].length() == 0) {
            Serial.println("<unbound>");
        } else {
            Serial.println(bindings[i]);
        }
    }
}

void setTeamNames(const String& team0, const String& team1) {
    // 空字符串回落到默认队名，避免显示页出现空白队伍名。
    teamNames[0] = team0.length() > 0 ? team0 : String("红方");
    teamNames[1] = team1.length() > 0 ? team1 : String("蓝方");
    saveTeamNamesToNvs();
}

void resetMatchScores() {
    // reset 只清比赛进度和总比分，绑定关系、未绑定设备表、队名都保留。
    resetTotalScores();
    currentRoundId = 1;
    roundOpen = true;
    roundScoreApplied = false;
    resetRoundSubmissions();
    stopCountdown();
}

void resetTotalScores() {
    // 下一轮可以只重置显示页/控制页的总比分，不影响当前轮号和裁判绑定关系。
    totalScores[0] = 0;
    totalScores[1] = 0;
    saveTotalsToNvs();
}

bool applyCompletedRoundScoreIfReady() {
    if (roundScoreApplied) {
        // 幂等保护：收到重复 SUBMIT 或手动 next-round 前再次调用时不会重复加总分。
        return false;
    }

    int redScores[BINDING_SLOT_COUNT];
    int blueScores[BINDING_SLOT_COUNT];
    uint8_t requiredCount = 0;

    for (uint8_t i = 0; i < BINDING_SLOT_COUNT; i++) {
        if (bindings[i].length() == 0) {
            // 未绑定槽位不参与“本轮是否完成”的要求，方便单裁判/双裁判测试。
            continue;
        }
        if (!roundSubmissions[i].submitted) {
            // 只要有一个已绑定裁判还没提交，本轮就不能结算。
            return false;
        }
        // 收集已绑定裁判的红蓝分，后面按 1/2/3 人规则分别计算最终值。
        redScores[requiredCount] = roundSubmissions[i].red;
        blueScores[requiredCount] = roundSubmissions[i].blue;
        requiredCount++;
    }

    if (requiredCount == 0) {
        // 没有任何绑定裁判时不自动结算，避免空轮次把 roundOpen 关掉。
        return false;
    }

    // 本轮结算成功后，把计算出的轮分累加进整场总比分。
    totalScores[0] += finalScoreFromSubmitted(redScores, requiredCount);
    totalScores[1] += finalScoreFromSubmitted(blueScores, requiredCount);
    roundScoreApplied = true;
    // 结算后关闭本轮，下一轮由 next-round 按钮/命令显式开启。
    roundOpen = false;
    saveTotalsToNvs();

    Serial.print("round complete: total ");
    Serial.print(teamNames[0]);
    Serial.print("=");
    Serial.print(totalScores[0]);
    Serial.print(" ");
    Serial.print(teamNames[1]);
    Serial.print("=");
    Serial.println(totalScores[1]);
    return true;
}

void startCountdown(uint32_t seconds) {
    countdownDurationSec = seconds;
    // seconds=0 表示不启动倒计时，显示页会显示 --:--。
    countdownActive = seconds > 0;
    // 记录绝对结束时间，网页每秒刷新时通过 countdownRemainingSec 计算剩余秒数。
    countdownEndMs = millis() + seconds * 1000UL;
}

void stopCountdown() {
    countdownActive = false;
    countdownDurationSec = 0;
    countdownEndMs = 0;
}

bool isCountdownActive() {
    return countdownActive;
}

uint32_t countdownRemainingSec() {
    if (!countdownActive) {
        return 0;
    }
    // 用有符号差值处理 millis 回绕和倒计时已经结束的情况。
    const long remainingMs = static_cast<long>(countdownEndMs - millis());
    if (remainingMs <= 0) {
        return 0;
    }
    // 向上取整，剩 1..999ms 时仍显示 1 秒，直到真正结束才显示 0。
    return static_cast<uint32_t>((remainingMs + 999L) / 1000L);
}

bool parseBatteryMv(const String& battText, int& battMv) {
    unsigned long parsedBatt = 0;
    if (ScoreProtocol::parseUnsignedLong(battText, parsedBatt) && parsedBatt <= 5000UL) {
        // 简单限定 0..5000mV，过滤乱码或异常协议字段。
        battMv = static_cast<int>(parsedBatt);
        return true;
    }
    // 解析失败时置 0，网页会显示 "--"，不误报具体电压。
    battMv = 0;
    return false;
}

void updateJudgeStatus(uint8_t slot, int battMv) {
    if (slot >= BINDING_SLOT_COUNT) {
        // 防御非法下标，避免协议异常时写越界。
        return;
    }
    judgeStatuses[slot].seen = true;
    judgeStatuses[slot].lastBattMv = battMv;
    judgeStatuses[slot].lastSeenMs = millis();
}

uint8_t countSubmittedJudges() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < BINDING_SLOT_COUNT; i++) {
        if (roundSubmissions[i].submitted) {
            count++;
        }
    }
    return count;
}

void resetRoundSubmissions() {
    for (uint8_t i = 0; i < BINDING_SLOT_COUNT; i++) {
        // 赋默认构造对象可以一次性清 submitted/msgId/red/blue。
        roundSubmissions[i] = PerJudgeSubmission();
    }
}

void printRoundState() {
    Serial.print("Round: ");
    Serial.print(currentRoundId);
    Serial.print("  open=");
    Serial.println(roundOpen ? "true" : "false");
    for (uint8_t i = 0; i < BINDING_SLOT_COUNT; i++) {
        Serial.print("  ");
        Serial.print(BINDING_SLOT_NAMES[i]);
        Serial.print(": ");
        if (!roundSubmissions[i].submitted) {
            Serial.println("<pending>");
        } else {
            Serial.print("red=");
            Serial.print(roundSubmissions[i].red);
            Serial.print(" blue=");
            Serial.print(roundSubmissions[i].blue);
            Serial.print(" msgId=");
            Serial.println(roundSubmissions[i].lastMsgId);
        }
    }
}
