#pragma once

void sim_modem_init_uart(void);
void sim_modem_start_task(void);

// 當 MQTT 連線建立時呼叫此函式，觸發讀取滯留的簡訊
void sim_modem_trigger_flush(void);
