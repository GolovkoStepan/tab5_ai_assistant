// Форматирование сведений для экрана «О программе» — без железа.
//
// Подробности и зачем это отдельный модуль — в include/util/about_info.h.

#include "util/about_info.h"

#include <stdio.h>

namespace about {

void uptime_label(uint32_t ms, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return;

    const uint32_t total_min = ms / 60000u;

    // Меньше минуты — «только что», а не «0 мин»: ноль читается как ошибка
    // счётчика, хотя устройство просто недавно включили.
    if (total_min == 0) {
        snprintf(out, out_len, "%s", "только что");
        return;
    }

    const uint32_t days = total_min / (60u * 24u);
    const uint32_t hours = (total_min / 60u) % 24u;
    const uint32_t mins = total_min % 60u;

    // Сутки и часы показываются, только когда они есть: «0 ч 7 мин» лишнее
    // слово в строке, которую читают одним взглядом.
    if (days > 0) {
        snprintf(out, out_len, "%u сут %u ч", static_cast<unsigned>(days), static_cast<unsigned>(hours));
        return;
    }
    if (hours > 0) {
        snprintf(out, out_len, "%u ч %u мин", static_cast<unsigned>(hours), static_cast<unsigned>(mins));
        return;
    }
    snprintf(out, out_len, "%u мин", static_cast<unsigned>(mins));
}

}  // namespace about
