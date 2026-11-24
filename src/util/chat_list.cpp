// Строка списка тредов: разбор имени и заголовка.
//
// Здесь только текст: SD читает hw/chat_store, экран показывает ui/threads_screen.

#include "util/chat_list.h"

#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

#include "util/api_json.h"
#include "util/chat_line.h"

namespace chatlist {
namespace {

/// Отрезает у строки хвост, разрезанный посреди символа UTF-8.
///
/// Заголовок обрезается по длине, а кириллица занимает по два байта: обрезка
/// ровно по границе буфера оставила бы висящий первый байт пары, и M5GFX
/// нарисовал бы вместо буквы рамку.
size_t trim_utf8(const char* s, size_t len)
{
    // Пока последний байт — продолжение (10xxxxxx), отступаем: значит, символ
    // начался раньше и в буфер не влез.
    while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80)
        --len;
    return len;
}

}  // namespace

bool parse_slug(const char* slug, Stamp& out)
{
    out = Stamp{};
    if (slug == nullptr) return false;

    // «chat-» + 8 цифр + «-» + 6 цифр = 19 символов. Хвост (суффикс «-1») не
    // проверяем: он не влияет на дату.
    if (strncmp(slug, "chat-", 5) != 0) return false;
    if (strlen(slug) < 19) return false;

    const char* p = slug + 5;
    for (int i = 0; i < 8; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
    }
    if (p[8] != '-') return false;
    for (int i = 9; i < 15; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
    }

    out.year = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
    out.month = (p[4] - '0') * 10 + (p[5] - '0');
    out.day = (p[6] - '0') * 10 + (p[7] - '0');
    out.hour = (p[9] - '0') * 10 + (p[10] - '0');
    out.minute = (p[11] - '0') * 10 + (p[12] - '0');
    out.second = (p[13] - '0') * 10 + (p[14] - '0');

    // Год 0 — признак «часы не были выставлены» (`chat-00000000-005946`).
    // Такое имя валидно, просто даты в нём нет, поэтому проверки диапазонов
    // для него не применяются: 00-й месяц — это не ошибка файла, а отметка
    // «времени не было».
    if (out.year == 0) {
        out.real = false;
        return true;
    }

    // Ограничения на диапазоны, а не только на цифры: «chat-20261324-120000»
    // разбирается по цифрам, но показать его в списке как дату нельзя.
    if (out.month < 1 || out.month > 12) return false;
    if (out.day < 1 || out.day > 31) return false;
    if (out.hour > 23 || out.minute > 59 || out.second > 59) return false;

    out.real = true;
    return true;
}

size_t format_stamp(const Stamp& stamp, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return 0;

    if (!stamp.real) {
        // Прочерк, а не «00.00 00:00»: последнее читается как настоящая дата
        // и заставляет искать ошибку в часах, которой нет.
        const int n = snprintf(out, out_len, "—");
        return n > 0 && static_cast<size_t>(n) < out_len ? static_cast<size_t>(n) : 0;
    }

    const int n = snprintf(out, out_len, "%02d.%02d %02d:%02d", stamp.day, stamp.month, stamp.hour, stamp.minute);
    return n > 0 && static_cast<size_t>(n) < out_len ? static_cast<size_t>(n) : 0;
}

bool title_from_line(const char* line, size_t len, char* out, size_t out_len)
{
    if (out == nullptr || out_len < 2) return false;
    out[0] = '\0';
    if (line == nullptr || len == 0) return false;

    JsonDocument doc;
    if (deserializeJson(doc, line, len) != DeserializationError::Ok) return false;

    JsonObject root = doc.as<JsonObject>();
    if (root.isNull()) return false;

    // Служебная строка метаданных — не заголовок.
    if (!root["type"].isNull()) return false;

    const char* role = root["role"].as<const char*>();
    if (role == nullptr || chatline::role_from_name(role) != api::Role::User) return false;

    const char* content = root["content"] | "";
    const size_t content_len = strlen(content);
    if (content_len == 0) return false;

    // Первая строка вопроса: в списке многострочный заголовок не читается.
    size_t used = 0;
    bool broke_at_newline = false;
    while (used < content_len && used + 1 < out_len) {
        const char c = content[used];
        if (c == '\n' || c == '\r') {
            broke_at_newline = true;
            break;
        }
        out[used] = c;
        ++used;
    }
    out[used] = '\0';

    // Многоточие нужно только если текст не поместился. Обрыв по переводу
    // строки — это конец логической строки, а не обрезка: там многоточие
    // врало бы («первая строка…», хотя она закончилась).
    if (!broke_at_newline && used < content_len) {
        // Кириллица занимает два байта: обрезка посреди пары дала бы рамку.
        used = trim_utf8(out, used);
        out[used] = '\0';
        if (used + 3 < out_len) {
            memcpy(out + used, "…", 3);
            out[used + 3] = '\0';
        }
    }

    return out[0] != '\0';
}

}  // namespace chatlist
