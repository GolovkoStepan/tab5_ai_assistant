// Реализация чистых функций форматирования.
// Не зависит от Arduino — чтобы тестировалось на хосте.

#include "util/format.h"

#include <stdio.h>
#include <string.h>

namespace {

/// Длина UTF-8 символа по первому байту. 0 — некорректный стартовый байт.
size_t utf8_char_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;  // некорректный байт — считаем за один, чтобы не зациклиться
}

/// Число UTF-8 символов в строке.
size_t utf8_length(const char* s)
{
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p != 0; p += utf8_char_len(*p)) {
        ++n;
    }
    return n;
}

/// Байтовое смещение N-го UTF-8 символа. Если символов меньше — длина строки.
size_t utf8_offset(const char* s, size_t index)
{
    const unsigned char* p = (const unsigned char*)s;
    for (size_t i = 0; i < index && *p != 0; ++i) {
        p += utf8_char_len(*p);
    }
    return (size_t)(p - (const unsigned char*)s);
}

}  // namespace

void format_bytes(unsigned long long bytes, char* out, size_t out_len)
{
    if (out_len == 0) return;

    constexpr unsigned long long kKiB = 1024ULL;
    constexpr unsigned long long kMiB = kKiB * 1024ULL;
    constexpr unsigned long long kGiB = kMiB * 1024ULL;

    if (bytes >= kGiB) {
        snprintf(out, out_len, "%.1f ГБ", (double)bytes / (double)kGiB);
    } else if (bytes >= kMiB) {
        snprintf(out, out_len, "%.1f МБ", (double)bytes / (double)kMiB);
    } else if (bytes >= kKiB) {
        snprintf(out, out_len, "%.1f КБ", (double)bytes / (double)kKiB);
    } else {
        snprintf(out, out_len, "%llu Б", bytes);
    }
}

void format_tokens(unsigned long tokens, char* out, size_t out_len)
{
    if (out_len == 0) return;

    if (tokens >= 1000000UL) {
        snprintf(out, out_len, "%.1fM", (double)tokens / 1000000.0);
    } else if (tokens >= 1000UL) {
        snprintf(out, out_len, "%.1fk", (double)tokens / 1000.0);
    } else {
        snprintf(out, out_len, "%lu", tokens);
    }
}

void truncate_text(const char* text, size_t max_chars, char* out, size_t out_len)
{
    if (out_len == 0) return;
    if (text == nullptr) {
        out[0] = '\0';
        return;
    }
    if (max_chars == 0) {
        out[0] = '\0';
        return;
    }

    const size_t total = utf8_length(text);
    if (total <= max_chars) {
        // Копируем как есть, но не выходим за буфер
        snprintf(out, out_len, "%s", text);
        return;
    }

    // Оставляем (max_chars - 1) символов и многоточие
    const size_t keep = max_chars - 1;
    const size_t byte_cut = utf8_offset(text, keep);
    const size_t copy_len = byte_cut < out_len - 1 ? byte_cut : out_len - 1;

    memcpy(out, text, copy_len);
    // Дописываем многоточие, если есть место (3 байта в UTF-8)
    if (copy_len + 3 < out_len) {
        memcpy(out + copy_len, "…", 3);
        out[copy_len + 3] = '\0';
    } else {
        out[copy_len] = '\0';
    }
}

size_t wrap_text(const char* text, size_t width, size_t* offsets, size_t max_lines)
{
    if (text == nullptr || offsets == nullptr || max_lines == 0) return 0;
    if (width == 0) width = 1;

    size_t line = 0;
    offsets[0] = 0;
    line = 1;

    const unsigned char* p = (const unsigned char*)text;
    size_t byte_pos = 0;
    size_t chars_on_line = 0;
    size_t last_space_byte = 0;    // байтовое смещение последнего пробела на строке
    size_t chars_since_space = 0;  // символов после последнего пробела

    while (*p != 0) {
        const size_t clen = utf8_char_len(*p);

        // Обработка явного перевода строки
        if (*p == '\n') {
            p += clen;
            byte_pos += clen;
            chars_on_line = 0;
            chars_since_space = 0;
            last_space_byte = 0;
            if (line < max_lines) {
                offsets[line++] = byte_pos;
            }
            continue;
        }

        if (*p == ' ') {
            last_space_byte = byte_pos;
            chars_since_space = 0;
        } else {
            ++chars_since_space;
        }

        // Строка переполнена — переносим по последнему пробелу, если он есть
        if (chars_on_line >= width) {
            if (line >= max_lines) break;
            if (last_space_byte > offsets[line - 1]) {
                offsets[line++] = last_space_byte + 1;
                chars_on_line = chars_since_space;
                last_space_byte = 0;
            } else {
                // Слово длиннее строки — режем принудительно
                offsets[line++] = byte_pos;
                chars_on_line = 0;
            }
        }

        p += clen;
        byte_pos += clen;
        ++chars_on_line;
    }

    return line;
}
