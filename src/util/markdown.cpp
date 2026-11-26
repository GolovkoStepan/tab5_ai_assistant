#include "util/markdown.h"

#include <string.h>

namespace md {
namespace {

bool is_marker(char ch)
{
    return ch == '*' || ch == '_' || ch == '`';
}

/// Знак списка: «- », «* », «+ » в начале строки. Такой `*` — не курсив,
/// и снимать его нельзя: пункт иначе слипнется с абзацем.
bool is_bullet(const char* s, size_t n, size_t i, size_t line_start)
{
    if (i != line_start || i + 1 >= n) return false;
    const char ch = s[i];
    return (ch == '-' || ch == '*' || ch == '+') && s[i + 1] == ' ';
}

/// Сколько одинаковых маркеров подряд начиная с i.
size_t run_len(const char* s, size_t n, size_t i)
{
    size_t k = 0;
    while (i + k < n && s[i + k] == s[i])
        ++k;
    return k;
}

/// Накопитель результата: при переполнении взводит флаг, а не рвёт текст.
struct Sink {
    char* out;
    size_t cap;
    size_t used{0};
    bool ok{true};

    void put(char ch)
    {
        if (!ok) return;
        if (used + 1 >= cap) {
            ok = false;
            return;
        }
        out[used++] = ch;
    }
};

}  // namespace

bool needed(const char* src, size_t len)
{
    if (src == nullptr) return false;
    if (len == 0) len = strlen(src);
    for (size_t i = 0; i < len; ++i) {
        const char ch = src[i];
        if (ch == '[' || is_marker(ch)) return true;
        // Только заголовок: решётка в середине строки — «№5», «C#».
        if (ch == '#' && (i == 0 || src[i - 1] == '\n')) return true;
    }
    return false;
}

size_t plain(const char* src, size_t len, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';
    if (src == nullptr) return 0;
    if (len == 0) len = strlen(src);

    Sink s{out, out_len};
    size_t line_start = 0;
    size_t skip_at = 0;   // начало снятой закрывающей пары
    size_t skip_len = 0;  // сколько её байт пропустить

    size_t i = 0;
    while (i < len) {
        if (skip_len > 0 && i == skip_at) {
            i += skip_len;
            skip_len = 0;
            continue;
        }

        const char ch = src[i];

        if (ch == '\n') {
            s.put('\n');
            ++i;
            line_start = i;
            continue;
        }

        // Заголовок «# …» / «## …»: решётки и пробел после них не нужны.
        if (ch == '#' && i == line_start) {
            size_t j = i;
            while (j < len && src[j] == '#')
                ++j;
            if (j < len && src[j] == ' ') ++j;
            i = j;
            continue;
        }

        if (is_bullet(src, len, i, line_start)) {
            s.put(ch);
            s.put(src[i + 1]);
            i += 2;
            continue;
        }

        // Ссылка «[текст](адрес)» → текст. Пустая подпись — остаётся адрес:
        // терять ссылку целиком хуже, чем показать адрес.
        if (ch == '[') {
            size_t close = i + 1;
            while (close < len && src[close] != ']' && src[close] != '\n')
                ++close;
            if (close < len && src[close] == ']' && close + 1 < len && src[close + 1] == '(') {
                size_t end = close + 2;
                int depth = 1;
                while (end < len && src[end] != '\n') {
                    if (src[end] == '(') ++depth;
                    if (src[end] == ')') {
                        --depth;
                        if (depth == 0) break;
                    }
                    ++end;
                }
                if (end < len && src[end] == ')') {
                    if (close > i + 1) {
                        for (size_t k = i + 1; k < close; ++k) {
                            // Разметка внутри подписи тоже не нужна.
                            if (is_marker(src[k])) continue;
                            s.put(src[k]);
                        }
                    } else {
                        for (size_t k = close + 2; k < end; ++k)
                            s.put(src[k]);
                    }
                    i = end + 1;
                    continue;
                }
            }
        }

        if (is_marker(ch)) {
            const size_t open = run_len(src, len, i);
            const char prev = i > 0 ? src[i - 1] : ' ';
            const char next = (i + open < len) ? src[i + open] : ' ';

            // `_` внутри слова — «snake_case», не курсив: снимать нельзя.
            const bool in_word = prev != ' ' && prev != '\n' && next != ' ' && next != '\n';
            if (in_word && ch == '_') {
                for (size_t k = 0; k < open; ++k)
                    s.put(ch);
                i += open;
                continue;
            }

            // Закрывающая пара в этой же строке. Пары нет — знак остаётся:
            // одиночная «*» значит умножение, а не курсив.
            size_t j = i + open;
            while (j < len && src[j] != ch && src[j] != '\n')
                ++j;
            if (j < len && src[j] == ch && j > i + open) {
                skip_at = j;
                skip_len = run_len(src, len, j);
                i += open;
                continue;
            }

            for (size_t k = 0; k < open; ++k)
                s.put(ch);
            i += open;
            continue;
        }

        s.put(ch);
        ++i;
    }

    // Переполнение — честный ноль, а не обрывок: половина ссылки или половина
    // слова читались бы как законченный текст.
    if (!s.ok) {
        out[0] = '\0';
        return 0;
    }
    out[s.used] = '\0';
    return s.used;
}

}  // namespace md
