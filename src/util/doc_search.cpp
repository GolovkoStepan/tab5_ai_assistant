// Поиск и сниппеты для архива документов. Без Arduino — тестируется на хосте.

#include "util/doc_search.h"

#include <stdio.h>
#include <string.h>

namespace docsearch {
namespace {

constexpr char32_t kInvalid = 0xFFFD;

/// Декодировать один символ UTF-8.
///
/// @param s    начало
/// @param left сколько байт доступно
/// @param cp   [выход] код символа
/// @return длина в байтах (никогда не 0 — на битом байте возвращает 1)
size_t decode(const char* s, size_t left, char32_t* cp)
{
    if (left == 0) {
        *cp = 0;
        return 0;
    }
    const unsigned char c = static_cast<unsigned char>(s[0]);
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && left >= 2) {
        *cp = static_cast<char32_t>(((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[1]) & 0x3Fu));
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && left >= 3) {
        *cp = static_cast<char32_t>(((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[1]) & 0x3Fu) << 6) |
                                    (static_cast<unsigned char>(s[2]) & 0x3Fu));
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && left >= 4) {
        *cp = static_cast<char32_t>(((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[1]) & 0x3Fu) << 12) |
                                    ((static_cast<unsigned char>(s[2]) & 0x3Fu) << 6) |
                                    (static_cast<unsigned char>(s[3]) & 0x3Fu));
        return 4;
    }
    *cp = kInvalid;
    return 1;
}

/// Привести к нижнему регистру: латиница, кириллица, Ё.
///
/// Полный Unicode CaseFolding не нужен: архив ведётся по-русски и по-английски,
/// а тянуть таблицу на 1400 символов ради «ß» — лишние килобайты во флеше.
char32_t fold(char32_t cp)
{
    if (cp >= U'A' && cp <= U'Z') return cp - U'A' + U'a';
    if (cp >= U'А' && cp <= U'Я') return cp - U'А' + U'а';
    if (cp == U'Ё') return U'ё';
    return cp;
}

}  // namespace

size_t utf8_char_len(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;  // битый байт — считаем одним, иначе зациклимся
}

const char* find_ci(const char* haystack, size_t hay_len, const char* needle, size_t needle_len)
{
    if (haystack == nullptr || needle == nullptr) return nullptr;
    if (needle_len == 0) return haystack;
    if (hay_len < needle_len) return nullptr;

    // Сравниваем посимвольно с приведением к нижнему регистру. Наивный
    // перебор здесь уместен: документы — десятки килобайт, а запрос — одно
    // слово. KMP искал бы то же самое, но требовал бы таблицу префиксов.
    for (size_t i = 0; i + needle_len <= hay_len; ++i) {
        // Начинать сравнение с середины многобайтного символа нельзя: там
        // может случайно оказаться байт, совпадающий с ASCII-буквой запроса.
        if (i > 0 && (static_cast<unsigned char>(haystack[i]) & 0xC0) == 0x80) continue;

        size_t hi = i;
        size_t ni = 0;
        bool matched = true;
        while (ni < needle_len) {
            if (hi >= hay_len) {
                matched = false;
                break;
            }
            char32_t hc = 0;
            char32_t nc = 0;
            const size_t hlen = decode(haystack + hi, hay_len - hi, &hc);
            const size_t nlen = decode(needle + ni, needle_len - ni, &nc);
            if (hlen == 0 || nlen == 0 || fold(hc) != fold(nc)) {
                matched = false;
                break;
            }
            hi += hlen;
            ni += nlen;
        }
        if (matched) return haystack + i;
    }
    return nullptr;
}

size_t snippet(const char* text, size_t text_len, size_t hit, size_t hit_len, size_t radius, char* out, size_t out_len)
{
    if (text == nullptr || out == nullptr || out_len == 0) return 0;
    if (hit > text_len) hit = text_len;
    if (hit + hit_len > text_len) hit_len = text_len - hit;

    // Начало окна: не раньше нуля и с выравниванием по границе символа.
    size_t start = hit > radius ? hit - radius : 0;
    while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
        --start;

    // Конец окна — тоже по границе символа.
    size_t end = hit + hit_len + radius;
    if (end > text_len) end = text_len;
    while (end < text_len && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end;

    size_t n = 0;
    auto put = [&](const char* s, size_t len) -> bool {
        if (n + len + 1 > out_len) return false;
        memcpy(out + n, s, len);
        n += len;
        out[n] = '\0';
        return true;
    };

    if (start > 0 && !put("…", 3)) return n;
    for (size_t i = start; i < end; ++i) {
        char c = text[i];
        // Сниппет — одна строка: многострочный кусок в ответе модели выглядел
        // бы как рваная лента без начала и конца.
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (!put(&c, 1)) return n;
    }
    if (end < text_len) put("…", 3);
    return n;
}

size_t truncate_utf8(const char* text, size_t len, size_t max_bytes, char* out, size_t out_len)
{
    if (text == nullptr || out == nullptr || out_len == 0) return 0;
    if (max_bytes > out_len - 1) max_bytes = out_len - 1;
    if (len <= max_bytes) {
        if (len + 1 > out_len) len = out_len - 1;
        memcpy(out, text, len);
        out[len] = '\0';
        return len;
    }

    // Отступаем назад, пока не окажемся на границе символа.
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;

    // Многоточие занимает 3 байта — оставляем под него место.
    const char* ellipsis = "…";
    while (cut > 0 && cut + 3 > out_len - 1) {
        --cut;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            --cut;
    }

    memcpy(out, text, cut);
    memcpy(out + cut, ellipsis, 3);
    out[cut + 3] = '\0';
    return cut + 3;
}

size_t format_list_row(const char* path, const char* title, const char* updated, char* out, size_t out_len)
{
    if (path == nullptr || out == nullptr || out_len == 0) return 0;
    const char* t = (title != nullptr && title[0] != '\0') ? title : path;
    const int n = (updated != nullptr && updated[0] != '\0') ? snprintf(out, out_len, "%s — %s (%s)", path, t, updated)
                                                             : snprintf(out, out_len, "%s — %s", path, t);
    if (n < 0 || static_cast<size_t>(n) >= out_len) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n);
}

}  // namespace docsearch
