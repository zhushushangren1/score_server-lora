#include "ServerWeb.h"

#include <Arduino.h>

#include "ServerActions.h"
#include "ServerState.h"
#include "WebUi.h"

namespace {

void buildWebUiState(WebUiState& state) {
    // 把业务层全局状态复制成 WebUiState 快照，网页渲染只读这份数据。
    state.currentRoundId = currentRoundId;
    state.roundOpen = roundOpen;
    state.teamNames[0] = teamNames[0];
    state.teamNames[1] = teamNames[1];
    state.totalScores[0] = totalScores[0];
    state.totalScores[1] = totalScores[1];
    state.countdownActive = isCountdownActive();
    state.countdownRemainingSec = countdownRemainingSec();
    state.submittedCount = countSubmittedJudges();

    // 目前业务层固定 3 个裁判位，但这里仍按 WebUi 数组容量做截断保护。
    const uint8_t judgeCount = BINDING_SLOT_COUNT < WEB_UI_MAX_JUDGES
        ? BINDING_SLOT_COUNT
        : WEB_UI_MAX_JUDGES;
    state.judgeCount = judgeCount;

    for (uint8_t i = 0; i < judgeCount; i++) {
        WebUiJudge& judge = state.judges[i];
        // i 同时索引 BINDING_SLOT_NAMES、bindings、judgeStatuses、roundSubmissions。
        judge.clientId = BINDING_SLOT_NAMES[i];
        judge.deviceId = bindings[i];
        judge.bound = bindings[i].length() > 0;
        judge.seen = judgeStatuses[i].seen;
        judge.lastBattMv = judgeStatuses[i].lastBattMv;
        judge.lastSeenMs = judgeStatuses[i].lastSeenMs;
        judge.submitted = roundSubmissions[i].submitted;
        judge.red = roundSubmissions[i].red;
        judge.blue = roundSubmissions[i].blue;
    }

    state.unboundCount = 0;
    for (uint8_t i = 0; i < MAX_UNBOUND_DEVICES &&
                        state.unboundCount < WEB_UI_MAX_UNBOUND_DEVICES; i++) {
        if (unboundDevices[i].id.length() == 0) {
            // 空槽跳过，WebUiState 中的未绑定设备数组保持紧凑排列。
            continue;
        }
        WebUiUnboundDevice& device = state.unboundDevices[state.unboundCount++];
        device.deviceId = unboundDevices[i].id;
        device.lastBattMv = unboundDevices[i].lastBattMv;
        device.lastSeenMs = unboundDevices[i].lastSeenMs;
    }
}

void webUiBindAction(const String& deviceId, const String& clientId) {
    // Web 表单动作复用串口命令处理函数，保证两种入口的校验和日志一致。
    String args[] = {deviceId, clientId};
    handleBindCommand(args, 2);
}

void webUiUnbindAction(const String& clientId) {
    // 解绑同样复用 ServerActions，避免网页和串口出现不同规则。
    String args[] = {clientId};
    handleUnbindCommand(args, 1);
}

void webUiNextRoundAction(uint32_t countdownSeconds) {
    // 网页能传倒计时秒数；实体按钮和无参数串口命令走 handleNextRoundCommand() 默认 0 秒。
    handleNextRoundCommand(countdownSeconds);
}

void webUiSetTeamNamesAction(const String& team0, const String& team1) {
    // 队名持久化在 ServerState 中完成，Web 层只传递用户输入。
    setTeamNames(team0, team1);
}

}  // namespace

void setupServerWeb() {
    WebUiActions webActions;
    // 将 WebUi 的抽象动作绑定到服务端业务函数。
    webActions.bind = webUiBindAction;
    webActions.unbind = webUiUnbindAction;
    webActions.nextRound = webUiNextRoundAction;
    webActions.reset = handleResetCommand;
    webActions.setTeamNames = webUiSetTeamNamesAction;
    // buildWebUiState 是状态提供者，webActions 是动作回调集合。
    setupWebUi(buildWebUiState, webActions);
}

void handleServerWebClient() {
    // main loop 调用的薄封装，保持 main.cpp 不依赖 WebUi.cpp 细节。
    handleWebUiClient();
}
