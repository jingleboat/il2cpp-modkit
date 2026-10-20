#pragma once
#include <cstdint>

/* 安装一个 arm64 内联 hook。
 *   target   目标函数地址（已加模块基址）
 *   replace  替换函数地址（ABI 与原函数一致）
 *   orig_out 可选，取回「调用原函数」的跳板指针
 */
bool hook_install(const char* name, void* target, void* replace, void** orig_out);
