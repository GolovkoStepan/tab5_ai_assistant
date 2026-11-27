#include "util/serial_cmd.h"

#include <string.h>

namespace serialcmd {
namespace {

bool starts_with_ci(const char* s, size_t len, const char* prefix)
{
    const size_t n = strlen(prefix);
    if (len < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// Сравнить слово с образцом без учёта регистра.
///
/// Работает именно со словом целиком, а не с началом строки: `wifi off extra`
/// — не команда, и «off» в нём не должен сойти за аргумент. Вызывающий уже
/// срезал пробелы по краям, так что длина слова — до первого пробела.
bool word_is(const char* s, size_t n, const char* word)
{
    if (n != strlen(word)) return false;
    return starts_with_ci(s, n, word);
}

/// Разбор `wifi off` / `wifi on`. Остальное после слова — ошибка, а не
/// молчаливое «понял как получилось».
bool parse_wifi(const char* s, size_t n, Kind* out)
{
    if (!starts_with_ci(s, n, "wifi")) return false;
    // «wifiX» — не команда wifi.
    if (n > 4 && !is_space(s[4])) return false;

    size_t start = 4;
    while (start < n && is_space(s[start]))
        ++start;
    const size_t rest = n - start;

    if (word_is(s + start, rest, "off")) {
        if (out != nullptr) *out = Kind::WifiOff;
        return true;
    }
    if (word_is(s + start, rest, "on")) {
        if (out != nullptr) *out = Kind::WifiOn;
        return true;
    }

    // «wifi» без аргумента и «wifi что-нибудь» одинаково бесполезны: пусть
    // человек увидит подсказку, а не решит, что радио уже выключено.
    if (out != nullptr) *out = Kind::Help;
    return true;
}

}  // namespace

Kind parse(const char* line, size_t len, const char** out_text, size_t* out_len)
{
    if (out_text != nullptr) *out_text = nullptr;
    if (out_len != nullptr) *out_len = 0;
    if (line == nullptr) return Kind::None;

    // Обрезаем пробелы по краям: терминал и `printf` любят добавлять `\r`.
    size_t begin = 0;
    while (begin < len && is_space(line[begin]))
        ++begin;
    size_t end = len;
    while (end > begin && is_space(line[end - 1]))
        --end;

    const size_t n = end - begin;
    if (n == 0) return Kind::None;
    const char* s = line + begin;

    if (n == 1 && s[0] == '?') return Kind::Help;
    if (starts_with_ci(s, n, "help") && (n == 4 || is_space(s[4]))) return Kind::Help;
    if (starts_with_ci(s, n, "thread") && (n == 6 || is_space(s[6]))) return Kind::Thread;

    // `wifi off` / `wifi on` — выключатель сети для проверки «что будет без
    // интернета». Без него такой прогон требует человека у экрана Wi-Fi.
    {
        Kind wifi = Kind::Unknown;
        if (parse_wifi(s, n, &wifi)) return wifi;
    }

    // `api <адрес>` — подменить адрес API на время прогона (проверка на
    // фальшивом сервере). Как и `wifi off`, это диагностика: набирать IP
    // кареткой на устройстве ненадёжно и не повторяется скриптом.
    if (starts_with_ci(s, n, "api") && (n == 3 || is_space(s[3]))) {
        size_t start = 3;
        while (start < n && is_space(s[start]))
            ++start;
        if (start >= n) {
            // `api` без адреса — подсказка, а не «понял как получилось».
            return Kind::Help;
        }
        if (out_text != nullptr) *out_text = s + start;
        if (out_len != nullptr) *out_len = n - start;
        return Kind::Api;
    }

    // `tz <зона>` — таймзона. Живёт рядом с `api`: и то, и другое правит
    // config.json, а набирать «YEKT-5» кареткой на устройстве неудобно и не
    // повторяется скриптом. Без аргумента — подсказка, как у `api`.
    if (starts_with_ci(s, n, "tz") && (n == 2 || is_space(s[2]))) {
        size_t start = 2;
        while (start < n && is_space(s[start]))
            ++start;
        if (start >= n) return Kind::Help;
        if (out_text != nullptr) *out_text = s + start;
        if (out_len != nullptr) *out_len = n - start;
        return Kind::Tz;
    }

    // `ask <текст>` и `> <текст>`.
    size_t start = 0;
    const bool is_ask = starts_with_ci(s, n, "ask") && (n == 3 || is_space(s[3]));
    if (is_ask) {
        start = 3;
    } else if (s[0] == '>') {
        start = 1;
    } else {
        return Kind::Unknown;
    }

    // Пробелы между командой и текстом не входят в вопрос.
    while (start < n && is_space(s[start]))
        ++start;
    if (start >= n) {
        // Команда без текста — это не вопрос. Отвечаем подсказкой, а не
        // пустым запросом к модели.
        return Kind::Help;
    }

    // Хвостовые пробелы уже срезаны выше, так что текст — от start до n.
    if (out_text != nullptr) *out_text = s + start;
    if (out_len != nullptr) *out_len = n - start;
    return Kind::Ask;
}

const char* help_text()
{
    return "Serial: \"ask <вопрос>\" или \"> <вопрос>\" — отправить в чат; "
           "\"thread\" — распечатать тред; \"wifi off\"/\"wifi on\" — выключить и включить сеть; "
           "\"api <адрес>\" — подменить адрес API (проверка на своём сервере); "
           "\"tz <зона>\" — сменить таймзону, формат POSIX: \"tz YEKT-5\"; "
           "\"help\" — эта подсказка";
}

}  // namespace serialcmd
