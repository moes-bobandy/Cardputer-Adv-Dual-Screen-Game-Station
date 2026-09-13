#include "utils.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <rom/ets_sys.h>

namespace share {

void sleep_until_us(uint64_t t_us)
{
    for (;;) {
        int64_t now = (int64_t)esp_timer_get_time();
        int64_t remain = (int64_t)t_us - now;
        if (remain <= 0) break;

        if (remain > 2000) {
            // big wait
            vTaskDelay(pdMS_TO_TICKS((remain - 1000) / 1000));
        } else {
            // short busy wait
            ets_delay_us((uint32_t)remain);
            break;
        }
    }
}

}
