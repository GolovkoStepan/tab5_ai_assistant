// Веб-поиск RouterAI: поле `plugins` и разбор источников. См. util/web_sources.h
// и Р-036 в docs/DECISIONS.md.
//
// Обратите внимание на две неочевидные вещи:
//
//  1. **Мы не проверяем, что пришёл именно `url_citation`.** В разведке
//     `type` был только таким, но фильтровать по нему значило бы терять
//     источники, если RouterAI добавит второй тип. Берём `url_citation`,
//     а если его нет — пробуем верхний уровень объекта: поле `url` там тоже
//     лежит у некоторых провайдеров.
//
//  2. **Склейка счётчиков `dropped`.** Источники приходят одним чанком, но
//     если плагин однажды начнёт слать их порциями, дубли надо считать
//     честно: иначе «dropped» молча потеряет половину.

#include "util/web_sources.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

namespace web {
namespace {

/// Копирование с обрезкой по границе UTF-8.
///
/// Обрезать русский заголовок посередине символа — значит получить в чате
/// «квадратик» в конце строки. Поэтому, если срез попал внутрь многобайтового
/// символа, отступаем назад до его начала.
size_t copy_text(char* dst, size_t cap, const char* src)
{
    if (dst == nullptr || cap == 0) return 0;
    dst[0] = '\0';
    if (src == nullptr) return 0;

    const size_t n = strlen(src);
    if (n < cap) {
        memcpy(dst, src, n + 1);
        return n;
    }

    size_t cut = cap - 1;
    // Байт-продолжение имеет старшие биты 10xxxxxx. Пока стоим на таком,
    // шагаем назад: нужно встать на начало символа.
    while (cut > 0 && (static_cast<unsigned char>(src[cut]) & 0xC0) == 0x80)
        --cut;
    memcpy(dst, src, cut);
    dst[cut] = '\0';
    return cut;
}

/// Схлопнуть переводы строк и повторяющиеся пробелы: `content` из выдачи
/// приходит многострочным, а в чате источник занимает одну строку.
size_t flatten(const char* src, char* dst, size_t cap)
{
    if (dst == nullptr || cap == 0) return 0;
    dst[0] = '\0';
    if (src == nullptr) return 0;

    size_t used = 0;
    bool space = false;
    for (const char* p = src; *p != '\0' && used + 1 < cap; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (space || used == 0) continue;
            space = true;
            dst[used++] = ' ';
            continue;
        }
        space = false;
        dst[used++] = static_cast<char>(c);
    }
    // Хвостовой пробел от последнего перевода строки не нужен.
    while (used > 0 && dst[used - 1] == ' ')
        --used;
    dst[used] = '\0';
    return used;
}

/// Один объект аннотации → Source. false — запись пустая (нет URL).
bool parse_one(JsonObject ann, Source& out)
{
    JsonObject cite = ann["url_citation"];
    if (cite.isNull()) cite = ann;  // второй возможный вид — поля на верхнем уровне

    const char* url = cite["url"] | (const char*)nullptr;
    if (url == nullptr || url[0] == '\0') return false;

    copy_text(out.url, sizeof(out.url), url);
    copy_text(out.title, sizeof(out.title), cite["title"] | (const char*)nullptr);

    // Выжимка — начало content одним абзацем.
    char flat[kMaxSnippetLen * 4];
    if (flatten(cite["content"] | (const char*)nullptr, flat, sizeof(flat)) > 0) {
        copy_text(out.snippet, sizeof(out.snippet), flat);
    }
    return true;
}

/// Уже есть такой URL? Источники приходят одним чанком, но полагаться на это
/// нельзя (Р-036, п. 3).
bool has_url(const List& list, const char* url)
{
    for (size_t i = 0; i < list.count; ++i) {
        if (strcmp(list.items[i].url, url) == 0) return true;
    }
    return false;
}

}  // namespace

size_t feed(const char* json, size_t len, List& out)
{
    if (json == nullptr || len == 0) return 0;

    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return 0;

    JsonArray choices = doc["choices"];
    if (choices.isNull() || choices.size() == 0) return 0;
    JsonObject choice = choices[0];

    // В стриминге — delta, без стриминга — message. Обе формы в одном месте.
    JsonArray ann = choice["delta"]["annotations"];
    if (ann.isNull() || ann.size() == 0) ann = choice["message"]["annotations"];
    if (ann.isNull() || ann.size() == 0) return 0;

    size_t added = 0;
    for (JsonVariant item : ann) {
        Source src;
        if (!parse_one(item.as<JsonObject>(), src)) continue;
        if (has_url(out, src.url)) {
            ++out.dropped;  // повтор того же списка — не потеря, но пусть видно
            continue;
        }
        if (out.count >= kMaxSources) {
            ++out.dropped;
            continue;
        }
        out.items[out.count++] = src;
        ++added;
    }
    return added;
}

bool mentions_annotations(const char* json, size_t len)
{
    if (json == nullptr || len == 0) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
    JsonVariant ann = doc["choices"][0]["delta"]["annotations"];
    if (ann.isNull()) ann = doc["choices"][0]["message"]["annotations"];
    return !ann.isNull();
}

size_t append(List& dst, const List& src)
{
    size_t added = 0;
    for (size_t i = 0; i < src.count; ++i) {
        const Source& s = src.items[i];
        if (has_url(dst, s.url)) continue;
        if (dst.count >= kMaxSources) {
            ++dst.dropped;
            continue;
        }
        dst.items[dst.count++] = s;
        ++added;
    }
    return added;
}

size_t build_plugins_json(uint8_t max_results, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return 0;

    // Границы: 0 сервер молча заменит своим умолчанием (проверка 4 в Р-036
    // показала, что «без параметра» = 5), а больше kMaxSources нам всё равно
    // некуда девать.
    uint8_t n = max_results;
    if (n == 0) n = 1;
    if (n > kMaxSources) n = kMaxSources;

    const int written = snprintf(out, out_len, "[{\"id\":\"web\",\"max_results\":%u}]", static_cast<unsigned>(n));
    if (written <= 0 || static_cast<size_t>(written) >= out_len) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(written);
}

const char* primary_url(const List& list)
{
    return list.count > 0 ? list.items[0].url : nullptr;
}

size_t domain_of(const char* url, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';
    if (url == nullptr) return 0;

    const char* host = url;
    if (strncmp(host, "https://", 8) == 0)
        host += 8;
    else if (strncmp(host, "http://", 7) == 0)
        host += 7;
    else if (strncmp(host, "//", 2) == 0)
        host += 2;

    // Хост кончается на '/', '?' или '#' — дальше путь.
    size_t n = 0;
    while (host[n] != '\0' && host[n] != '/' && host[n] != '?' && host[n] != '#')
        ++n;
    // Порт отбрасываем: в подписи он лишний.
    for (size_t i = 0; i < n; ++i) {
        if (host[i] == ':') {
            n = i;
            break;
        }
    }
    if (n == 0) return 0;

    // «www.» не несёт смысла и удлиняет подпись.
    if (n > 4 && strncmp(host, "www.", 4) == 0) {
        host += 4;
        n -= 4;
    }
    if (n + 1 > out_len) {
        out[0] = '\0';
        return 0;
    }
    memcpy(out, host, n);
    out[n] = '\0';
    return n;
}

size_t format_source(const Source& src, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';

    char domain[64];
    if (domain_of(src.url, domain, sizeof(domain)) == 0) {
        // URL кривой — показываем его как есть, лишь бы не потерять источник.
        return copy_text(out, out_len, src.url);
    }

    const int written = src.title[0] != '\0' ? snprintf(out, out_len, "%s — %s", domain, src.title)
                                             : snprintf(out, out_len, "%s", domain);
    if (written <= 0 || static_cast<size_t>(written) >= out_len) {
        // Не влезло целиком — отдаём хотя бы домен, он короткий.
        return copy_text(out, out_len, domain);
    }
    return static_cast<size_t>(written);
}

size_t format_list(const List& list, char* out, size_t out_len)
{
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';

    // Пустой список — пустой текст. «Не дал результатов» решает экран: он
    // знает, был ли поиск вообще.
    if (list.count == 0) return 0;

    size_t used = 0;
    const int head = snprintf(out, out_len, "\n\nИсточники:");
    if (head <= 0 || static_cast<size_t>(head) >= out_len) {
        out[0] = '\0';
        return 0;
    }
    used = static_cast<size_t>(head);

    char line[web::kMaxUrlLen < 320 ? 320 : web::kMaxUrlLen];
    for (size_t i = 0; i < list.count; ++i) {
        format_source(list.items[i], line, sizeof(line));

        // Считаем ДО записи: snprintf вернёт длину, которая поместилась бы,
        // и прибавлять её к смещению нельзя — смещение вышло бы за буфер.
        const int need = snprintf(nullptr, 0, "\n%u. %s", static_cast<unsigned>(i + 1), line);
        if (need <= 0 || used + static_cast<size_t>(need) >= out_len) {
            // Обрезка посередине строки читалась бы как «источники кончились».
            // Обрываем на границе пункта и ставим многоточие, если оно влезает.
            const char kEllipsis[] = "\n…";  // «\n» + три байта UTF-8 + ноль
            const size_t kEllipsisLen = sizeof(kEllipsis) - 1;
            if (used + kEllipsisLen < out_len) {
                memcpy(out + used, kEllipsis, kEllipsisLen + 1);
                used += kEllipsisLen;
            }
            out[used] = '\0';
            return used;
        }

        snprintf(out + used, out_len - used, "\n%u. %s", static_cast<unsigned>(i + 1), line);
        used += static_cast<size_t>(need);
    }

    out[used] = '\0';
    return used;
}

}  // namespace web
