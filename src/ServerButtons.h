// 服务端实体按钮模块接口。
// 主程序只需要初始化按钮并在 loop() 中周期性扫描。
#pragma once

// 初始化服务端实体按钮 GPIO。
// GPIO5：短按进入下一轮；GPIO12：长按 3 秒重置总比分。
void setupServerButtons();

// 扫描并去抖服务端实体按钮。
// 非阻塞；每个 loop 调用一次即可。触发动作时会调用 ServerActions 中的复用业务函数。
void pollServerButtons();
