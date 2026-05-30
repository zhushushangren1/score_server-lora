#pragma once

// 初始化服务端实体按钮 GPIO。
// GPIO4：短按进入下一轮；GPIO5：长按 3 秒重置总比分。
void setupServerButtons();

// 扫描并去抖服务端实体按钮。
// 非阻塞；每个 loop 调用一次即可。触发动作时会调用 ServerActions 中的复用业务函数。
void pollServerButtons();
