// Арифметика страниц списков — без Arduino.
//
// Подробности и обоснование — в include/util/list_page.h.

#include "util/list_page.h"

#include <stdio.h>
#include <string.h>

namespace listpage {

size_t shown(size_t total, size_t offset, size_t loaded)
{
    if (offset >= total) return 0;
    const size_t left = total - offset;
    // loaded не может быть больше остатка: если файлы удалили между чтениями,
    // «показано» не должно обгонять «всего».
    return loaded < left ? loaded : left;
}

bool has_more(size_t total, size_t offset, size_t loaded)
{
    return next_offset(total, offset, loaded) < total;
}

size_t next_offset(size_t total, size_t offset, size_t loaded)
{
    const size_t advance = offset + loaded;
    return advance < total ? advance : total;
}

size_t label(const char* noun, size_t total, size_t shown, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';

    const char* name = noun != nullptr ? noun : "";
    int n = 0;
    if (shown >= total) {
        // Всё поместилось — одно число. «12 из 12» не сообщает ничего, чего не
        // видно по самому списку.
        n = snprintf(out, out_len, "%s: %u", name, static_cast<unsigned>(total));
    } else {
        n = snprintf(out, out_len, "%s: %u из %u", name, static_cast<unsigned>(shown), static_cast<unsigned>(total));
    }

    if (n <= 0 || static_cast<size_t>(n) + 1 > out_len) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n);
}

size_t more_hint(size_t remaining, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';
    if (remaining == 0) return 0;

    // Стрелка — та, которой листают: списки ходят по страницам «← →»
    // (v1.1.0), и «↓ — ещё 77» обещала бы движение, которого нет.
    const int n = snprintf(out, out_len, "→ — ещё %u", static_cast<unsigned>(remaining));
    if (n <= 0 || static_cast<size_t>(n) + 1 > out_len) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n);
}

}  // namespace listpage
