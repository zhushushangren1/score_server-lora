#pragma once

#include <Arduino.h>

constexpr uint8_t WEB_UI_MAX_JUDGES = 3;
constexpr uint8_t WEB_UI_MAX_UNBOUND_DEVICES = 6;

// Web 层使用的单个裁判显示模型。
// 该结构只面向页面渲染，不直接持有 ServerState 的数组引用，避免网页层修改业务状态。
struct WebUiJudge {
    String clientId;
    String deviceId;
    bool bound = false;
    bool seen = false;
    int lastBattMv = 0;
    unsigned long lastSeenMs = 0;
    bool submitted = false;
    int red = 0;
    int blue = 0;
};

// Web 层使用的未绑定设备显示模型。
struct WebUiUnboundDevice {
    String deviceId;
    int lastBattMv = 0;
    unsigned long lastSeenMs = 0;
};

// Web 页面每次渲染时的一次性状态快照。
// currentRoundId/roundOpen/submittedCount：比赛当前轮摘要。
// teamNames/totalScores：显示页核心比分数据。
// countdownActive/countdownRemainingSec：下一轮倒计时状态。
// judges/unboundDevices：控制页用于绑定、解绑、观察在线状态。
struct WebUiState {
    uint32_t currentRoundId = 1;
    bool roundOpen = true;
    String teamNames[2];
    int totalScores[2] = {0, 0};
    bool countdownActive = false;
    uint32_t countdownRemainingSec = 0;
    uint8_t submittedCount = 0;
    uint8_t judgeCount = 0;
    WebUiJudge judges[WEB_UI_MAX_JUDGES];
    uint8_t unboundCount = 0;
    WebUiUnboundDevice unboundDevices[WEB_UI_MAX_UNBOUND_DEVICES];
};

// WebUi 向业务层拉取页面状态的回调。
// 参数 state：由调用方填充；WebUi 只读取，不修改业务全局变量。
using WebUiStateProvider = void (*)(WebUiState& state);

// WebUi 可触发的业务动作集合。
// bind/unbind：控制页裁判绑定动作。
// nextRound：控制页下一轮动作，参数为倒计时秒数。
// reset：重置总比分和当前轮。
// setTeamNames：保存两队名称。
struct WebUiActions {
    void (*bind)(const String& deviceId, const String& clientId) = nullptr;
    void (*unbind)(const String& clientId) = nullptr;
    void (*nextRound)(uint32_t countdownSeconds) = nullptr;
    void (*reset)() = nullptr;
    void (*setTeamNames)(const String& team0, const String& team1) = nullptr;
};

// 启动 WiFi AP 和 HTTP 路由。
// provider：页面状态提供者；actions：页面按钮对应的业务回调。
void setupWebUi(WebUiStateProvider provider, const WebUiActions& actions);

// 处理 WebServer 请求。loop 中反复调用。
void handleWebUiClient();
