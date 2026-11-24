// Время в ISO-8601 и смещение из POSIX-таймзоны. Без Arduino.
//
// Своя функция, а не strftime: в newlib для ESP32 формат %z может быть не
// поддержан, а tzset() зависит от глобального состояния и переменной TZ,
// которую на плате никто не выставляет. Здесь же смещение приходит явно из
// настроек, и результат одинаков и на хосте, и на плате.

#include "util/iso_time.h"

#include <stdio.h>
#include <string.h>

namespace isotime {

bool parse_tz_offset(const char* tz, int* out_minutes)
{
    if (tz == nullptr || out_minutes == nullptr) return false;

    // Ищем последнюю часть со знаком: "MSK-3", "UTC+5", "<+03>-3".
    // Имя может содержать что угодно, кроме цифр и знаков, — по стандарту
    // POSIX имя это не менее трёх букв, но на практике встречается разное.
    const char* p = tz;
    const char* sign_pos = nullptr;
    while (*p != '\0') {
        if ((*p == '+' || *p == '-') && p > tz) {
            sign_pos = p;
        }
        ++p;
    }

    if (sign_pos == nullptr) return false;

    // Знак POSIX: "+5" означает UTC−5, поэтому инвертируем.
    const int posix_sign = (*sign_pos == '-') ? 1 : -1;

    const char* num = sign_pos + 1;
    if (*num < '0' || *num > '9') return false;

    int total_minutes = 0;
    int hours = 0;
    while (*num >= '0' && *num <= '9') {
        hours = hours * 10 + (*num - '0');
        if (hours > 14) return false;
        ++num;
    }

    total_minutes = hours * 60;

    // POSIX допускает минуты через двоеточие: "MSK-3:30".
    if (*num == ':') {
        ++num;
        int minutes = 0;
        int digits = 0;
        while (*num >= '0' && *num <= '9' && digits < 2) {
            minutes = minutes * 10 + (*num - '0');
            ++digits;
            ++num;
        }
        if (digits == 0 || minutes > 59) return false;
        total_minutes += minutes;
    }

    // Хвост после числа — не наша забота только если это пробелы; иначе
    // строка выглядит как опечатка, и молча её принимать не стоит.
    if (*num != '\0') return false;

    *out_minutes = posix_sign * total_minutes;
    return true;
}

bool is_valid(int year, int month, int day, int hour, int minute, int second)
{
    if (year < 2020 || year > 2099) return false;
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > 31) return false;
    if (hour < 0 || hour > 23) return false;
    if (minute < 0 || minute > 59) return false;
    if (second < 0 || second > 59) return false;
    return true;
}

size_t format_iso8601(
    char* out, size_t out_len, int year, int month, int day, int hour, int minute, int second, int tz_minutes)
{
    if (out == nullptr || out_len == 0) return 0;
    if (!is_valid(year, month, day, hour, minute, second)) return 0;
    if (tz_minutes < -14 * 60 || tz_minutes > 14 * 60) return 0;

    const char sign = tz_minutes < 0 ? '-' : '+';
    const int abs_minutes = tz_minutes < 0 ? -tz_minutes : tz_minutes;

    const int n = snprintf(out, out_len, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", year, month, day, hour, minute,
        second, sign, abs_minutes / 60, abs_minutes % 60);

    if (n <= 0 || static_cast<size_t>(n) + 1 > out_len) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n);
}

}  // namespace isotime
