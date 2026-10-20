#pragma once
/* 易物三件套：作价恒占优 / 显示池子 / 确认搬空 */

void trade_install();          /* 装 hook（在 install_tweaks 里调） */
void trade_hook_fire();        /* 供 ui 面板刷新用，无实际逻辑 */
