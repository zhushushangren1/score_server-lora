#pragma once

// 初始化服务端 Web 层。
// 负责把 ServerState 转换为 WebUiState，并把绑定/解绑/下一轮/重置/改队名动作注册给 WebUi。
void setupServerWeb();

// 处理一个 WebServer 客户端请求。
// 非阻塞，loop 中高频调用；底层实际请求处理在 WebUi.cpp。
void handleServerWebClient();
