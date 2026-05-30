#pragma once

// 处理服务端 USB 串口命令。
// 支持：bind、unbind、list、next-round [seconds]、reset。
// 该函数按字节读取 Serial，内部维护行缓冲，接受 CR/LF/CRLF 作为行结束符。
void handleSerialCommand();
