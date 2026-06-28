// 服务端 Web 页面和 HTTP 路由实现。
// 负责启动 WiFi AP、渲染 /score 显示页和 /control 控制页，并处理网页表单提交。
#include "WebUi.h"

#include <WebServer.h>
#include <WiFi.h>

namespace {

constexpr const char* WIFI_AP_SSID = "ScoreServer";
constexpr const char* WIFI_AP_PASSWORD = "score1234";

// Arduino WebServer 是同步处理模型：loop 中反复调用 handleClient() 才会处理请求。
WebServer webServer(80);
// WebUi 不直接依赖 ServerState 全局变量，而是通过回调拿一份页面快照。
WebUiStateProvider stateProvider = nullptr;
// WebUiActions 把按钮动作转回业务层，避免网页代码直接操作绑定表/轮次。
WebUiActions webActions;

String htmlEscape(const String& value) {
    String escaped;
    escaped.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); i++) {
        const char c = value.charAt(i);
        switch (c) {
            // 所有来自队名、deviceId、clientId 的文本都进 HTML，必须转义防止破坏页面结构。
            case '&': escaped += F("&amp;"); break;
            case '<': escaped += F("&lt;"); break;
            case '>': escaped += F("&gt;"); break;
            case '"': escaped += F("&quot;"); break;
            case '\'': escaped += F("&#39;"); break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

String shortDeviceId(const String& id) {
    if (id.length() <= 4) {
        // 设备号异常短时直接返回原文，避免 substring 负下标。
        return id;
    }
    // 页面主显示短码，括号里仍显示完整 deviceId，方便对照数码管后四位。
    return id.substring(id.length() - 4);
}

String formatBatteryMv(int battMv) {
    if (battMv <= 0) {
        // 0 表示没有有效电压读数，网页不显示 0.00V，避免误判为没电。
        return F("--");
    }
    return String(battMv / 1000.0f, 2) + F("V");
}

String formatAge(unsigned long lastSeenMs, bool seen) {
    if (!seen) {
        // 服务端启动后还没收到过该裁判消息时，不计算在线年龄。
        return F("--");
    }
    const unsigned long ageMs = millis() - lastSeenMs;
    if (ageMs < 1000UL) {
        return F("刚刚");
    }
    if (ageMs < 60000UL) {
        return String(ageMs / 1000UL) + F("秒前");
    }
    return String(ageMs / 60000UL) + F("分钟前");
}

String formatCountdown(uint32_t seconds) {
    const uint32_t minutes = seconds / 60UL;
    const uint32_t secs = seconds % 60UL;
    String text = String(minutes);
    text += F(":");
    if (secs < 10UL) {
        // 秒数补零，保证显示为 m:ss，例如 1:05。
        text += F("0");
    }
    text += String(secs);
    return text;
}

const char* onlineLabel(unsigned long lastSeenMs, bool seen) {
    if (!seen) {
        return "未见到";
    }
    const unsigned long ageSec = (millis() - lastSeenMs) / 1000UL;
    if (ageSec <= 15UL) {
        // 心跳未锁定 10s、锁定 15s；15s 内认为在线。
        return "在线";
    }
    if (ageSec <= 30UL) {
        // 超过一次心跳但还没太久，标为弱连接，便于发现临界无线/供电问题。
        return "弱连接";
    }
    return "离线";
}

const char* onlineClass(unsigned long lastSeenMs, bool seen) {
    if (!seen) {
        return "muted";
    }
    const unsigned long ageSec = (millis() - lastSeenMs) / 1000UL;
    if (ageSec <= 15UL) {
        return "ok";
    }
    if (ageSec <= 30UL) {
        return "warn";
    }
    return "bad";
}

uint8_t boundedJudgeCount(const WebUiState& state) {
    // 防御 provider 传入超过数组容量的数量，避免渲染时越界。
    return state.judgeCount > WEB_UI_MAX_JUDGES ? WEB_UI_MAX_JUDGES : state.judgeCount;
}

uint8_t boundedUnboundCount(const WebUiState& state) {
    // 未绑定设备表同样按 WebUiState 固定数组容量截断。
    return state.unboundCount > WEB_UI_MAX_UNBOUND_DEVICES ? WEB_UI_MAX_UNBOUND_DEVICES : state.unboundCount;
}

void loadState(WebUiState& state) {
    if (stateProvider != nullptr) {
        // 每次请求实时拉取状态快照，显示页刷新后能看到最新比分/倒计时。
        stateProvider(state);
    }
}

void sendRedirect(const char* path) {
    // POST 后用 303 跳转回控制页，避免浏览器刷新时重复提交表单。
    webServer.sendHeader("Location", path, true);
    webServer.send(303, "text/plain", "");
}

void appendPageStart(String& page, const char* title, bool autoRefresh, bool showHeader) {
    // 所有页面共用同一套 CSS；showHeader=false 时用于纯显示大屏。
    page += F("<!doctype html><html><head><meta charset=\"utf-8\">");
    page += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
    if (autoRefresh) {
        // 保留整页 meta refresh 开关作为兜底；当前动态数据主要由 /live 局部刷新。
        page += F("<meta http-equiv=\"refresh\" content=\"1\">");
    }
    page += F("<title>");
    page += title;
    page += F("</title><style>");
    page += F("body{margin:0;font-family:Arial,'Microsoft YaHei',sans-serif;background:#f4f6f8;color:#17202a}");
    page += F("header{background:#17202a;color:#fff;padding:12px 16px;display:flex;gap:14px;align-items:center;flex-wrap:wrap}");
    page += F("header strong{font-size:18px}nav a{color:#fff;text-decoration:none;margin-right:10px;padding:6px 8px;border-radius:6px;background:#2c3e50}");
    page += F("main{max-width:980px;margin:0 auto;padding:14px}section{background:#fff;border:1px solid #d8dee4;border-radius:8px;margin:0 0 12px;padding:12px}");
    page += F("h1,h2{margin:0 0 10px}table{width:100%;border-collapse:collapse}th,td{border-bottom:1px solid #e6e9ed;padding:8px;text-align:left}th{background:#f8fafc}");
    page += F(".meta{display:flex;gap:10px;flex-wrap:wrap}.pill{display:inline-block;padding:4px 8px;border-radius:6px;background:#eef2f7}");
    page += F(".ok{color:#087f23}.warn{color:#9a6700}.bad{color:#b42318}.muted{color:#687076}.score{font-size:22px;font-weight:700}");
    page += F(".scoreboard{display:grid;grid-template-columns:1fr auto 1fr;gap:18px;align-items:center;text-align:center}.team-name{font-size:clamp(28px,7vw,72px);font-weight:700}.team-score{font-size:clamp(64px,18vw,180px);font-weight:800;line-height:1}.versus{font-size:clamp(24px,5vw,56px);color:#687076}.timer{font-size:clamp(36px,9vw,96px);font-weight:800;text-align:center}.timer-wrap{grid-column:1/4;border-top:1px solid #e6e9ed;padding-top:16px}");
    page += F("button{border:0;border-radius:6px;background:#1f6feb;color:#fff;padding:8px 10px;margin:2px;cursor:pointer}button.danger{background:#b42318}button.secondary{background:#57606a}button:disabled{opacity:.45;cursor:not-allowed}");
    page += F("input{border:1px solid #c8d1dc;border-radius:6px;padding:7px 8px;margin:2px;min-width:90px}");
    page += F("form{display:inline}code{background:#eef2f7;padding:2px 5px;border-radius:4px}.empty{color:#687076;padding:8px 0}");
    page += F("@media(max-width:680px){.scoreboard{grid-template-columns:1fr}.versus{display:none}.timer-wrap{grid-column:1}.team-score{font-size:96px}}");
    page += F("@media(max-width:680px){table,thead,tbody,tr,th,td{display:block}th{display:none}td{border-bottom:0;padding:5px 0}tr{border-bottom:1px solid #e6e9ed;padding:6px 0}}");
    page += F("</style></head><body>");
    if (showHeader) {
        // 控制页保留导航；显示页按用户要求不显示顶部导航栏。
        page += F("<header><strong>Score Server</strong><nav>");
        page += F("<a href=\"/score\">显示页</a><a href=\"/control\">控制页</a>");
        page += F("</nav></header>");
    }
    page += F("<main>");
}

void appendPageEnd(String& page) {
    page += F("</main></body></html>");
}

void appendLiveRefreshScript(String& page, bool controlPage) {
    // 动态数据通过 /live 局部刷新，不再整页 reload。
    // /score 只更新比分板；/control 还会更新轮次摘要、裁判表和未绑定设备表。
    page += F("<script>");
    page += F("(function(){");
    page += F("var busy=false,lastUser=0,submitting=false;");
    if (controlPage) {
        page += F("function mark(){lastUser=Date.now();}");
        page += F("['pointerdown','click','keydown'].forEach(function(e){document.addEventListener(e,mark,true);});");
        page += F("document.addEventListener('submit',function(){submitting=true;},true);");
    }
    page += F("function set(id,html){var e=document.getElementById(id);if(e)e.innerHTML=html;}");
    page += F("function refresh(){");
    if (controlPage) {
        page += F("if(submitting||Date.now()-lastUser<800)return;");
    }
    page += F("if(busy)return;busy=true;fetch('/live',{cache:'no-store'}).then(function(r){return r.text();}).then(function(t){var p=t.split('\\n<!--SPLIT-->\\n');set('scoreboard-live',p[0]||'');");
    if (controlPage) {
        page += F("set('round-live',p[1]||'');set('judge-live',p[2]||'');set('unbound-live',p[3]||'');");
        page += F("set('event-log-live',p[4]||'');");
    }
    page += F("}).catch(function(){}).then(function(){busy=false;});}");
    page += F("setInterval(refresh,1000);setTimeout(refresh,250);");
    page += F("}());");
    page += F("</script>");
}

void appendRoundSummary(String& page, const WebUiState& state) {
    page += F("<section><h2>当前轮次</h2><div class=\"meta\">");
    page += F("<span class=\"pill\">Round ");
    page += String(state.currentRoundId);
    page += F("</span><span class=\"pill\">");
    page += state.roundOpen ? F("OPEN") : F("CLOSED");
    page += F("</span><span class=\"pill\">已提交 ");
    page += String(state.submittedCount);
    page += F("/");
    page += String(boundedJudgeCount(state));
    page += F("</span><span class=\"pill\">未绑定设备 ");
    page += String(boundedUnboundCount(state));
    page += F("</span></div></section>");
}

void appendScoreboard(String& page, const WebUiState& state) {
    // 显示页核心内容只包含队名、总比分、倒计时；控制页也复用这块作为预览。
    page += F("<section class=\"scoreboard\">");
    page += F("<div><div class=\"team-name\">");
    page += htmlEscape(state.teamNames[0]);
    page += F("</div><div class=\"team-score\">");
    page += String(state.totalScores[0]);
    page += F("</div></div><div class=\"versus\">VS</div><div><div class=\"team-name\">");
    page += htmlEscape(state.teamNames[1]);
    page += F("</div><div class=\"team-score\">");
    page += String(state.totalScores[1]);
    page += F("</div></div><div class=\"timer-wrap\"><div class=\"timer\">");
    // 倒计时未启动时显示 --:--，避免误以为 0:00 是刚结束。
    page += state.countdownActive ? formatCountdown(state.countdownRemainingSec) : F("--:--");
    page += F("</div></div></section>");
}

void appendTeamNameForm(String& page, const WebUiState& state) {
    // 两个 input 名称必须和 handleWebTeams 中读取的 team0/team1 一致。
    page += F("<section><h2>队伍名称</h2>");
    page += F("<form method=\"post\" action=\"/teams\">");
    page += F("<input name=\"team0\" maxlength=\"24\" value=\"");
    page += htmlEscape(state.teamNames[0]);
    page += F("\">");
    page += F("<input name=\"team1\" maxlength=\"24\" value=\"");
    page += htmlEscape(state.teamNames[1]);
    page += F("\">");
    page += F("<button type=\"submit\">保存队名</button>");
    page += F("</form></section>");
}

void appendJudgeTable(String& page, const WebUiState& state, bool includeActions) {
    // includeActions=false 时可作为纯展示表，当前控制页传 true 以显示解绑按钮。
    page += F("<section><h2>裁判状态</h2><table><thead><tr>");
    page += F("<th>裁判</th><th>设备</th><th>状态 / 最后通信</th><th>电量</th><th>本轮</th>");
    if (includeActions) {
        page += F("<th>操作</th>");
    }
    page += F("</tr></thead><tbody>");

    const uint8_t judgeCount = boundedJudgeCount(state);
    for (uint8_t i = 0; i < judgeCount; i++) {
        const WebUiJudge& judge = state.judges[i];
        // bound=false 时不看 seen，因为该槽位没有目标设备。
        const bool seen = judge.bound && judge.seen;
        page += F("<tr><td><code>");
        page += htmlEscape(judge.clientId);
        page += F("</code></td><td>");
        if (judge.bound) {
            page += htmlEscape(shortDeviceId(judge.deviceId));
            page += F(" <span class=\"muted\">(");
            page += htmlEscape(judge.deviceId);
            page += F(")</span>");
        } else {
            page += F("<span class=\"muted\">未绑定</span>");
        }
        page += F("</td><td><span class=\"");
        page += onlineClass(judge.lastSeenMs, seen);
        page += F("\">");
        page += onlineLabel(judge.lastSeenMs, seen);
        page += F("</span> <span class=\"muted\">");
        page += formatAge(judge.lastSeenMs, seen);
        page += F("</span></td><td>");
        page += seen ? formatBatteryMv(judge.lastBattMv) : F("--");
        page += F("</td><td>");
        if (judge.submitted) {
            page += F("<span class=\"score\">");
            page += String(judge.red);
            page += F(" - ");
            page += String(judge.blue);
            page += F("</span>");
        } else {
            page += F("<span class=\"muted\">待提交</span>");
        }
        page += F("</td>");
        if (includeActions) {
            page += F("<td>");
            if (judge.bound) {
            page += F("<form method=\"post\" action=\"/unbind\" onsubmit=\"return confirm('确认解绑该裁判？')\">");
                // client 隐藏字段传槽位名，业务层按槽位解绑而不是按短码解绑。
                page += F("<input type=\"hidden\" name=\"client\" value=\"");
                page += htmlEscape(judge.clientId);
                page += F("\"><button class=\"danger\" type=\"submit\">解绑</button></form>");
            } else {
                page += F("<span class=\"muted\">--</span>");
            }
            page += F("</td>");
        }
        page += F("</tr>");
    }

    page += F("</tbody></table></section>");
}

void appendUnboundTable(String& page, const WebUiState& state, bool includeActions) {
    // 未绑定表展示已经 HELLO/HEARTBEAT 但尚未绑定到 client1/2/3 的裁判端。
    page += F("<section><h2>未绑定设备</h2>");
    const uint8_t unboundCount = boundedUnboundCount(state);
    if (unboundCount == 0) {
        page += F("<div class=\"empty\">暂无未绑定裁判端</div></section>");
        return;
    }

    page += F("<table><thead><tr><th>设备</th><th>状态 / 最后通信</th><th>电量</th>");
    if (includeActions) {
        page += F("<th>绑定</th>");
    }
    page += F("</tr></thead><tbody>");

    const uint8_t judgeCount = boundedJudgeCount(state);
    for (uint8_t i = 0; i < unboundCount; i++) {
        const WebUiUnboundDevice& device = state.unboundDevices[i];
        page += F("<tr><td>");
        page += htmlEscape(shortDeviceId(device.deviceId));
        page += F(" <span class=\"muted\">(");
        page += htmlEscape(device.deviceId);
        page += F(")</span></td><td><span class=\"");
        page += onlineClass(device.lastSeenMs, true);
        page += F("\">");
        page += onlineLabel(device.lastSeenMs, true);
        page += F("</span> <span class=\"muted\">");
        page += formatAge(device.lastSeenMs, true);
        page += F("</span></td><td>");
        page += formatBatteryMv(device.lastBattMv);
        page += F("</td>");

        if (includeActions) {
            page += F("<td><form method=\"post\" action=\"/bind\">");
            // device 隐藏字段传完整 deviceId，按钮只决定绑定到哪个 client 槽位。
            page += F("<input type=\"hidden\" name=\"device\" value=\"");
            page += htmlEscape(device.deviceId);
            page += F("\">");
            for (uint8_t slot = 0; slot < judgeCount; slot++) {
                const WebUiJudge& judge = state.judges[slot];
                page += F("<button type=\"submit\" name=\"client\" value=\"");
                page += htmlEscape(judge.clientId);
                page += F("\"");
                if (judge.bound) {
                    // 已被占用的槽位禁用绑定按钮，防止覆盖已有裁判端。
                    page += F(" class=\"secondary\" disabled");
                }
                page += F(">绑定 ");
                page += htmlEscape(judge.clientId);
                page += F("</button>");
            }
            page += F("</form></td>");
        }
        page += F("</tr>");
    }

    page += F("</tbody></table></section>");
}

void appendEventLogTable(String& page, const WebUiState& state) {
    page += F("<section><h2>最近事件</h2>");
    if (state.eventLogCount == 0) {
        page += F("<div class=\"empty\">暂无事件</div></section>");
        return;
    }

    page += F("<table><thead><tr><th>时间</th><th>事件</th></tr></thead><tbody>");
    const uint8_t logCount = state.eventLogCount > WEB_UI_MAX_EVENT_LOGS
        ? WEB_UI_MAX_EVENT_LOGS
        : state.eventLogCount;
    for (uint8_t i = 0; i < logCount; i++) {
        const WebUiEventLog& entry = state.eventLogs[i];
        page += F("<tr><td><span class=\"muted\">");
        page += formatAge(entry.atMs, entry.atMs > 0);
        page += F("</span></td><td>");
        page += htmlEscape(entry.text);
        page += F("</td></tr>");
    }
    page += F("</tbody></table></section>");
}

void handleLiveFragments() {
    WebUiState state;
    loadState(state);

    String page;
    page.reserve(10000);
    appendScoreboard(page, state);
    page += F("\n<!--SPLIT-->\n");
    appendRoundSummary(page, state);
    page += F("\n<!--SPLIT-->\n");
    appendJudgeTable(page, state, true);
    page += F("\n<!--SPLIT-->\n");
    appendUnboundTable(page, state, true);
    page += F("\n<!--SPLIT-->\n");
    appendEventLogTable(page, state);

    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send(200, "text/html; charset=utf-8", page);
}

void handleRootPage() {
    // 根路径默认进入显示页，手机/大屏打开 AP 后输入 IP 即可看到比分。
    sendRedirect("/score");
}

void handleScorePage() {
    WebUiState state;
    loadState(state);

    String page;
    page.reserve(5500);
    // showHeader=false 是显示页去掉上方导航栏的关键开关。
    appendPageStart(page, "Score", false, false);
    page += F("<div id=\"scoreboard-live\">");
    appendScoreboard(page, state);
    page += F("</div>");
    appendLiveRefreshScript(page, false);
    appendPageEnd(page);
    webServer.send(200, "text/html; charset=utf-8", page);
}

void handleControlPage() {
    WebUiState state;
    loadState(state);

    String page;
    page.reserve(10000);
    // 控制页不用 meta refresh，改用下面的 JS：没有输入框焦点时自动刷新，有焦点时暂停。
    appendPageStart(page, "Control", false, true);
    page += F("<h1>控制页</h1>");
    appendTeamNameForm(page, state);
    page += F("<div id=\"scoreboard-live\">");
    appendScoreboard(page, state);
    page += F("</div><div id=\"round-live\">");
    appendRoundSummary(page, state);
    page += F("</div>");

    page += F("<section><h2>比赛控制</h2>");
    page += F("<form method=\"post\" action=\"/next-round\" onsubmit=\"return confirm('确认进入下一轮并开始倒计时？')\">");
    page += F("<span class=\"muted\">倒计时秒数</span>");
    // seconds 由 handleWebNextRound 读取，范围限制在浏览器端，服务端仍做非负处理。
    page += F("<input type=\"number\" name=\"seconds\" min=\"0\" max=\"9999\" value=\"60\">");
    page += F("<button type=\"submit\">下一轮</button></form>");
    page += F("<form method=\"post\" action=\"/reset\" onsubmit=\"return confirm('确认重置总比分和轮次？绑定关系和队名不会清除。')\">");
    page += F("<button class=\"danger\" type=\"submit\">重置比分</button></form>");
    page += F("</section>");

    page += F("<div id=\"judge-live\">");
    appendJudgeTable(page, state, true);
    page += F("</div><div id=\"unbound-live\">");
    appendUnboundTable(page, state, true);
    page += F("</div><div id=\"event-log-live\">");
    appendEventLogTable(page, state);
    page += F("</div>");
    appendLiveRefreshScript(page, true);
    appendPageEnd(page);
    webServer.send(200, "text/html; charset=utf-8", page);
}

void handleWebBind() {
    if (webActions.bind == nullptr) {
        // setupServerWeb 理论上总会注册动作；为空说明初始化顺序或回调配置错误。
        webServer.send(503, "text/plain", "bind action unavailable");
        return;
    }
    if (!webServer.hasArg("device") || !webServer.hasArg("client")) {
        // 表单被手工构造或浏览器提交异常时，不调用业务层。
        webServer.send(400, "text/plain", "missing device/client");
        return;
    }
    String device = webServer.arg("device");
    String client = webServer.arg("client");
    // trim 防止手工 POST 时带空格导致 bindingSlotIndex 匹配失败。
    device.trim();
    client.trim();
    webActions.bind(device, client);
    sendRedirect("/control");
}

void handleWebUnbind() {
    if (webActions.unbind == nullptr) {
        webServer.send(503, "text/plain", "unbind action unavailable");
        return;
    }
    if (!webServer.hasArg("client")) {
        // 解绑必须明确槽位名。
        webServer.send(400, "text/plain", "missing client");
        return;
    }
    String client = webServer.arg("client");
    // 去掉首尾空格后再交给 ServerActions 校验 client1/2/3。
    client.trim();
    webActions.unbind(client);
    sendRedirect("/control");
}

void handleWebNextRound() {
    if (webActions.nextRound == nullptr) {
        webServer.send(503, "text/plain", "next-round action unavailable");
        return;
    }
    uint32_t seconds = 0;
    if (webServer.hasArg("seconds")) {
        const int parsed = webServer.arg("seconds").toInt();
        if (parsed > 0) {
            // 负数、空串、非数字都会被当作 0，不启动倒计时。
            seconds = static_cast<uint32_t>(parsed);
        }
    }
    webActions.nextRound(seconds);
    sendRedirect("/control");
}

void handleWebReset() {
    if (webActions.reset == nullptr) {
        webServer.send(503, "text/plain", "reset action unavailable");
        return;
    }
    webActions.reset();
    // reset 后回控制页，页面会重新拉取 round=1、总比分=0 的状态。
    sendRedirect("/control");
}

void handleWebTeams() {
    if (webActions.setTeamNames == nullptr) {
        webServer.send(503, "text/plain", "team action unavailable");
        return;
    }
    String team0 = webServer.hasArg("team0") ? webServer.arg("team0") : String();
    String team1 = webServer.hasArg("team1") ? webServer.arg("team1") : String();
    // 空队名由业务层回落为默认“红方/蓝方”。
    team0.trim();
    team1.trim();
    webActions.setTeamNames(team0, team1);
    sendRedirect("/control");
}

void handleWebNotFound() {
    webServer.send(404, "text/plain", "not found");
}

}  // namespace

void setupWebUi(WebUiStateProvider provider, const WebUiActions& actions) {
    // 保存回调指针，后续 HTTP handler 会通过它们读状态/执行业务动作。
    stateProvider = provider;
    webActions = actions;

    WiFi.mode(WIFI_AP);
    // 关闭 WiFi 省电，降低手机连接 AP 后请求偶发延迟。
    WiFi.setSleep(false);
    const bool apStarted = WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);

    // 路由集中注册在这里，ServerWeb.cpp 只负责把业务回调接进来。
    webServer.on("/", HTTP_GET, handleRootPage);
    webServer.on("/score", HTTP_GET, handleScorePage);
    webServer.on("/control", HTTP_GET, handleControlPage);
    webServer.on("/live", HTTP_GET, handleLiveFragments);
    webServer.on("/bind", HTTP_POST, handleWebBind);
    webServer.on("/unbind", HTTP_POST, handleWebUnbind);
    webServer.on("/next-round", HTTP_POST, handleWebNextRound);
    webServer.on("/reset", HTTP_POST, handleWebReset);
    webServer.on("/teams", HTTP_POST, handleWebTeams);
    webServer.onNotFound(handleWebNotFound);
    // begin 后必须在 loop 中持续调用 handleWebUiClient() 才会真正处理请求。
    webServer.begin();

    Serial.print("WiFi AP ");
    Serial.print(apStarted ? "ready: " : "failed: ");
    Serial.print(WIFI_AP_SSID);
    Serial.print("  password=");
    Serial.print(WIFI_AP_PASSWORD);
    Serial.print("  ip=");
    Serial.println(WiFi.softAPIP());
}

void handleWebUiClient() {
    // 非阻塞处理一个或少量待处理连接，适合放在主 loop。
    webServer.handleClient();
}
