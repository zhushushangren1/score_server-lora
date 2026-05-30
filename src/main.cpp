#include <Arduino.h>
#include <Preferences.h>
#include <ScoreProtocol.h>
#include <TM1637Display.h>

// 服务端 E22-400T22D 串口接线（与裁判端一致，改到 ESP32-S3 DevKitC 右侧连续排针）：
// ESP32-S3 GPIO41 TX -> E22 RXD
// ESP32-S3 GPIO40 RX <- E22 TXD
// ESP32-S3 GPIO42     <- E22 AUX
// ESP32-S3 GPIO38     -> E22 M0
// ESP32-S3 GPIO39     -> E22 M1
//
// M0=LOW 且 M1=LOW 时，E22 进入普通透明传输模式。
// E22 模块出厂串口波特率通常是 9600。
constexpr int LORA_TX_PIN = 41;
constexpr int LORA_RX_PIN = 40;
constexpr int LORA_AUX_PIN = 42;
constexpr int LORA_M0_PIN = 38;
constexpr int LORA_M1_PIN = 39;
constexpr uint32_t LORA_UART_BAUD = 9600;

// TM1637 数码管接线（与裁判端一致）：CLK -> GPIO48，DIO -> GPIO47。
// 避开 GPIO35~GPIO37：N16R8 等带 OPI PSRAM 的板卡用这三个脚连八线 PSRAM，不能复用。
// 服务端用它来显示当前比赛进度："轮号.已提交数" 例如 "01.00" / "02.03"。
constexpr int TM1637_CLK_PIN = 48;
constexpr int TM1637_DIO_PIN = 47;
TM1637Display display(TM1637_CLK_PIN, TM1637_DIO_PIN);

// 显示状态最低刷新间隔。状态变化点会主动调 updateDisplay，这里只是兜底。
constexpr unsigned long DISPLAY_REFRESH_INTERVAL_MS = 200;
unsigned long lastDisplayRefreshMs = 0;

// LoRa 接收行缓冲，handleLoraInput 按字节追加，遇到 '\n' 即视为一帧结束。
String loraLine;

// 未绑定设备列表上限。
// 第一版业务最多 3 台裁判机，外加几台不属于本场比赛的同频设备，6 个槽位足够。
// 设计成定长数组而非动态容器：避免 String 的堆碎片，行为可预测。
constexpr uint8_t MAX_UNBOUND_DEVICES = 6;

// 单条未绑定设备记录。
// id：裁判机自报的 deviceId（来自 MAC，8 位 hex）。
// lastBattMv：该设备最近一次 HELLO/HEARTBEAT 上报的电池电压（毫伏）。
// lastSeenMs：最近一次收到该设备消息时的 millis() 值，用于后续判断在线/离线。
// 空槽位以 id.length() == 0 表示。
struct UnboundDevice {
    String id;
    int lastBattMv = 0;
    unsigned long lastSeenMs = 0;
};

// 未绑定设备表，所有 HELLO 中自报 clientId == "UNASSIGNED" 的设备都会登记到这里。
// 后续步骤会在网页/串口下发 ASSIGN 时从这张表里选目标。
UnboundDevice unboundDevices[MAX_UNBOUND_DEVICES];

// 在未绑定设备表中查找指定 deviceId 的槽位下标。
// id：要查找的 deviceId。
// 返回：找到则返回 0..MAX_UNBOUND_DEVICES-1 的下标；找不到返回 -1。
int findUnboundDevice(const String& id) {
    for (uint8_t i = 0; i < MAX_UNBOUND_DEVICES; i++) {
        if (unboundDevices[i].id.length() > 0 && unboundDevices[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 把一台未绑定设备的最近信息写入表，已存在则更新（电压/最近活跃时间），不存在则插入到第一个空槽位。
// id：deviceId（必须非空）。
// battMv：本次 HELLO/HEARTBEAT 携带的电池电压。
// 返回：true=登记/更新成功；false=表已满（达到 MAX_UNBOUND_DEVICES 上限，新设备被丢弃，会在调试串口给出提示）。
// 说明：第一版暂不做槽位老化淘汰，依赖后续 ASSIGN 之后从表中移除来腾位置。
bool upsertUnboundDevice(const String& id, int battMv) {
    const int existing = findUnboundDevice(id);
    if (existing >= 0) {
        unboundDevices[existing].lastBattMv = battMv;
        unboundDevices[existing].lastSeenMs = millis();
        return true;
    }

    for (uint8_t i = 0; i < MAX_UNBOUND_DEVICES; i++) {
        if (unboundDevices[i].id.length() == 0) {
            unboundDevices[i].id = id;
            unboundDevices[i].lastBattMv = battMv;
            unboundDevices[i].lastSeenMs = millis();
            return true;
        }
    }
    return false;
}

// 把当前未绑定设备表打印到调试串口，供联调阶段直接观察。
// 输出形式示例：
//   Unbound devices (2):
//     [0] A1B2C3D4  batt=3820mV  age=1234ms
//     [1] F09B12AC  batt=3790mV  age=5678ms
// 设计文档 12 节"未绑定设备列表"页面后续会用到同一份数据，先用串口验证。
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

// 从未绑定设备表中移除指定 deviceId（绑定成功后调用，腾出槽位给别的新设备）。
// id：要移除的 deviceId。
// 返回：true=找到并清空；false=表中没有该 id。
bool removeUnboundDevice(const String& id) {
    const int idx = findUnboundDevice(id);
    if (idx < 0) {
        return false;
    }
    unboundDevices[idx].id = "";
    unboundDevices[idx].lastBattMv = 0;
    unboundDevices[idx].lastSeenMs = 0;
    return true;
}

// 绑定槽位数量。设计文档固定 3 个裁判位（client1/client2/client3）。
// 写成常量而非魔数，方便后续若改成 5 裁判位也只动一处。
constexpr uint8_t BINDING_SLOT_COUNT = 3;

// 各槽位字符串名，下标 0/1/2 ↔ client1/client2/client3。
const char* const BINDING_SLOT_NAMES[BINDING_SLOT_COUNT] = {
    "client1", "client2", "client3"
};

// NVS 命名空间。Preferences 库要求命名空间名 ≤15 字节。
constexpr const char* NVS_NAMESPACE = "score_server";

// 三个绑定槽位对应的 NVS 键名。键名长度也限制 ≤15 字节。
const char* const NVS_BINDING_KEYS[BINDING_SLOT_COUNT] = {
    "bind_client1", "bind_client2", "bind_client3"
};

// 绑定表内存副本。bindings[i] 存储槽位 i 当前绑定的 deviceId；空字符串表示该槽位未绑定。
// 真值来源是 NVS；setup() 启动时加载，所有更新都通过 saveBindingSlot 保持内存与 NVS 同步。
String bindings[BINDING_SLOT_COUNT];

// 全局 Preferences 实例，setup() 中 begin 一次后长期持有，避免每次写入都付出 begin/end 开销。
Preferences prefs;

// 把字符串 "client1"/"client2"/"client3" 转成槽位下标 0/1/2。
// name：待识别字符串。
// 返回：合法名返回 0..BINDING_SLOT_COUNT-1；不识别返回 -1。
int bindingSlotIndex(const String& name) {
    for (uint8_t i = 0; i < BINDING_SLOT_COUNT; i++) {
        if (name == BINDING_SLOT_NAMES[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 查找当前哪个槽位绑定了指定 deviceId。
// id：要查找的 deviceId。
// 返回：找到返回 0..2；未找到返回 -1。
// 用途：bind 命令时检查 deviceId 是否已经绑定到别的槽位（防止一个设备占两个位置）。
int findBindingByDevice(const String& id) {
    for (uint8_t i = 0; i < BINDING_SLOT_COUNT; i++) {
        if (bindings[i].length() > 0 && bindings[i] == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 从 NVS 加载三个绑定槽位到 bindings 数组。
// 调用时机：setup() 一次。
// 副作用：调用 prefs.begin()，之后 prefs 在整个程序生命周期内保持打开。
void loadBindingsFromNvs() {
    prefs.begin(NVS_NAMESPACE, false);
    for (uint8_t i = 0; i < BINDING_SLOT_COUNT; i++) {
        bindings[i] = prefs.getString(NVS_BINDING_KEYS[i], "");
    }
}

// 更新指定槽位的绑定 deviceId（同步写 NVS 和内存）。
// slot：0..BINDING_SLOT_COUNT-1，调用方需自行保证合法。
// deviceId：新绑定的 deviceId；传空字符串表示清除该槽位。
// 副作用：写 NVS（耗费 flash 寿命），仅在绑定关系真正变化时调用。
void saveBindingSlot(uint8_t slot, const String& deviceId) {
    bindings[slot] = deviceId;
    if (deviceId.length() == 0) {
        prefs.remove(NVS_BINDING_KEYS[slot]);
    } else {
        prefs.putString(NVS_BINDING_KEYS[slot], deviceId);
    }
}

// 把当前三个绑定槽位打印到串口。
// 用于 list 命令和启动时让操作员一眼看到绑定状态。
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

// 每位裁判在本轮的提交记录。
// submitted：是否已经成功提交。
// lastMsgId：最近一次被接受的 SUBMIT 帧的 msgId，用于 deviceId+roundId+msgId 去重。
//   - 同一 msgId 的重复 SUBMIT → 回 OK_DUPLICATE。
//   - 同一裁判同一轮的不同 msgId → 回 ERR_ALREADY_SUBMITTED（设计文档第 6 节规则）。
// red/blue：被接受的本轮分数（0..99），供后续步骤 14 合分使用。
struct PerJudgeSubmission {
    bool submitted = false;
    unsigned long lastMsgId = 0;
    int red = 0;
    int blue = 0;
};

// 当前轮号。从 1 开始，每次 next-round 命令递增。第一版只在内存里，重启会归 1。
// NVS 持久化交给设计文档步骤 13。
uint32_t currentRoundId = 1;

// 当前轮是否仍在收分阶段。
// 第一版即使 3 名裁判全部提交也不会自动置 false，因为合分/广播属于步骤 14；
// 这里保持 true，仅为 STATUS 字段提供合理值。
bool roundOpen = true;

// 三个槽位的本轮提交记录，下标与 bindings[] 一一对应。
PerJudgeSubmission roundSubmissions[BINDING_SLOT_COUNT];

// 清空所有槽位的本轮提交记录。
// 调用时机：next-round 命令；后续 reset 也会用到。
void resetRoundSubmissions() {
    for (uint8_t i = 0; i < BINDING_SLOT_COUNT; i++) {
        roundSubmissions[i] = PerJudgeSubmission();
    }
}

// 打印当前轮号、是否开放、以及三位裁判的提交状态。
// 用于启动横幅、list 命令、每次 SUBMIT 处理后给操作员一份状态快照。
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

// 统计本轮已经提交的裁判数。0..BINDING_SLOT_COUNT。
// 用于在数码管上显示提交进度，让操作员一眼能看出还差几个人。
uint8_t countSubmittedJudges() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < BINDING_SLOT_COUNT; i++) {
        if (roundSubmissions[i].submitted) {
            n++;
        }
    }
    return n;
}

// 把当前轮号与已提交人数显示到 TM1637。
// 格式 "RR.NN"：左两位 = currentRoundId（钳到 0..99 显示），右两位 = 已提交人数。
// 与裁判端的 "red.blue" 显示形式对称，操作员视觉上容易区分。
// 由 setup / handleSubmit / handleNextRoundCommand 等状态变化点主动调用，loop 也兜底周期刷新。
void updateDisplay() {
    lastDisplayRefreshMs = millis();
    const int round = currentRoundId > 99 ? 99 : static_cast<int>(currentRoundId);
    display.showScore(round, countSubmittedJudges());
}

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

// 组装并发送一帧 STATUS，作为对裁判机 HELLO 的应答。
// 帧格式：STATUS,deviceId,clientId,roundId,roundOpen,submitted,crc16。
// deviceId：从收到的 HELLO 取来，原样回填，便于裁判机识别这是发给自己的应答。
// clientId：裁判机当前自报的 ID（"UNASSIGNED" 或 "client1/2/3"）。
// roundId / roundOpen 取自全局 currentRoundId / roundOpen；submitted 反映对应槽位本轮是否已提交。
// 未绑定设备（clientId == "UNASSIGNED"）不属于任何槽位，submitted 始终回 0。
void sendStatus(const String& deviceId, const String& clientId) {
    int submitted = 0;
    const int slot = bindingSlotIndex(clientId);
    if (slot >= 0 && roundSubmissions[slot].submitted) {
        submitted = 1;
    }

    String fields[] = {
        "STATUS",
        deviceId,
        clientId,
        String(currentRoundId),
        roundOpen ? "1" : "0",
        submitted ? "1" : "0"
    };

    sendLoraLine(ScoreProtocol::buildFrame(fields, 6));
}

// 组装并发送一帧 ASSIGN，要求目标裁判机绑定为指定 clientId。
// 帧格式：ASSIGN,deviceId,clientId,crc16。
// deviceId：目标裁判机 deviceId（来自未绑定表或网页选择）。
// clientId：要分配的槽位名，"client1" / "client2" / "client3"。
// 第一版只发一次，不重传；丢包重传留给设计文档步骤 7 实现。
void sendAssign(const String& deviceId, const String& clientId) {
    String fields[] = {
        "ASSIGN",
        deviceId,
        clientId
    };
    sendLoraLine(ScoreProtocol::buildFrame(fields, 3));
}

// 组装并发送一帧 UNBIND，通知目标裁判机清除本地绑定。
// 帧格式：UNBIND,deviceId,crc16。
// 第一版只发一次，不重传。
void sendUnbind(const String& deviceId) {
    String fields[] = {
        "UNBIND",
        deviceId
    };
    sendLoraLine(ScoreProtocol::buildFrame(fields, 2));
}

// 组装并发送一帧 ACK，作为对 SUBMIT 的应答。
// 帧格式：ACK,deviceId,clientId,roundId,msgId,status,crc16。
// deviceId / clientId / roundId / msgId：均原样回填，让裁判机能精确匹配回到挂起的提交。
// status：协议规定的结果码：
//   OK                    - 首次接受，已记录分数
//   OK_DUPLICATE          - 相同 deviceId+roundId+msgId 已见过，幂等再 ACK，不重复计分
//   ERR_ALREADY_SUBMITTED - 同一裁判同一轮已用不同 msgId 提交过，新提交被拒
//   ERR_BAD_ROUND         - roundId 与服务端当前轮不一致
// 裁判机端把前三种都视为"已被服务端确认"进入 LOCKED；ERR_BAD_ROUND 则给操作员错误反馈。
void sendAck(const String& deviceId, const String& clientId,
             const String& roundIdText, const String& msgIdText,
             const char* status) {
    String fields[] = {
        "ACK",
        deviceId,
        clientId,
        roundIdText,
        msgIdText,
        String(status)
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

// 处理一帧 HELLO。
// frame：parseFrame 成功的帧，类型已经是 Hello。
// 行为：
//   - fieldCount != 4 → 打日志丢弃。
//   - 自报 clientId == "UNASSIGNED" → 解析电压、登记到未绑定表、回 STATUS。
//   - 自报 clientId == "client1/2/3" → 与绑定表比对：
//       匹配则正常回 STATUS；
//       不匹配（槽位未绑定，或绑给了别的 deviceId）→ 打日志，并向该设备回发一帧 UNBIND
//         以纠正客户端本地的过期 NVS。这同时也是 UNBIND 丢包的隐式重传机制：
//         只要客户端还冒充错误的 clientId，每次 HELLO 都会触发一次新的 UNBIND，迟早会送达。
// 处理一台裁判机刚刚自报存在的信号——HELLO 或 HEARTBEAT 共用的核心逻辑。
// deviceId / clientId / battText：来自帧字段；frameLabel 用于日志区分（"HELLO" / "HEARTBEAT"）。
// 行为（与 handleHello/handleHeartbeat 调用方共享）：
//   - 自报 UNASSIGNED 但服务端有该 deviceId 的绑定记录 → 重发 ASSIGN（隐式重传）。
//   - 自报 UNASSIGNED 且服务端也没绑定 → 解析电池电压，登记/刷新未绑定表，回 STATUS。
//   - 自报 client1/2/3 但与绑定表不一致或槽位不存在 → 重发 UNBIND 纠正客户端。
//   - 自报与绑定表一致 → 回 STATUS。
// 抽成独立函数避免 HEARTBEAT 实现时再写一遍同样的自愈逻辑。
void respondToPresence(const String& deviceId, const String& clientId,
                       const String& battText, const char* frameLabel) {
    if (clientId == "UNASSIGNED") {
        // 一种特殊情况：客户端自报 UNASSIGNED，但服务端记录里这个 deviceId 已经绑到了某 clientX。
        // 说明之前发出的 ASSIGN 在空中丢了，客户端从未真正绑定成功。
        // 服务端必须再发一遍 ASSIGN，否则两边状态会永远分裂（服务端以为绑了，客户端不知道）。
        const int existingSlot = findBindingByDevice(deviceId);
        if (existingSlot >= 0) {
            Serial.print(frameLabel);
            Serial.print(": ");
            Serial.print(deviceId);
            Serial.print(" claims UNASSIGNED but server has it bound to ");
            Serial.print(BINDING_SLOT_NAMES[existingSlot]);
            Serial.println(", resending ASSIGN");
            sendAssign(deviceId, BINDING_SLOT_NAMES[existingSlot]);
            return;
        }

        // 电池电压字段非法时仍然登记设备，但电压计 0。
        // 不能因为电压解析失败就丢掉整个设备，否则会错过绑定机会。
        int battMv = 0;
        unsigned long parsedBatt = 0;
        if (ScoreProtocol::parseUnsignedLong(battText, parsedBatt) && parsedBatt <= 5000UL) {
            battMv = static_cast<int>(parsedBatt);
        }

        if (!upsertUnboundDevice(deviceId, battMv)) {
            Serial.print("Unbound table full, dropped: ");
            Serial.println(deviceId);
        } else {
            printUnboundDevices();
        }
        sendStatus(deviceId, clientId);
        return;
    }

    // 自报为 client1/2/3 的设备：必须与绑定表一致才正常回应。
    const int slot = bindingSlotIndex(clientId);
    if (slot < 0) {
        Serial.print(frameLabel);
        Serial.print(": unknown clientId '");
        Serial.print(clientId);
        Serial.println("', sending UNBIND to correct");
        sendUnbind(deviceId);
        return;
    }

    if (bindings[slot] != deviceId) {
        Serial.print(frameLabel);
        Serial.print(": ");
        Serial.print(deviceId);
        Serial.print(" claims ");
        Serial.print(clientId);
        Serial.print(" but server has '");
        Serial.print(bindings[slot]);
        Serial.println("', sending UNBIND to correct");
        sendUnbind(deviceId);
        return;
    }

    // 绑定一致，按业务正常处理。
    sendStatus(deviceId, clientId);
}

// 处理一帧 HELLO。
// 帧格式：HELLO,deviceId,clientId,battMv,crc16，fieldCount 必须为 4。
// HELLO 语义：客户端"我来了"的一次性自报，通常在上电或重连时发，频率低。
// 后续保活由 HEARTBEAT 承担。
void handleHello(const ScoreProtocol::ParsedFrame& frame) {
    if (frame.fieldCount != 4) {
        Serial.println("HELLO: bad field count, ignored");
        return;
    }
    respondToPresence(frame.fields[1], frame.fields[2], frame.fields[3], "HELLO");
}

// 处理一帧 HEARTBEAT。
// 帧格式：HEARTBEAT,deviceId,clientId,battMv,msgId,crc16，fieldCount 必须为 5。
// HEARTBEAT 语义：周期性保活（设计文档第 9 节：未提交 10s 一次，已锁定 15s 一次）。
// 第一版服务端对 HEARTBEAT 与 HELLO 同等处理，回 STATUS 帮客户端同步轮次状态；
// 等步骤 14 引入 STATUS,ALL 广播后，服务端可以改为 HEARTBEAT 只更新最近活跃时间、不回 STATUS。
// msgId 字段当前只用于日志，服务端不维护 HEARTBEAT 的 msgId 去重。
void handleHeartbeat(const ScoreProtocol::ParsedFrame& frame) {
    if (frame.fieldCount != 5) {
        Serial.println("HEARTBEAT: bad field count, ignored");
        return;
    }
    respondToPresence(frame.fields[1], frame.fields[2], frame.fields[3], "HEARTBEAT");
}

// 处理一帧 SUBMIT。
// 帧格式：SUBMIT,deviceId,clientId,roundId,msgId,red,blue,battMv,crc16，fieldCount 必须为 8。
// 流程（设计文档第 6/8 节）：
//   1) 字段数错 / 字段非法 → 丢弃，不回 ACK（让客户端按超时重传）。
//   2) clientId 非法 → 不回 ACK，并发 UNBIND 纠正客户端本地状态。
//   3) clientId 合法但绑定不匹配 → 同上，发 UNBIND 纠正。
//   4) red/blue 越界 0..99 → 丢弃。
//   5) roundId != currentRoundId → ACK ERR_BAD_ROUND。
//   6) 已记录过相同 msgId → ACK OK_DUPLICATE（幂等，不重复计分）。
//   7) 本轮已用不同 msgId 提交过 → ACK ERR_ALREADY_SUBMITTED。
//   8) 全部通过 → 写入 roundSubmissions[slot]、ACK OK、打印轮次状态。
// 注意：每个槽位独立去重，不影响其他裁判。
void handleSubmit(const ScoreProtocol::ParsedFrame& frame) {
    if (frame.fieldCount != 8) {
        Serial.println("SUBMIT: bad field count, ignored");
        return;
    }

    const String& subDevice = frame.fields[1];
    const String& subClient = frame.fields[2];
    const String& subRoundText = frame.fields[3];
    const String& subMsgText = frame.fields[4];
    const String& subRedText = frame.fields[5];
    const String& subBlueText = frame.fields[6];
    // frame.fields[7] 是 battMv，本步骤暂不使用（步骤 10 接 ADC 后会用作裁判电量上报）。

    // ===== 绑定校验 =====
    const int slot = bindingSlotIndex(subClient);
    if (slot < 0) {
        Serial.print("SUBMIT: unknown clientId '");
        Serial.print(subClient);
        Serial.println("', sending UNBIND to correct");
        sendUnbind(subDevice);
        return;
    }
    if (bindings[slot] != subDevice) {
        Serial.print("SUBMIT: ");
        Serial.print(subDevice);
        Serial.print(" claims ");
        Serial.print(subClient);
        Serial.print(" but server has '");
        Serial.print(bindings[slot]);
        Serial.println("', sending UNBIND to correct");
        sendUnbind(subDevice);
        return;
    }

    // ===== 字段解析 =====
    unsigned long subRound = 0;
    if (!ScoreProtocol::parseUnsignedLong(subRoundText, subRound)) {
        Serial.println("SUBMIT: bad roundId, ignored");
        return;
    }
    unsigned long subMsgId = 0;
    if (!ScoreProtocol::parseUnsignedLong(subMsgText, subMsgId)) {
        Serial.println("SUBMIT: bad msgId, ignored");
        return;
    }
    int red = 0;
    int blue = 0;
    if (!ScoreProtocol::parseIntInRange(subRedText, 0, 99, red) ||
        !ScoreProtocol::parseIntInRange(subBlueText, 0, 99, blue)) {
        Serial.println("SUBMIT: bad red/blue, ignored");
        return;
    }

    // ===== 轮号校验 =====
    if (subRound != currentRoundId) {
        Serial.print("SUBMIT: roundId ");
        Serial.print(subRound);
        Serial.print(" != current ");
        Serial.println(currentRoundId);
        sendAck(subDevice, subClient, subRoundText, subMsgText, "ERR_BAD_ROUND");
        return;
    }

    // ===== 去重 =====
    PerJudgeSubmission& s = roundSubmissions[slot];
    if (s.submitted && s.lastMsgId == subMsgId) {
        // 完全相同的重复提交（ACK 在空中丢了导致客户端重传），幂等 ACK 即可。
        Serial.print("SUBMIT: duplicate msgId=");
        Serial.print(subMsgId);
        Serial.println(", ack only");
        sendAck(subDevice, subClient, subRoundText, subMsgText, "OK_DUPLICATE");
        return;
    }
    if (s.submitted) {
        // 同一轮换了新 msgId 想再提一次（裁判想改分？）。设计文档规定本轮一旦提交即锁定。
        Serial.print("SUBMIT: ");
        Serial.print(subClient);
        Serial.println(" already submitted this round");
        sendAck(subDevice, subClient, subRoundText, subMsgText, "ERR_ALREADY_SUBMITTED");
        return;
    }

    // ===== 接受新提交 =====
    s.submitted = true;
    s.lastMsgId = subMsgId;
    s.red = red;
    s.blue = blue;

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

    sendAck(subDevice, subClient, subRoundText, subMsgText, "OK");
    printRoundState();
    updateDisplay();
}

// 处理一帧 ASSIGN_ACK。
// 帧格式：ASSIGN_ACK,deviceId,clientId,crc16。
// 第一版只打日志确认绑定生效；重传/状态机交给步骤 7。
void handleAssignAck(const ScoreProtocol::ParsedFrame& frame) {
    if (frame.fieldCount != 3) {
        Serial.println("ASSIGN_ACK: bad field count, ignored");
        return;
    }
    Serial.print("ASSIGN_ACK from ");
    Serial.print(frame.fields[1]);
    Serial.print(" as ");
    Serial.println(frame.fields[2]);
}

// 处理一帧 UNBIND_ACK。
// 帧格式：UNBIND_ACK,deviceId,crc16。
// 第一版只打日志确认裁判机已清除本地绑定。
void handleUnbindAck(const ScoreProtocol::ParsedFrame& frame) {
    if (frame.fieldCount != 2) {
        Serial.println("UNBIND_ACK: bad field count, ignored");
        return;
    }
    Serial.print("UNBIND_ACK from ");
    Serial.println(frame.fields[1]);
}

// 从 Serial1（E22 透传口）按字节读入，遇到 '\n' 视为一行结束，然后做 CRC 校验、字段解析、类型识别、打印和业务处理。
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
                            // 其他类型当前没有业务处理。
                            break;
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

// 串口命令行缓冲。handleSerialCommand 按字节累积，遇到 '\n' 视为一行结束并解析。
String serialLine;

// 把一行用 ' ' 拆分成最多 maxTokens 段，写入 tokens；连续空格按一个处理。
// line：原始行（不含末尾换行）。
// tokens：输出数组。
// maxTokens：tokens 容量。
// 返回：实际解析出的段数（0 到 maxTokens）。
// 说明：极简分词，不处理引号；命令格式简单，不需要复杂语法。
uint8_t tokenizeBySpace(const String& line, String tokens[], uint8_t maxTokens) {
    uint8_t count = 0;
    int start = 0;
    const int len = line.length();

    while (start < len && count < maxTokens) {
        while (start < len && line[start] == ' ') {
            start++;
        }
        if (start >= len) break;

        int end = start;
        while (end < len && line[end] != ' ') {
            end++;
        }
        tokens[count++] = line.substring(start, end);
        start = end;
    }
    return count;
}

// 处理 "bind <deviceId> client1|client2|client3" 命令。
// args：tokens[1] 起的参数数组。
// argc：参数个数（不含 "bind" 本身）。
// 行为：
//   1) 参数个数 != 2 → 提示用法。
//   2) clientX 非法 → 提示。
//   3) 该槽位已被占用 → 提示，需先 unbind。
//   4) deviceId 不在未绑定表 → 提示（但允许强行绑定？第一版不允许，避免误绑离线设备）。
//   5) 该 deviceId 已绑到别的槽 → 提示，需先 unbind 那一槽。
//   6) 全部通过 → 写 NVS、从未绑定表移除、发 ASSIGN。
// 不论成功还是失败，命令末尾都打印一次绑定表，方便排查服务端状态分裂。
void handleBindCommand(const String args[], uint8_t argc) {
    if (argc != 2) {
        Serial.println("Usage: bind <deviceId> client1|client2|client3");
        return;
    }

    const String& targetDevice = args[0];
    const String& targetSlotName = args[1];

    const int slot = bindingSlotIndex(targetSlotName);
    if (slot < 0) {
        Serial.println("bind: invalid slot, must be client1/client2/client3");
        return;
    }

    if (bindings[slot].length() > 0) {
        Serial.print("bind: ");
        Serial.print(targetSlotName);
        Serial.print(" already bound to ");
        Serial.print(bindings[slot]);
        Serial.println(", unbind first");
        printBindings();
        return;
    }

    if (findUnboundDevice(targetDevice) < 0) {
        Serial.print("bind: device ");
        Serial.print(targetDevice);
        Serial.println(" not in unbound table; wait for its HELLO first");
        printBindings();
        return;
    }

    const int otherSlot = findBindingByDevice(targetDevice);
    if (otherSlot >= 0) {
        Serial.print("bind: device already bound to ");
        Serial.print(BINDING_SLOT_NAMES[otherSlot]);
        Serial.println(", unbind first");
        printBindings();
        return;
    }

    saveBindingSlot(static_cast<uint8_t>(slot), targetDevice);
    removeUnboundDevice(targetDevice);
    sendAssign(targetDevice, targetSlotName);

    Serial.print("bind: assigned ");
    Serial.print(targetDevice);
    Serial.print(" -> ");
    Serial.println(targetSlotName);
    printBindings();
}

// 处理 "unbind client1|client2|client3" 命令。
// args：tokens[1] 起的参数数组。
// argc：参数个数。
// 行为：
//   1) 参数个数 != 1 或槽位非法 → 提示。
//   2) 槽位本就为空 → 提示。
//   3) 否则记录被解绑的 deviceId、清 NVS、发 UNBIND。
// 不论成功还是失败，命令末尾都打印一次绑定表。
void handleUnbindCommand(const String args[], uint8_t argc) {
    if (argc != 1) {
        Serial.println("Usage: unbind client1|client2|client3");
        return;
    }

    const int slot = bindingSlotIndex(args[0]);
    if (slot < 0) {
        Serial.println("unbind: invalid slot");
        return;
    }

    if (bindings[slot].length() == 0) {
        Serial.print("unbind: ");
        Serial.print(args[0]);
        Serial.println(" already unbound");
        printBindings();
        return;
    }

    const String deviceId = bindings[slot];
    saveBindingSlot(static_cast<uint8_t>(slot), String());
    sendUnbind(deviceId);

    Serial.print("unbind: cleared ");
    Serial.print(args[0]);
    Serial.print(" (was ");
    Serial.print(deviceId);
    Serial.println(")");
    printBindings();
}

// 处理 "list" 命令：把绑定表、未绑定设备表和当前轮次状态一起打印。
void handleListCommand() {
    printBindings();
    printUnboundDevices();
    printRoundState();
}

// 处理 "next-round" 命令：把 currentRoundId 加一，清空本轮提交记录。
// 第一版未做"必须本轮完成才允许下一轮"的限制，也未做累计总分（那属于步骤 14）。
// 推进轮号后立即给所有已绑定的裁判机各发一帧 STATUS，让它们当场看到新 roundId 并解锁，
// 不必等下一次 HEARTBEAT 才同步。这是步骤 14 STATUS,ALL 广播的精简前奏。
void handleNextRoundCommand() {
    currentRoundId++;
    resetRoundSubmissions();
    Serial.print("next-round: advanced to ");
    Serial.println(currentRoundId);

    for (uint8_t i = 0; i < BINDING_SLOT_COUNT; i++) {
        if (bindings[i].length() > 0) {
            sendStatus(bindings[i], BINDING_SLOT_NAMES[i]);
        }
    }

    printRoundState();
    updateDisplay();
}

// 从 Serial（调试串口）按字节读入命令行，遇到 '\r' 或 '\n' 视为一行结束并解析。
// 命令格式：
//   bind <deviceId> client1|client2|client3
//   unbind client1|client2|client3
//   list
// 同时接受 CR、LF 和 CRLF 作为行结束符：
//   - PlatformIO Monitor 默认只发 CR，pyserial-miniterm 也常如此；
//   - 多数 Linux/Web 终端发 LF；
//   - Windows/Putty 可能发 CRLF。
//   遇到任一终止符就处理一次行；空行直接跳过，所以 CRLF 第二个字符不会引发重复处理。
// 处理前先 trim 整行：剥掉首尾任何空白（空格、制表符、boot 阶段产生的低位字节），
// 否则首次连接时 USB CDC 握手可能留下的脏字节会粘在命令前面，把 token 数搞错。
// 解析前会把整行 echo 一遍到串口（"CMD: ..."），便于诊断终端输入异常。
// 输入超过 80 字节直接丢弃，避免缓冲区无限增长。
void handleSerialCommand() {
    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());

        if (c == '\r' || c == '\n') {
            String line = serialLine;
            serialLine = "";

            line.trim();
            if (line.length() == 0) {
                continue;
            }

            Serial.print("CMD: ");
            Serial.println(line);

            constexpr uint8_t MAX_TOKENS = 4;
            String tokens[MAX_TOKENS];
            const uint8_t n = tokenizeBySpace(line, tokens, MAX_TOKENS);

            if (n == 0) {
                continue;
            }

            if (tokens[0] == "bind") {
                handleBindCommand(&tokens[1], static_cast<uint8_t>(n - 1));
            } else if (tokens[0] == "unbind") {
                handleUnbindCommand(&tokens[1], static_cast<uint8_t>(n - 1));
            } else if (tokens[0] == "list") {
                handleListCommand();
            } else if (tokens[0] == "next-round") {
                handleNextRoundCommand();
            } else {
                Serial.print("Unknown command: ");
                Serial.println(tokens[0]);
                Serial.println("Available: bind / unbind / list / next-round");
            }

            continue;
        }

        if (serialLine.length() < 80) {
            serialLine += c;
        } else {
            serialLine = "";
        }
    }
}

// Arduino 启动钩子，上电后只运行一次。
// 职责：初始化调试串口、加载 NVS 绑定表、把 E22 控制脚切到透传模式（M0=M1=LOW）、打开 Serial1 与 E22 通信、并打印启动横幅。
void setup() {
    Serial.begin(115200);
    const unsigned long serialWaitStart = millis();
    while (!Serial && millis() - serialWaitStart < 5000) {
        delay(10);
    }
    delay(500);

    // 加载持久化的绑定表。loadBindingsFromNvs 内部会 prefs.begin()，从此 prefs 保持打开。
    loadBindingsFromNvs();

    Serial.println();
    Serial.println("score_server-lora boot");
    Serial.print("Millis: ");
    Serial.println(millis());
    printBindings();
    printRoundState();

    pinMode(LORA_M0_PIN, OUTPUT);
    pinMode(LORA_M1_PIN, OUTPUT);
    pinMode(LORA_AUX_PIN, INPUT_PULLUP);

    // M0=LOW、M1=LOW：E22 进入正常透明传输模式（详见设计文档 4.3 节）。
    digitalWrite(LORA_M0_PIN, LOW);
    digitalWrite(LORA_M1_PIN, LOW);

    Serial1.begin(LORA_UART_BAUD, SERIAL_8N1, LORA_RX_PIN, LORA_TX_PIN);
    delay(500);

    // 初始化数码管：满亮度并按当前轮次状态刷一次。
    display.setBrightness(7);
    display.clear();
    updateDisplay();

    Serial.println("E22 UART transparent ready");
    Serial.println("Serial commands: bind <deviceId> client1|2|3 / unbind clientX / list / next-round");
}

// Arduino 主循环，会被反复调用。
// 服务端做三件事：消化 LoRa 收到的数据，处理操作员从串口敲入的命令，按周期兜底刷新数码管。
void loop() {
    handleLoraInput();
    handleSerialCommand();

    if (millis() - lastDisplayRefreshMs >= DISPLAY_REFRESH_INTERVAL_MS) {
        updateDisplay();
    }
}
