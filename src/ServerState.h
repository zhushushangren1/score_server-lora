// 服务端全局状态声明。
// 定义绑定槽位、裁判提交记录、在线状态、队名/总比分和倒计时相关接口。
#pragma once

#include <Arduino.h>

constexpr uint8_t MAX_UNBOUND_DEVICES = 6;
constexpr uint8_t BINDING_SLOT_COUNT = 3;
constexpr uint8_t TEAM_COUNT = 2;

// 未绑定设备记录。
// id：裁判端自报的 deviceId，来自 MAC 低 32 位。
// lastBattMv：最近一次 HELLO/HEARTBEAT 上报的电池电压，单位毫伏。
// lastSeenMs：最近一次收到该设备存在信号时的 millis()，用于网页计算在线/离线。
struct UnboundDevice {
    String id;
    int lastBattMv = 0;
    unsigned long lastSeenMs = 0;
};

// 单个裁判位在当前轮的提交记录。
// submitted：本轮是否已接受过该裁判位的 SUBMIT。
// lastMsgId：最近一次被接受的 SUBMIT msgId，用于重复包幂等处理。
// red/blue：该裁判提交的红蓝分数，范围 0..99。
struct PerJudgeSubmission {
    bool submitted = false;
    unsigned long lastMsgId = 0;
    int red = 0;
    int blue = 0;
};

// 已绑定裁判的在线状态。
// seen：服务端启动后是否收到过该槽位裁判的 HELLO/HEARTBEAT/SUBMIT。
// lastBattMv：最近一次上报电池电压。
// lastSeenMs：最近一次收到有效消息的 millis()。
struct JudgeStatus {
    bool seen = false;
    int lastBattMv = 0;
    unsigned long lastSeenMs = 0;
};

extern UnboundDevice unboundDevices[MAX_UNBOUND_DEVICES];
extern const char* const BINDING_SLOT_NAMES[BINDING_SLOT_COUNT];

// bindings[i] 保存 BINDING_SLOT_NAMES[i] 当前绑定的 deviceId；空字符串表示未绑定。
extern String bindings[BINDING_SLOT_COUNT];

// 队伍名称和总比分。teamNames[0]/totalScores[0] 对应红方，teamNames[1]/totalScores[1] 对应蓝方。
extern String teamNames[TEAM_COUNT];
extern int totalScores[TEAM_COUNT];

// 当前轮号、收分状态、以及本轮总分是否已经累计到 totalScores。
extern uint32_t currentRoundId;
extern bool roundOpen;
extern bool roundScoreApplied;

// 当前轮的裁判提交表和已绑定裁判在线状态表，下标均与 BINDING_SLOT_NAMES/bindings 一致。
extern PerJudgeSubmission roundSubmissions[BINDING_SLOT_COUNT];
extern JudgeStatus judgeStatuses[BINDING_SLOT_COUNT];

// 在未绑定设备表中查找 deviceId。
// 返回：找到则为数组下标；未找到为 -1。
int findUnboundDevice(const String& id);

// 插入或刷新未绑定设备。
// id：设备号；battMv：本次上报电池电压。
// 返回：true=成功；false=表满且无法插入新设备。
bool upsertUnboundDevice(const String& id, int battMv);

// 从未绑定表删除 deviceId。绑定成功后调用，避免控制页继续显示已绑定设备。
bool removeUnboundDevice(const String& id);

// 打印未绑定设备列表到串口。
void printUnboundDevices();

// 把 client1/client2/client3 转为 0/1/2 下标。非法字符串返回 -1。
int bindingSlotIndex(const String& name);

// 查找某个 deviceId 当前绑定在哪个槽位。未绑定返回 -1。
int findBindingByDevice(const String& id);

// 从 NVS 加载绑定表、队名、总比分。setup() 中调用一次。
void loadBindingsFromNvs();

// 保存单个绑定槽位到 NVS，并同步内存 bindings[]。
// slot：0..2；deviceId：新绑定设备号，空字符串表示清空。
void saveBindingSlot(uint8_t slot, const String& deviceId);

// 打印绑定表到串口。
void printBindings();

// 修改队名并写入 NVS。空字符串会回落到默认“红方/蓝方”。
void setTeamNames(const String& team0, const String& team1);

// 清空总比分和当前轮状态，不清绑定表和队名。
void resetMatchScores();

// 若所有已绑定裁判都提交，则按计分规则累计到总比分，并把 roundOpen 置为 false。
// 返回：true=本次调用完成了累计；false=条件不满足或已经累计过。
bool applyCompletedRoundScoreIfReady();

// 启动/停止/查询网页显示倒计时。
// seconds：倒计时秒数；0 表示不启动。
void startCountdown(uint32_t seconds);
void stopCountdown();
bool isCountdownActive();
uint32_t countdownRemainingSec();

// 解析协议中的电池电压字段。
// battText：文本毫伏值；battMv：输出毫伏值，解析失败时为 0。
bool parseBatteryMv(const String& battText, int& battMv);

// 刷新某个裁判槽位的在线/电量状态。
void updateJudgeStatus(uint8_t slot, int battMv);

// 统计当前轮已经提交的裁判数量。
uint8_t countSubmittedJudges();

// 清空当前轮提交表。
void resetRoundSubmissions();

// 打印当前轮号、开放状态和各裁判提交状态。
void printRoundState();
